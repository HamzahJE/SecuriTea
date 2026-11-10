// ==========================================
// APP GLUE: STORAGE AND IR RESULTS -> SHARED STATE
// ==========================================
// The SD card itself is in storage.h/.cpp and the IR hardware in ir_engine.h/.cpp. Both return
// results and reasons; the functions here turn those into what the user sees (status line,
// learn phase, button name) and keep the UI's own view of the directory and the command index.
// None of them holds the SD lock: that lock lives inside storage.cpp, so a shared-state update
// can never happen while the card is locked.

// ---- Card mount, from uiTask ------------------------------------------------------------
void sdMountRetryTick(unsigned long now)
{
    StorageTick tick = storageMountRetryTick(now);
    if (tick == StorageTick::CameBack)
    {
        setStatus("SD card is back");
    }
    else if (tick == StorageTick::RetryFailed)
    {
        setStatus("SD card not answering, retrying");
    }
}

// ---- Directory view ---------------------------------------------------------------------------
void rebuildDirectoryCache(const String &path)
{
    storageListDirectory(path, g_dirCache);
}

// Builds the filtered view (g_dirView) from the cache, rebuilding the cache if needed.
void loadDirectory(const String &path)
{
    g_dirViewCount = 0;

    if (!g_dirCache.valid || path != g_dirCache.path)
    {
        rebuildDirectoryCache(path);
    }
    if (!g_dirCache.valid)
    {
        return;
    }

    if (path != "/")
    {
        g_dirView[g_dirViewCount++] = kDirViewBack;
    }
    for (int i = 0; i < g_dirCache.count && g_dirViewCount < kMaxDirItems; i++)
    {
        if (matchesFileBrowserFilter(g_dirCache.pool + g_dirCache.nameOffset[i]))
        {
            g_dirView[g_dirViewCount++] = (uint16_t)i;
        }
    }
    g_menuIndex = 0;
}

bool dirRowIsBack(int row)
{
    return g_dirView[row] == kDirViewBack;
}

bool dirRowIsFolder(int row)
{
    return dirRowIsBack(row) || g_dirCache.isFolder[g_dirView[row]];
}

const char *dirRowName(int row)
{
    if (dirRowIsBack(row))
    {
        return ".. (Back)";
    }
    return g_dirCache.pool + g_dirCache.nameOffset[g_dirView[row]];
}

String dirRowLabel(int row)
{
    if (!dirRowIsBack(row) && dirRowIsFolder(row))
    {
        return "[DIR] " + String(dirRowName(row));
    }
    return String(dirRowName(row));
}

// ---- Command index and loading -------------------------------------------------------------
// One scan per file open. After this every command is one seek away.
int buildCommandIndex(const String &path, CommandIndex &index)
{
    bool capped = false;
    int count = storageIndexCommands(path, index, capped);
    if (count < 0)
    {
        setStatus("File Missing");
        return 0;
    }
    if (capped)
    {
        setStatus("Capped at " + String(kMaxCommandsPerFile) + " cmds");
    }
    return count;
}

// Reads the command that starts at byte `offset` into `out`. Returns nullptr on success or a
// short reason; either way the status line reports it.
const char *loadFlipperCommandReason(const String &path, uint32_t offset, IrPayload &out)
{
    const char *reason = storageLoadCommand(path, offset, out);
    if (reason)
    {
        setStatus(reason);
        return reason;
    }
    setStatus("Loaded: " + out.btnName + (out.isParsed ? " (parsed)" : ""));
    return nullptr;
}

bool loadFlipperCommandAt(const String &path, uint32_t offset, IrPayload &out)
{
    return loadFlipperCommandReason(path, offset, out) == nullptr;
}

// uiTask only: what the UI calls when you scroll or open a file. Loading the whole command
// validates it, so a bad command shows its reason now and not when you press Send. Only the
// name is kept for the screen; irTask loads its own copy when it sends.
bool publishCommandAt(const String &path, uint32_t offset)
{
    IrPayload payload;
    const char *reason = loadFlipperCommandReason(path, offset, payload);
    if (reason)
    {
        // The remote view has no status line. Put the reason where the name goes, so the
        // screen never keeps showing the previous command's name for one that failed to load.
        setButtonName(String("! ") + reason);
        return false;
    }
    setButtonName(payload.btnName);
    return true;
}

// ---- Universal profiles ------------------------------------------------------------------
bool loadUniversalCommandList(const String &path)
{
    int count = storageListGroups(path, g_univCmdItems, kMaxUnivItems);
    g_univCmdCount = count < 0 ? 0 : count;
    if (count < 0)
    {
        setStatus("Missing " + path);
        return false;
    }
    if (count == 0)
    {
        setStatus("No commands in profile");
        return false;
    }
    return true;
}

// ---- Learn -------------------------------------------------------------------------------
// irTask only. Saves the capture irTask is holding, which is the one the screen described.
bool saveLearnedCommandToSD(const IrPayload &p)
{
    String savedName;
    const char *reason = storageSaveLearned(p, savedName);
    if (reason)
    {
        setStatus(reason);
        setLearnPhase(LearnPhase::Error);
        return false;
    }
    setStatus("Saved " + savedName);
    setLearnPhase(LearnPhase::Saved);
    return true;
}

// irTask only. Polls the receiver through the ir_engine; on a capture, reports it on the shared
// state so the Learn screen can describe it. Returns true when `out` holds a usable signal.
bool captureLearnSignal(IrPayload &out)
{
    String protocol;
    String detail;
    IrCaptureResult result = irCapture(out, protocol, detail);
    if (result == IrCaptureResult::Nothing)
    {
        return false;
    }
    if (result == IrCaptureResult::Empty)
    {
        setLearnDetails("", "");
        setStatus("Capture failed");
        setLearnPhase(LearnPhase::Error);
        return false;
    }
    setLearnDetails(protocol, detail);
    setStatus("Captured " + protocol);
    setLearnPhase(LearnPhase::Captured);
    return true;
}

// ---- Transmit --------------------------------------------------------------------------
// Sends exactly the payload it is given through the ir_engine and reports on the shared state.
// irTask uses this with its own copy, so nothing the UI does between load and send can change
// what goes out of the LED.
bool transmitPayload(const IrPayload &p, const IrTiming &t)
{
    markTransmitting(true, p.btnName);
    setStatus("Sending " + p.btnName);

    IrSendResult result = irSend(p, t);
    bool ok = result == IrSendResult::Sent;
    if (ok)
    {
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    markTransmitting(false, "");
    if (ok)
    {
        setStatus("Sent " + p.btnName);
    }
    else
    {
        setStatus(result == IrSendResult::UnsupportedProtocol ? "Unsupported protocol" : "No raw payload");
    }
    return ok;
}
