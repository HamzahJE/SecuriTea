#pragma once
// Shared types, limits and tunables for the SecuriTea IR app.
//
// Every type that appears in a function signature of a .ino file lives here: the Arduino
// builder generates prototypes for .ino functions and places them before the sketch body,
// so the types those prototypes name must already be declared by a header.

#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

// ---- Capacity limits -----------------------------------------------------------------
constexpr int kMaxRawBuffer = 500;        // pulses per raw signal
constexpr int kMaxCommandsPerFile = 1000; // approved cap; longer files are truncated with a notice
constexpr int kMaxUnivItems = 60;         // distinct groups in a universal profile
constexpr int kMaxDirItems = 260;         // entries per directory listing
constexpr int kDirNamePoolBytes = 6144;   // bytes of file names cached per directory
constexpr int kMaxPathLen = 96;           // fixed-size path copies carried in queue messages
constexpr int kMaxNameLen = 32;           // fixed-size name copies carried in queue messages
constexpr int kMaxLineLen = 256;          // header lines of a .ir file; "data:" lines stream past this

// ---- .ir validation (approved behaviour) --------------------------------------------
constexpr uint32_t kMaxPulseUs = 20000;    // a single mark or space longer than this is rejected
constexpr uint32_t kMaxSignalUs = 2000000; // a raw signal longer than 2 s is rejected
constexpr uint16_t kMinCarrierKHz = 10;
constexpr uint16_t kMaxCarrierKHz = 56;
constexpr uint16_t kDefaultCarrierKHz = 38;

// ---- UI tunables ---------------------------------------------------------------------
constexpr int kJoyLowThreshold = 1000;   // ADC counts; below = stick pushed one way
constexpr int kJoyHighThreshold = 3000;  // ADC counts; above = stick pushed the other way
constexpr unsigned long kJoyRepeatMs = 250;
constexpr unsigned long kUiFrameMs = 33;
constexpr unsigned long kUiHeartbeatMs = 250;   // redraw at least this often; 33 = every frame (original),
                                                // 250 = only on change plus a slow heartbeat (step 1 of the
                                                // timing changes, under board test since 2026-11-09)
constexpr unsigned long kIdleDisplayOffMs = 180000;
constexpr unsigned long kSdPowerSettleMs = 1000; // card power-up to the mount, the original firmware's value
constexpr unsigned long kSdLateMountRetryMs = 3000; // if the boot mount failed: retry on demand, this often at most
constexpr unsigned long kSdOpenRetryDelayMs = 30;   // pause before the single second try of a failed open
constexpr unsigned long kIrPollMs = 15;          // irTask wakes this often, the original firmware's value
constexpr bool kSerialDiagnostics = false;       // true prints a [diag] memory line every kDiagnosticsIntervalMs
constexpr unsigned long kDeleteConfirmWindowMs = 3000;
constexpr unsigned long kDiagnosticsIntervalMs = 5000;
constexpr int kScanBreatherLines = 64;         // vTaskDelay(1) every N lines of an SD scan
constexpr int kMenuVisibleRows = 3;

// ---- Paths (Flipper SD layout) ------------------------------------------------------
constexpr const char *kDeleteJournalPath = "/.delete_journal";
constexpr const char *kLearnedDir = "/captures";
constexpr const char *kLearnedFilePath = "/captures/learned.ir";

// ---- Enums ---------------------------------------------------------------------------
enum class AppState
{
    MainMenu,
    UniversalMenu,
    UniversalRemote,
    Learn,
    FileBrowser,
    RemoteView
};

enum class LearnPhase
{
    Idle,
    Listening,
    Captured,
    Saving,
    Saved,
    Error
};

enum class RemoteSendMode
{
    One,
    Loop,
    Delete
};

enum class UniversalMode
{
    Single,
    Aggressive
};

enum class IrCommandType
{
    RemoteSendOne,
    RemoteSendAll,
    RemoteDelete,
    UniversalSend,
    LearnStart,
    LearnStop,
    LearnSave
};

enum class IrEventType
{
    DeleteResult
};

// ---- Messages between tasks ----------------------------------------------------------
// Queues copy structs byte for byte, so nothing in a message may be a String or a pointer:
// that would hand the receiver a pointer into the sender's heap.

