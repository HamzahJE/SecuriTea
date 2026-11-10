#pragma once
// ir_engine module: the IR hardware, the LED driver and the receiver.
//
// Pure hardware. Nothing here writes shared state or the status line; the caller reports
// what happened. The library objects live inside ir_engine.cpp, so this header is the only
// way the rest of the program reaches the IR hardware. Called from irTask only (and
// irEngineBegin() from setup()).

#include "app_state.h"

// Configures the LED pin. Call once from setup().
void irEngineBegin();

// ---- Transmit ----------------------------------------------------------------------------
enum class IrSendResult
{
    Sent,
    UnsupportedProtocol, // a parsed command this firmware cannot encode
    EmptyRaw             // a raw command with no timings
};

// Sends the payload once (parsed protocols repeat the frame t.frameRepeat extra times,
// Sony at least three). Blocks for the length of the signal.
IrSendResult irSend(const IrPayload &p, const IrTiming &t);

// ---- Receive (learn mode) ----------------------------------------------------------------
// Starts the receiver. Allocates a hardware timer, so call it once per listening session;
// use irListenRearm() to capture again, and irListenStop() to free the timer.
void irListenStart();
void irListenRearm();
void irListenStop();

enum class IrCaptureResult
{
    Nothing,  // no complete signal yet
    Empty,    // a signal was decoded but held no usable timings
    Captured  // `out` holds the signal; protocol and detail describe it
};

// Polls the receiver once. On Captured, `out` holds the raw timings in the form the .ir
// loader accepts, `protocol` the decoded protocol name and `detail` a short description.
IrCaptureResult irCapture(IrPayload &out, String &protocol, String &detail);
