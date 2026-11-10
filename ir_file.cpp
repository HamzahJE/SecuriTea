#include "ir_file.h"

#include <string.h>
#include <stdlib.h>

namespace
{
bool isBlank(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

// Accumulates digits into a small token and validates each value as it completes.
class RawTokenizer
{
public:
    explicit RawTokenizer(IrPayload &out) : out_(out)
    {
    }

    bool feed(char c)
    {
        if (c >= '0' && c <= '9')
        {
            if (tokLen_ >= sizeof(tok_) - 1)
            {
                err_ = "Bad raw value";
                return false;
            }
            tok_[tokLen_++] = c;
            return true;
        }
        if (isBlank(c))
        {
            return flush();
        }
        err_ = "Bad raw value";
        return false;
    }

    bool flush()
    {
        if (tokLen_ == 0)
        {
            return true;
        }
        tok_[tokLen_] = '\0';
        tokLen_ = 0;
        unsigned long value = strtoul(tok_, nullptr, 10);
        if (value == 0 || value > kMaxPulseUs)
        {
            err_ = "Pulse out of range";
            return false;
        }
        if (out_.rawLen >= kMaxRawBuffer)
        {
            err_ = "Signal too long";
            return false;
        }
        totalUs_ += value;
        if (totalUs_ > kMaxSignalUs)
        {
            err_ = "Signal over 2 s";
            return false;
        }
        out_.raw[out_.rawLen++] = (uint16_t)value;
        return true;
    }

    const char *error() const
    {
        return err_;
    }

private:
    IrPayload &out_;
    uint32_t totalUs_ = 0;
    char tok_[8] = {};
    size_t tokLen_ = 0;
    const char *err_ = nullptr;
};
} // namespace

bool readLineBounded(File &file, char *buf, size_t cap, bool &truncated)
{
    truncated = false;
    size_t len = 0;
    bool gotAny = false;

    while (true)
    {
        if (len + 1 >= cap)
        {
            int next = file.peek();
            if (next == '\n')
            {
                file.read();
            }
            else if (next >= 0)
            {
                truncated = true;
            }
            break;
        }
        int c = file.read();
        if (c < 0)
        {
            break;
        }
        gotAny = true;
        if (c == '\n')
        {
            break;
        }
        buf[len++] = (char)c;
    }

    size_t start = 0;
    while (start < len && isBlank(buf[start]))
    {
        start++;
    }
    if (start > 0)
    {
        memmove(buf, buf + start, len - start);
        len -= start;
    }
    // A truncated line may be cut mid-token; keep its tail intact so streaming can continue it.
    if (!truncated)
    {
        while (len > 0 && isBlank(buf[len - 1]))
        {
            len--;
        }
    }
    buf[len] = '\0';
    return gotAny;
}

// A line can be megabytes long in a hostile file. Reading it must not starve the idle task
// (which feeds the watchdog), so every loop that follows one line to its end pauses for one
// tick each kLineBreatherBytes.
constexpr size_t kLineBreatherBytes = 4096;

static void lineBreather(size_t &bytes)
{
    if (++bytes % kLineBreatherBytes == 0)
    {
        vTaskDelay(1);
    }
}

void skipRestOfLine(File &file)
{
    size_t bytes = 0;
    int c;
    do
    {
        c = file.read();
        lineBreather(bytes);
    } while (c >= 0 && c != '\n');
}

bool copyLine(File &src, File &dst, const char *head, bool more, size_t &bytesWritten)
{
    size_t headLen = strlen(head);
    size_t n = dst.print(head);
    bytesWritten += n;
    if (n != headLen)
    {
        return false;
    }
    if (more)
    {
        size_t bytes = 0;
        int c;
        while ((c = src.read()) >= 0 && c != '\n')
        {
            lineBreather(bytes);
            if (c == '\r')
            {
                continue;
            }
            if (dst.write((uint8_t)c) != 1)
            {
                return false;
            }
            bytesWritten++;
        }
    }
    if (dst.write((uint8_t)'\n') != 1)
    {
        return false;
    }
    bytesWritten++;
    return true;
}

bool startsWith(const char *line, const char *prefix)
{
    return strncmp(line, prefix, strlen(prefix)) == 0;
}

const char *skipSpaces(const char *s)
{
    while (*s == ' ' || *s == '\t')
    {
        s++;
    }
    return s;
}

const char *baseName(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

bool endsWithIgnoreCase(const char *s, const char *suffix)
{
    size_t n = strlen(s);
    size_t m = strlen(suffix);
    return n >= m && strcasecmp(s + n - m, suffix) == 0;
}

bool parseFlipperHex32(const char *field, uint32_t &value)
{
    value = 0;
    const char *p = skipSpaces(field);
    int byteIndex = 0;
    while (*p != '\0')
    {
        if (byteIndex >= 4)
        {
            return false;
        }
        char *end = nullptr;
        unsigned long byte = strtoul(p, &end, 16);
        if (end == p || byte > 0xFF || !(*end == '\0' || isBlank(*end)))
        {
            return false;
        }
        value |= (uint32_t)byte << (8 * byteIndex);
        byteIndex++;
        p = skipSpaces(end);
    }
    return byteIndex > 0;
}

bool parseCarrierHz(const char *field, uint16_t &freqKHz)
{
    const char *p = skipSpaces(field);
    char *end = nullptr;
    unsigned long v = strtoul(p, &end, 10);
    if (end == p || !(*end == '\0' || isBlank(*end)))
    {
        return false;
    }
    if (v >= 1000)
    {
        v /= 1000;
    }
    if (v < kMinCarrierKHz || v > kMaxCarrierKHz)
    {
        return false;
    }
    freqKHz = (uint16_t)v;
    return true;
}

bool parseRawValues(File &file, const char *head, bool more, IrPayload &out, const char *&err)
{
    out.rawLen = 0;
    RawTokenizer tokens(out);

    for (const char *p = head; *p != '\0'; p++)
    {
        if (!tokens.feed(*p))
        {
            err = tokens.error();
            if (more)
            {
                skipRestOfLine(file);
            }
            return false;
        }
    }

    if (more)
    {
        size_t bytes = 0;
        int c;
        while ((c = file.read()) >= 0 && c != '\n')
        {
            lineBreather(bytes);
            if (!tokens.feed((char)c))
            {
                err = tokens.error();
                skipRestOfLine(file);
                return false;
            }
        }
    }

    if (!tokens.flush())
    {
        err = tokens.error();
        return false;
    }
    if (out.rawLen == 0)
    {
        err = "Empty raw data";
        return false;
    }
    return true;
}

bool parseCommandAt(File &file, IrPayload &out, const char *&err)
{
    enum class Type
    {
        Unknown,
        Raw,
        Parsed
    };

    err = nullptr;
    out.rawLen = 0;
    out.freqKHz = kDefaultCarrierKHz;
    out.isParsed = false;
    out.protocol = "";
    out.address = 0;
    out.command = 0;

    char line[kMaxLineLen];
    bool truncated = false;

    if (!readLineBounded(file, line, sizeof(line), truncated) || !startsWith(line, "name:"))
    {
        err = "Bad command offset";
        return false;
    }
    if (truncated)
    {
        skipRestOfLine(file);
    }
    out.btnName = skipSpaces(line + 5);

    Type type = Type::Unknown;
    while (readLineBounded(file, line, sizeof(line), truncated))
    {
        if (startsWith(line, "name:"))
        {
            break; // next command: this one ended without data/command
        }
        if (startsWith(line, "data:"))
        {
            if (type != Type::Raw)
            {
                if (truncated)
                {
                    skipRestOfLine(file);
                }
                err = "data without type: raw";
                return false;
            }
            if (!parseRawValues(file, skipSpaces(line + 5), truncated, out, err))
            {
                return false;
            }
            out.isParsed = false;
            return true;
        }
        if (truncated)
        {
            skipRestOfLine(file);
        }

        if (startsWith(line, "type:"))
        {
            const char *v = skipSpaces(line + 5);
            if (startsWith(v, "raw"))
            {
                type = Type::Raw;
            }
            else if (startsWith(v, "parsed"))
            {
                type = Type::Parsed;
            }
            else
            {
                err = "Unknown type";
                return false;
            }
        }
        else if (startsWith(line, "frequency:"))
        {
            if (!parseCarrierHz(line + 10, out.freqKHz))
            {
                err = "Carrier out of range";
                return false;
            }
        }
        else if (startsWith(line, "protocol:"))
        {
            out.protocol = skipSpaces(line + 9);
        }
        else if (startsWith(line, "address:"))
        {
            if (!parseFlipperHex32(line + 8, out.address))
            {
                err = "Bad address";
                return false;
            }
        }
        else if (startsWith(line, "command:"))
        {
            if (type != Type::Parsed)
            {
                err = "command without type: parsed";
                return false;
            }
            if (!parseFlipperHex32(line + 8, out.command))
            {
                err = "Bad command";
                return false;
            }
            out.isParsed = true;
            return true;
        }
        // duty_cycle, comments and blank lines are ignored.
    }

    err = (type == Type::Unknown) ? "Command has no type" : "Command incomplete";
    return false;
}