// Send timing for one job. Captured when the command is queued so that toggling the mode
// on the UI mid-job cannot change a running job.
struct IrTiming
{
    uint16_t sendDelayMs;
    uint16_t repeatGapMs;
    uint8_t transmitRepeats;
    uint8_t loadRetries;
    uint8_t frameRepeat;
    bool aggressive;
};

// uiTask -> irTask. Self-contained: everything irTask needs is captured at press time.
struct IrCommand
{
    IrCommandType type;
    int arg;                  // universal send: group index (for the status line only)
    int commandIndex;         // remote view: 1-based command number at press time
    int commandCount;         // remote view: command count at press time
    uint32_t offset;          // remote view: byte offset of that command in the file
    uint32_t jobGeneration;   // g_sendGeneration when queued; a Back press makes it stale
    IrTiming timing;
    char path[kMaxPathLen];   // the .ir file this command works on
    char name[kMaxNameLen];   // universal send: group name
};

// irTask -> uiTask. irTask reports what happened; uiTask decides what the screen does.
struct IrEvent
{
    IrEventType type;
    bool success;
    bool fileRemoved;         // the whole file is gone (last command deleted)
    char text[kMaxNameLen];   // deleted name on success, reason on failure
    char path[kMaxPathLen];   // the file the result is about (the UI may have moved on)
};

// ---- Data owned by one task at a time -------------------------------------------------
// One IR signal, by value. Each task loads into its own IrPayload and never shares it: the
// signal data does not live in g_shared at all, only the name the screen shows does.
struct IrPayload
{
    uint16_t raw[kMaxRawBuffer] = {};
    uint16_t rawLen = 0;
    uint16_t freqKHz = kDefaultCarrierKHz;
    bool isParsed = false;
    String protocol;
    uint32_t address = 0;
    uint32_t command = 0;
    String btnName;
};

// Byte offset of every "name:" line in a .ir file. Built once per file open, then each
// command is read with one seek instead of rescanning from byte 0.
struct CommandIndex
{
    int count = 0;
    uint32_t offsets[kMaxCommandsPerFile] = {};
};

// Directory listing as fixed storage: names packed into one pool instead of 260 Strings.
// Every member is zero-initialised so the 7 KB object lives in .bss, not as an image in flash.
struct DirCache
{
    char path[kMaxPathLen] = {};
    bool valid = false;
    int count = 0;
    int poolUsed = 0;
    uint16_t nameOffset[kMaxDirItems] = {};
    bool isFolder[kMaxDirItems] = {};
    char pool[kDirNamePoolBytes] = {};
};

constexpr uint16_t kDirViewBack = 0xFFFF; // view row that shows ".. (Back)"

// Learn mode, owned by irTask. `signal` is frozen once captured: later decodes are ignored
// until the user asks to capture again, so Save always writes the signal the screen showed.
struct LearnSession
{
    bool listening = false; // receiver and its hardware timer are running
    bool captured = false;  // `signal` holds a capture waiting to be saved
    IrPayload signal;
};

struct JoyInput
{
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    bool press = false;
    bool any() const
    {
        return up || down || left || right || press;
    }
};

// ---- State shared between tasks, guarded by g_stateMutex ----------------------------
// Display state only. Each field has exactly one writing task (status is the exception: both
// tasks report through it, last message wins). The mutex makes each copy whole; the single
// writer is what keeps two fields from disagreeing with each other.
struct SharedState
{
    String btnName = "None";  // uiTask: name of the command the remote view has selected
    String status = "Ready";  // both tasks: last message for the status line
    bool transmitting = false; // irTask
    String sendingName;        // irTask: name going out of the LED right now, "" when idle
    bool univSending = false;
    int univProgressCurrent = 0;
    int univProgressTotal = 0;
    String univProgressName;
    LearnPhase learnPhase = LearnPhase::Idle;
    String learnProtocol;
    String learnDetail;
};

// What the renderer needs from SharedState, copied under the lock once per frame.
struct UiSnapshot
{
    String btnName;
    String status;
    bool transmitting = false;
    String sendingName;
    bool univSending = false;
    int univProgressCurrent = 0;
    int univProgressTotal = 0;
    String univProgressName;
    LearnPhase learnPhase = LearnPhase::Idle;
    String learnProtocol;
    String learnDetail;
};
