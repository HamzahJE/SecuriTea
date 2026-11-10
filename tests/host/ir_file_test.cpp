#include "ir_file.h"
#include <cstdio>
#include <vector>
#include <random>
unsigned long g_delayCalls = 0;
static int g_fail = 0, g_pass = 0;
#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static bool parse(const std::string &text, IrPayload &p, const char *&err, size_t *endPos = nullptr) {
    std::string b = text; File f(&b); bool ok = parseCommandAt(f, p, err); if (endPos) *endPos = f.position(); return ok;
}
static std::string rawCmd(const std::string &data, const char *freq = "38000") {
    return std::string("name: Power\ntype: raw\nfrequency: ") + freq + "\nduty_cycle: 0.33\ndata: " + data + "\n";
}
static std::string values(int n, int v) { std::string s; for (int i = 0; i < n; i++) { s += std::to_string(v); s += ' '; } return s; }

int main() {
    char line[256]; bool trunc = false;
    { // ---- bounded line reader
        std::string b = "abc\r\n   def \t\n\nlast"; File f(&b);
        CHECK(readLineBounded(f, line, sizeof line, trunc) && !strcmp(line, "abc") && !trunc);
        CHECK(readLineBounded(f, line, sizeof line, trunc) && !strcmp(line, "def"));
        CHECK(readLineBounded(f, line, sizeof line, trunc) && !strcmp(line, ""));
        CHECK(readLineBounded(f, line, sizeof line, trunc) && !strcmp(line, "last"));
        CHECK(!readLineBounded(f, line, sizeof line, trunc));
    }
    { // long line: truncated, rest left in the file, next line intact
        std::string b = std::string(600, 'x') + "\nnext\n"; File f(&b);
        CHECK(readLineBounded(f, line, sizeof line, trunc) && trunc && strlen(line) == 255);
        skipRestOfLine(f);
        CHECK(readLineBounded(f, line, sizeof line, trunc) && !strcmp(line, "next") && !trunc);
    }
    { // a line of exactly cap-1 chars is not "truncated", and its newline is consumed
        std::string b = std::string(255, 'y') + "\nz\n"; File f(&b);
        CHECK(readLineBounded(f, line, sizeof line, trunc) && !trunc && strlen(line) == 255);
        CHECK(readLineBounded(f, line, sizeof line, trunc) && !strcmp(line, "z"));
    }
    { char small[4]; std::string b = "abcdefg\nq"; File f(&b); // tiny buffer never overflows
        CHECK(readLineBounded(f, small, sizeof small, trunc) && trunc && !strcmp(small, "abc")); }

    IrPayload p; const char *err = nullptr; size_t end = 0;
    // ---- raw commands
    CHECK(parse(rawCmd("9000 4500 560 560 560"), p, err) && p.rawLen == 5 && p.raw[0] == 9000 && p.raw[4] == 560 && p.freqKHz == 38 && !p.isParsed && p.btnName == "Power");
    CHECK(parse(rawCmd(values(500, 600)), p, err) && p.rawLen == 500);                 // long data line streams past 256
    CHECK(!parse(rawCmd(values(501, 600)), p, err) && !strcmp(err, "Signal too long"));
    CHECK(!parse(rawCmd("9000 0 560"), p, err) && !strcmp(err, "Pulse of 0 us"));
    CHECK(parse(rawCmd("9000 40000 560"), p, err) && p.raw[1] == 40000);                // repeat gap: accepted as is
    CHECK(parse(rawCmd("20000 20000 1"), p, err) && p.rawLen == 3);
    CHECK(parse(rawCmd("70000 560 1"), p, err) && p.raw[0] == 65535);                   // over the limit: shortened
    CHECK(parse(rawCmd("560 100000 560"), p, err) && p.raw[1] == 65535);                // never wrapped, never refused
    CHECK(!parse(rawCmd("99999999999 560"), p, err) && !strcmp(err, "Bad raw value"));
    CHECK(!parse(rawCmd(values(201, 20000)), p, err) && !strcmp(err, "Signal over 4 s")); // 4.02 s
    CHECK(parse(rawCmd(values(200, 20000)), p, err) && p.rawLen == 200);               // exactly 4 s is allowed
    CHECK(!parse(rawCmd("560 -560 560"), p, err) && !strcmp(err, "Bad raw value"));
    CHECK(!parse(rawCmd("560 abc"), p, err));
    CHECK(!parse(rawCmd(""), p, err) && !strcmp(err, "Empty raw data"));
    CHECK(parse(rawCmd("560 560 560\r"), p, err) && p.rawLen == 3);                    // CRLF file
    CHECK(!parse(rawCmd("560 560", "1"), p, err) && !strcmp(err, "Carrier out of range"));
    CHECK(!parse(rawCmd("560 560", "10000000"), p, err));
    CHECK(parse(rawCmd("560 560 1", "56000"), p, err) && p.freqKHz == 56);
    CHECK(parse(rawCmd("560 560 1", "10000"), p, err) && p.freqKHz == 10);
    CHECK(!parse(rawCmd("560 560 1", "9999"), p, err));
    CHECK(!parse("name: A\ndata: 560 560\n", p, err) && !strcmp(err, "data without type: raw"));
    CHECK(!parse("type: raw\ndata: 1 2 3\n", p, err) && !strcmp(err, "Bad command offset"));
    // after a rejected long data line the reader sits at the start of the next line
    { std::string t = rawCmd(values(600, 600)) + "name: Next\n"; CHECK(!parse(t, p, err, &end));
      std::string b = t; File f(&b); f.seek(end); CHECK(readLineBounded(f, line, sizeof line, trunc) && !strcmp(line, "name: Next")); }
    // ---- parsed commands
    CHECK(parse("name: Vol_up\ntype: parsed\nprotocol: NEC\naddress: 04 00 00 00\ncommand: 08 00 00 00\n", p, err) && p.isParsed && p.address == 4 && p.command == 8 && p.protocol == "NEC");
    CHECK(parse("name: X\ntype: parsed\nprotocol: NECext\naddress: 86 FF 00 00\ncommand: 1B E4 00 00\n", p, err) && p.address == 0xFF86 && p.command == 0xE41B);
    CHECK(!parse("name: X\ntype: parsed\nprotocol: NEC\naddress: ZZ 00\ncommand: 08\n", p, err) && !strcmp(err, "Bad address"));
    CHECK(!parse("name: X\ntype: parsed\nprotocol: NEC\naddress: 04\ncommand: 1FF\n", p, err) && !strcmp(err, "Bad command"));
    CHECK(!parse("name: X\ntype: parsed\naddress: 01 02 03 04 05\ncommand: 08\n", p, err));
    CHECK(!parse("name: X\ntype: raw\ncommand: 08 00\n", p, err) && !strcmp(err, "command without type: parsed"));
    CHECK(!parse("name: X\ntype: weird\n", p, err) && !strcmp(err, "Unknown type"));
    CHECK(!parse("name: X\nname: Y\ntype: raw\ndata: 1 2 3\n", p, err) && !strcmp(err, "Command has no type"));
    CHECK(!parse("name: X\ntype: raw\n", p, err) && !strcmp(err, "Command incomplete"));
    CHECK(!parse("", p, err));
    { uint32_t v; CHECK(parseFlipperHex32(" 0a", v) && v == 10); CHECK(!parseFlipperHex32("", v)); CHECK(!parseFlipperHex32("0x", v)); CHECK(!parseFlipperHex32("12 3G", v)); }
    // ---- copyLine: byte count matches what landed, CR dropped, short write detected
    { std::string src = "tail of line\r\nnext\n", dst; File s(&src), d(&dst); size_t n = 0;
      CHECK(copyLine(s, d, "head ", true, n) && dst == "head tail of line\n" && n == dst.size());
      CHECK(readLineBounded(s, line, sizeof line, trunc) && !strcmp(line, "next")); }
    { std::string src = "", dst; File s(&src), d(&dst); size_t n = 0; d.failAfter(3);
      CHECK(!copyLine(s, d, "abcdef", false, n)); }
    { std::string src = "abcdef\n", dst; File s(&src), d(&dst); size_t n = 0; d.failAfter(4);
      CHECK(!copyLine(s, d, "ab", true, n)); }
    // ---- a multi-megabyte line still yields to the idle task
    { std::string b(3 * 1024 * 1024, 'x'); File f(&b); g_delayCalls = 0; skipRestOfLine(f); CHECK(g_delayCalls >= 700); }

    // ---- fuzz: random token soup must never crash, overflow or accept an out-of-range signal
    const char *tok[] = {"name:", "type:", " raw", " parsed", "frequency:", "protocol:", "address:", "command:", "data:", "duty_cycle:",
                         " ", "\n", "\r\n", "0", "1", "560", "20000", "20001", "65536", "99999999", "38000", "FF", "zz", "-", "#", "\t", "NEC", "\0x"};
    std::mt19937 rng(12345); long accepted = 0, total = 0;
    for (int iter = 0; iter < 60000; iter++) {
        std::string b; int n = 1 + rng() % 400;
        for (int i = 0; i < n; i++) { if (rng() % 9 == 0) b += (char)(rng() % 256); else b += tok[rng() % (sizeof tok / sizeof *tok)]; }
        if (rng() % 4 == 0) b = "name: f\ntype: raw\nfrequency: 38000\ndata: " + b;
        File f(&b); size_t guard = 0;
        while (guard++ < 2000) {
            size_t start = f.position(); bool tr = false; char ln[256];
            if (!readLineBounded(f, ln, sizeof ln, tr)) break;
            if (tr) skipRestOfLine(f);
            if (!startsWith(ln, "name:")) continue;
            size_t after = f.position(); f.seek(start);
            IrPayload q; const char *e = nullptr; total++;
            if (parseCommandAt(f, q, e)) {
                accepted++;
                if (q.isParsed) { /* address and command are plain 32-bit values */ }
                else { uint32_t sum = 0; CHECK(q.rawLen >= 1 && q.rawLen <= kMaxRawBuffer);
                       for (int i = 0; i < q.rawLen; i++) { CHECK(q.raw[i] >= 1 && q.raw[i] <= kMaxPulseUs); sum += q.raw[i]; }
                       CHECK(sum <= kMaxSignalUs); CHECK(q.freqKHz >= kMinCarrierKHz && q.freqKHz <= kMaxCarrierKHz); }
            } else { CHECK(e != nullptr); }
            f.seek(after);
        }
    }
    printf("fuzz: %ld commands parsed, %ld accepted\n", total, accepted);
    printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
