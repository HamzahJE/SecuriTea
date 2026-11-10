#include "ir_engine.h"

#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>
#include "board_config.h"

namespace
{
constexpr uint16_t kCaptureBufferSize = 1024;
constexpr uint8_t kCaptureTimeoutMs = 50; // a gap this long ends a capture

IRrecv g_receiver(board::kIrRecvPin, kCaptureBufferSize, kCaptureTimeoutMs, true);
decode_results g_decoded;
IRsend g_sender(board::kIrLedPin);

// NEC and Samsung put the least significant bit on air first, and a Flipper .ir file stores the
// fields the same way. The IR library's senders shift out the MOST significant bit of their
// argument first. So each frame is assembled in on-air order (first byte in the low bits) and
// then reversed as a whole 32-bit value. Check: NEC address 0x04 command 0x08 must come out as
// 0x20DF10EF, the LG TV power code.
uint32_t necStandardFrame(uint8_t addr8, uint8_t cmd8)
{
    uint32_t onAir = ((uint32_t)addr8) | ((uint32_t)(addr8 ^ 0xFF) << 8) | ((uint32_t)cmd8 << 16) |
                     ((uint32_t)(cmd8 ^ 0xFF) << 24);
    return (uint32_t)reverseBits(onAir, 32);
}

uint32_t necExtendedFrame(uint16_t addr16, uint16_t cmd16)
{
    uint32_t onAir = (uint32_t)addr16 | ((uint32_t)cmd16 << 16);
    return (uint32_t)reverseBits(onAir, 32);
}

// Samsung32 repeats the address byte (it does not invert it), then command and inverted command.
uint32_t samsung32Frame(uint8_t addr8, uint8_t cmd8)
{
    uint32_t onAir = ((uint32_t)addr8) | ((uint32_t)addr8 << 8) | ((uint32_t)cmd8 << 16) |
                     ((uint32_t)(cmd8 ^ 0xFF) << 24);
    return (uint32_t)reverseBits(onAir, 32);
}

// Returns false for a protocol this firmware cannot encode.
bool sendParsed(const IrPayload &p, const IrTiming &t)
{
    String proto = p.protocol;
    proto.toUpperCase();

    uint8_t addr8 = (uint8_t)(p.address & 0xFF);
    uint8_t cmd8 = (uint8_t)(p.command & 0xFF);
    uint16_t addr16 = (uint16_t)(p.address & 0xFFFF);
    uint16_t cmd16 = (uint16_t)(p.command & 0xFFFF);

    if (proto == "SAMSUNG32" || proto == "SAMSUNG")
    {
        g_sender.sendSAMSUNG(samsung32Frame(addr8, cmd8), 32, t.frameRepeat);
    }
    else if (proto == "NECEXT")
    {
        g_sender.sendNEC(necExtendedFrame(addr16, cmd16), 32, t.frameRepeat);
        if (t.aggressive)
        {
            delay(25);
            g_sender.sendNEC(necStandardFrame(addr8, cmd8), 32, t.frameRepeat);
        }
    }
    else if (proto == "NEC")
    {
        g_sender.sendNEC(necStandardFrame(addr8, cmd8), 32, t.frameRepeat);
        if (t.aggressive)
        {
            delay(25);
            g_sender.sendNEC(necExtendedFrame(addr16, cmd16), 32, t.frameRepeat);
        }
    }
    else if (proto == "RC5")
    {
        uint16_t data = (uint16_t)(((addr8 & 0x1F) << 6) | (cmd8 & 0x3F));
        g_sender.sendRC5(data, 12, t.frameRepeat);
    }
    else if (proto == "RC6")
    {
        uint32_t data = ((uint32_t)addr8 << 8) | (uint32_t)cmd8;
        g_sender.sendRC6(data, 20, t.frameRepeat);
    }
    else if (proto == "SIRC" || proto == "SONY")
    {
        // encodeSony() does the bit reversal. Sony receivers need the frame at least 3 times.
        uint16_t repeats = t.frameRepeat > kSonyMinRepeat ? t.frameRepeat : kSonyMinRepeat;
        g_sender.sendSony(g_sender.encodeSony(12, cmd8, addr8), 12, repeats);
    }
    else
    {
        return false;
    }
    return true;
}
} // namespace

void irEngineBegin()
{
    g_sender.begin();
}

IrSendResult irSend(const IrPayload &p, const IrTiming &t)
{
    if (p.isParsed)
    {
        return sendParsed(p, t) ? IrSendResult::Sent : IrSendResult::UnsupportedProtocol;
    }
    if (p.rawLen == 0)
    {
        return IrSendResult::EmptyRaw;
    }
    g_sender.sendRaw(p.raw, p.rawLen, p.freqKHz);
    return IrSendResult::Sent;
}

void irListenStart()
{
    g_receiver.enableIRIn();
}

void irListenRearm()
{
    g_receiver.resume();
}

void irListenStop()
{
    g_receiver.disableIRIn();
}

IrCaptureResult irCapture(IrPayload &out, String &protocol, String &detail)
{
    if (!g_receiver.decode(&g_decoded))
    {
        return IrCaptureResult::Nothing;
    }

    // rawbuf[0] is the gap before the signal, so the signal is entries 1..rawlen-1, in ticks of
    // kRawTick us, alternating mark, space, mark... Stop at anything the .ir loader would not
    // load back (a 0, or the total going past kMaxSignalUs), so every saved capture replays.
    uint16_t localLen = 0;
    uint32_t totalUs = 0;
    for (uint16_t i = 1; i < g_decoded.rawlen && localLen < kMaxRawBuffer; i++)
    {
        uint32_t us = (uint32_t)g_decoded.rawbuf[i] * kRawTick;
        if (us == 0 || us > kMaxPulseUs || totalUs + us > kMaxSignalUs)
        {
            break;
        }
        totalUs += us;
        out.raw[localLen++] = (uint16_t)us;
    }
    if (localLen % 2 == 0 && localLen > 0)
    {
        localLen--; // a signal ends on a mark; drop a trailing space
    }
    out.rawLen = localLen;
    out.freqKHz = kDefaultCarrierKHz;
    out.isParsed = false;
    out.btnName = "Captured";

    protocol = typeToString(g_decoded.decode_type, false);
    if (protocol.length() == 0)
    {
        protocol = "UNKNOWN";
    }
    protocol.toUpperCase();

    detail = "Bits:" + String(g_decoded.bits);
    if (g_decoded.address != 0 || g_decoded.command != 0)
    {
        String addrHex = String((uint32_t)g_decoded.address, HEX);
        String cmdHex = String((uint32_t)g_decoded.command, HEX);
        addrHex.toUpperCase();
        cmdHex.toUpperCase();
        detail = "A:0x" + addrHex + " C:0x" + cmdHex;
    }
    else if (g_decoded.value != 0)
    {
        String valueHex = uint64ToString(g_decoded.value, 16);
        valueHex.toUpperCase();
        detail = "V:0x" + valueHex + " B:" + String(g_decoded.bits);
    }

    Serial.println("=== LEARN CAPTURE ===");
    Serial.println(resultToHumanReadableBasic(&g_decoded));
    Serial.println(resultToSourceCode(&g_decoded));

    g_receiver.resume();
    return localLen == 0 ? IrCaptureResult::Empty : IrCaptureResult::Captured;
}
