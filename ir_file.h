#pragma once
// Flipper .ir file parsing and validation. Pure functions: no globals, no locks, no status
// line. Callers hold the SD lock and decide what to tell the user.
//
// File format (one command per block):
//   name: Power
//   type: raw | parsed
//   frequency: 38000            (raw, Hz)
//   duty_cycle: 0.33            (raw, ignored)
//   data: 9000 4500 560 ...     (raw, microseconds, mark space mark ...)
//   protocol: NEC               (parsed)
//   address: 04 00 00 00        (parsed, little-endian bytes)
//   command: 08 00 00 00        (parsed)

#include <Arduino.h>
#include <FS.h>
#include "app_state.h"

// Reads one line into buf without the newline, leading and trailing whitespace removed.
// Returns false at end of file with nothing read. If the line is longer than cap-1 bytes,
// buf holds the first cap-1 bytes, truncated is set and the rest stays unread in the file
// so the caller can either stream it (parseRawValues) or drop it (skipRestOfLine).
bool readLineBounded(File &file, char *buf, size_t cap, bool &truncated);
void skipRestOfLine(File &file);

// Copies the current line (head already read, the rest still in src if more) to dst with
// a single '\n'. Adds the bytes written to bytesWritten. Returns false on a short write.
bool copyLine(File &src, File &dst, const char *head, bool more, size_t &bytesWritten);

bool startsWith(const char *line, const char *prefix);
const char *skipSpaces(const char *s);
const char *baseName(const char *path);
bool endsWithIgnoreCase(const char *s, const char *suffix);

// "04 00 00 00" -> 0x00000004. Rejects anything that is not 1-4 hex bytes.
bool parseFlipperHex32(const char *field, uint32_t &value);

// "38000" (Hz) or "38" (kHz) -> 38. Rejects carriers outside kMinCarrierKHz..kMaxCarrierKHz.
bool parseCarrierHz(const char *field, uint16_t &freqKHz);

// Parses the values of a "data:" line into out.raw. head is what the line buffer already
// holds after "data:"; more says the file still holds the rest of the line. Each pulse must
// be 1..kMaxPulseUs, the total at most kMaxSignalUs, the count at most kMaxRawBuffer.
// On failure err names the reason and the rest of the line has been consumed.
bool parseRawValues(File &file, const char *head, bool more, IrPayload &out, const char *&err);

// Parses one complete command starting at the file's current position (a "name:" line).
// Stops at the end of the command. On failure err names the reason.
bool parseCommandAt(File &file, IrPayload &out, const char *&err);
