// ==========================================
// SD CARD + IR ENGINES
// ==========================================
// Rule for every function here: no setStatus() while g_sdMutex is held. Collect the result,
// release the SD lock, then talk to the UI. Taking g_stateMutex inside g_sdMutex would nest
// locks, and the architecture forbids that.

// irTask runs at priority 2 on core 0, above the IDLE task that feeds the watchdog. yield()
// does not let IDLE run; vTaskDelay(1) does. Called once per line of a long scan.
void scanBreather(int &lineCounter)
{
    if (++lineCounter % kScanBreatherLines == 0)
    {
        vTaskDelay(1);
    }
}

// ---- Directory cache -----------------------------------------------------------------
bool addDirEntry(const char *name, bool isFolder)
{
    size_t len = strlen(name) + 1;
    if (g_dirCache.count >= kMaxDirItems || g_dirCache.poolUsed + (int)len > kDirNamePoolBytes)
    {
        return false;
    }
    memcpy(g_dirCache.pool + g_dirCache.poolUsed, name, len);
    g_dirCache.nameOffset[g_dirCache.count] = (uint16_t)g_dirCache.poolUsed;
    g_dirCache.isFolder[g_dirCache.count] = isFolder;
    g_dirCache.poolUsed += (int)len;
    g_dirCache.count++;
    return true;
}

bool keepInBrowser(const char *name, bool isFolder)
{
    if (name[0] == '.' || startsWith(name, "System") || strcmp(name, "univ_tv.ir") == 0)
    {
        return false;
    }
    return isFolder || endsWithIgnoreCase(name, ".ir");
}

// ---- Card access -----------------------------------------------------------------------
// The card is treated exactly the way the original firmware treated it, because that is the
// behaviour proven on this hardware: it is mounted once in setup(), and after that it is only
// touched when the user asks for a file. Nothing in this firmware probes, resets or remounts
// a card that is mounted.
//
// History, so nobody adds it back: in November 2026 this file grew a periodic sector read,
// a "card lost" detector, SD.end()/SD.begin() remount ladders and raw reset commands. With
// them the card dropped out within seconds of every boot. The original firmware, which has
// none of that, ran on the same card and wiring without a single drop.

// If the boot mount failed (for example no card in the slot), try again when a file is next
// needed, at most once every kSdLateMountRetryMs. Never runs on a mounted card.
// Caller holds the SD lock.
bool ensureSdMountedLocked()
{
    static unsigned long lastTryMs = 0;
    static bool tried = false;
    if (g_sdMounted)
    {
        return true;
    }
    unsigned long now = millis();
    if (tried && now - lastTryMs < kSdLateMountRetryMs)
    {
        return false;
    }
    tried = true;
    lastTryMs = now;
    g_sdMounted = SD.begin(board::kSdCsPin, g_sdSpi, board::kSdSpiHz);
    return g_sdMounted;
}

// uiTask, every frame. While the card is not mounted (the boot mount failed, or the card stopped
// answering and the one restart in openSdForRead() failed), try to mount it again every
// kSdLateMountRetryMs without waiting for the user, and say so on the status line. Prints one
// line per try with the time, so the serial log shows how long an episode lasts.
// Never runs on a mounted card.
void sdMountRetryTick(unsigned long now)
{
    static unsigned long lastTryMs = 0;
    static bool wasAway = false;

    lockSD();
    bool mounted = g_sdMounted;
    unlockSD();
    if (mounted)
    {
        if (wasAway)
        {
            wasAway = false;
            Serial.printf("[sd] card is back at t=%lus\n", now / 1000);
            setStatus("SD card is back");
        }
        return;
    }

    wasAway = true;
    if (now - lastTryMs < kSdLateMountRetryMs)
    {
        return;
    }
    lastTryMs = now;
    lockSD();
    g_sdMounted = SD.begin(board::kSdCsPin, g_sdSpi, board::kSdSpiHz);
    unlockSD();
    Serial.printf("[sd] mount retry at t=%lus: %s\n", now / 1000, g_sdMounted ? "ok" : "failed");
    if (!g_sdMounted)
    {
        setStatus("SD card not answering, retrying");
    }
}

// Opens a path for reading. If the open fails it is tried once more after a short pause,
// which covers a card that was briefly busy.
//
// If the card itself has stopped answering (the root folder cannot be opened either), the
// card is left alone for kSdRecoveryWaitMs and then mounted again, at most once every
// kSdRecoveryMinIntervalMs. This is the one place a mounted card is ever touched, and it is
// needed because of a gap in the SD driver: after a card has reset itself it answers data
// commands with "illegal command", and the driver only re-initialises a card that answers
// with silence. Without this, one episode leaves the card unusable until the board reboots.
// The wait matters: a card asked to restart while it is still busy only fails again.
// Caller holds the SD lock.
File openSdForRead(const String &path)
{
    static unsigned long lastRecoveryMs = 0;
    static bool recoveredBefore = false;

    if (!ensureSdMountedLocked())
    {
        return File();
    }
    File file = SD.open(path);
    if (file)
    {
        return file;
    }
    delay(kSdOpenRetryDelayMs);
    file = SD.open(path);
    if (file)
    {
        return file;
    }

    File root = SD.open("/");
    if (root)
    {
        root.close();
        return File(); // the card is fine; this path simply does not exist
    }

    unsigned long now = millis();
    if (recoveredBefore && now - lastRecoveryMs < kSdRecoveryMinIntervalMs)
    {
        return File();
    }
    recoveredBefore = true;
    lastRecoveryMs = now;

    Serial.printf("[sd] card not answering while opening %s; leaving it alone for %lu ms\n", path.c_str(),
                  kSdRecoveryWaitMs);
    delay(kSdRecoveryWaitMs);
    SD.end();
    g_sdMounted = SD.begin(board::kSdCsPin, g_sdSpi, board::kSdSpiHz);
    Serial.printf("[sd] mounted again: %s\n", g_sdMounted ? "yes" : "no");
    return g_sdMounted ? SD.open(path) : File();
}

void rebuildDirectoryCache(const String &path)
{
    g_dirCache.count = 0;
    g_dirCache.poolUsed = 0;
    g_dirCache.valid = false;
    strlcpy(g_dirCache.path, path.c_str(), sizeof(g_dirCache.path));

    lockSD();
    File dir = openSdForRead(path);
    recoverInterruptedDeleteLocked(); // once per boot; a restored file then shows in this listing
    if (!dir || !dir.isDirectory())
    {
        if (dir)
        {
            dir.close();
        }
        unlockSD();
        return;
    }

    // Same calls, in the same order, as the original firmware's directory scan.
    int lines = 0;
    File file = dir.openNextFile();
    while (file)
    {
        const char *name = baseName(file.name());
        bool isFolder = file.isDirectory();
        if (keepInBrowser(name, isFolder) && !addDirEntry(name, isFolder))
        {
            file.close();
            break; // cache full; the rest of the folder is not shown
        }
        file.close();
        scanBreather(lines);
        file = dir.openNextFile();
    }

    dir.close();
    unlockSD();
    g_dirCache.valid = true;
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

// ---- Command index and loading -------------------------------------------------------
// One scan per file open. After this every command is one seek away.
int buildCommandIndex(const String &path, CommandIndex &index)
{
    index.count = 0;

    lockSD();
    File file = openSdForRead(path);
    if (!file)
    {
        unlockSD();
        setStatus("File Missing");
        return 0;
    }

    char line[kMaxLineLen];
    bool truncated = false;
    bool capped = false;
    int lines = 0;
    while (true)
    {
        uint32_t lineStart = file.position();
        if (!readLineBounded(file, line, sizeof(line), truncated))
        {
            break;
        }
        if (truncated)
        {
            skipRestOfLine(file);
        }
        if (startsWith(line, "name:"))
        {
            if (index.count >= kMaxCommandsPerFile)
            {
                capped = true;
                break;
            }
            index.offsets[index.count++] = lineStart;
        }
        scanBreather(lines);
    }
    file.close();
    unlockSD();

    if (capped)
    {
        setStatus("Capped at " + String(kMaxCommandsPerFile) + " cmds");
    }
    return index.count;
}

// Reads the command that starts at byte `offset` into `out`. Returns nullptr on success or a
// short reason. Touches the SD card and the status line only; the caller decides what to do
// with the loaded signal.
const char *loadFlipperCommandReason(const String &path, uint32_t offset, IrPayload &out)
{
    const char *err = nullptr;

    lockSD();
    File file = openSdForRead(path);
    if (!file)
    {
        unlockSD();
        setStatus("File Missing");
        return "File missing";
    }
    bool ok = file.seek(offset) && parseCommandAt(file, out, err);
    file.close();
    unlockSD();

    if (!ok)
    {
        const char *reason = err ? err : "Seek failed";
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

// ---- Safe delete -----------------------------------------------------------------------
// Rewrite to .tmp, verify its size, then journal the path and rename in two steps:
//   path -> path.bak, path.tmp -> path, remove path.bak.
// recoverInterruptedDeleteLocked() reads the journal once per boot and finishes or undoes a rename
// sequence that power loss cut short. All of this runs with g_sdMutex held.
bool writeDeleteJournal(const String &path)
{
    SD.remove(kDeleteJournalPath);
    File journal = SD.open(kDeleteJournalPath, FILE_WRITE);
    if (!journal)
    {
        return false;
    }
    size_t expected = path.length() + 1;
    size_t written = journal.print(path) + journal.print('\n');
    journal.close();

    File check = SD.open(kDeleteJournalPath);
    bool ok = check && written == expected && check.size() == expected;
    if (check)
    {
        check.close();
    }
    return ok;
}

void clearDeleteJournal()
{
    SD.remove(kDeleteJournalPath);
}

// Finishes or undoes a delete that power loss cut short. Runs once per boot, the first time the
// user opens a folder or deletes a command, and not in setup(): the boot sequence stays exactly
// the original firmware's. Caller holds the SD lock. No delete can be in flight then (a delete
// holds the lock from start to end), so a journal found here is always stale.
void recoverInterruptedDeleteLocked()
{
    static bool done = false;
    if (done || !g_sdMounted)
    {
        return;
    }
    done = true;

    if (!SD.exists(kDeleteJournalPath))
    {
        return;
    }

    char pathBuf[kMaxPathLen] = "";
    File journal = SD.open(kDeleteJournalPath);
    bool truncated = false;
    bool havePath = journal && readLineBounded(journal, pathBuf, sizeof(pathBuf), truncated) && !truncated;
    if (journal)
    {
        journal.close();
    }

    bool resolved = true;
    if (havePath && pathBuf[0] == '/')
    {
        String path = pathBuf;
        String tmpPath = path + ".tmp";
        String backupPath = path + ".bak";
        bool haveOriginal = SD.exists(path);
        bool haveBackup = SD.exists(backupPath);

        if (!haveOriginal && haveBackup)
        {
            // Cut between the two renames: put the original back.
            resolved = SD.rename(backupPath.c_str(), path.c_str());
            Serial.printf("[recover] restore %s from .bak: %s\n", path.c_str(), resolved ? "ok" : "FAILED");
        }
        else if (haveOriginal && haveBackup)
        {
            // Cut after the new file was renamed into place: the delete completed.
            SD.remove(backupPath.c_str());
            Serial.printf("[recover] removed stale %s\n", backupPath.c_str());
        }
        SD.remove(tmpPath.c_str());
    }

    // Keep the journal if the original could not be restored, so the next mount tries again.
    if (resolved)
    {
        clearDeleteJournal();
    }
}

// Copies every command except target_index (1-based) to path.tmp. Returns the number of
// commands kept, or -1 on error with `reason` set. Caller holds g_sdMutex.
int rewriteWithoutCommand(const String &path, const String &tmpPath, int targetIndex, String &deletedName,
                          const char *&reason)
{
    File source = SD.open(path);
    if (!source)
    {
        reason = "File missing";
        return -1;
    }

    SD.remove(tmpPath.c_str());
    File temp = SD.open(tmpPath, FILE_WRITE);
    if (!temp)
    {
        source.close();
        reason = "Cannot write .tmp";
        return -1;
    }

    char line[kMaxLineLen];
    bool truncated = false;
    int commandIndex = 0;
    int kept = 0;
    bool skipping = false;
    bool found = false;
    bool writeOk = true;
    size_t bytesWritten = 0;
    int lines = 0;

    while (writeOk && readLineBounded(source, line, sizeof(line), truncated))
    {
        if (startsWith(line, "name:"))
        {
            commandIndex++;
            skipping = (commandIndex == targetIndex);
            if (skipping)
            {
                found = true;
                deletedName = skipSpaces(line + 5);
            }
            else
            {
                kept++;
            }
        }

        if (skipping)
        {
            if (truncated)
            {
                skipRestOfLine(source);
            }
        }
        else
        {
            writeOk = copyLine(source, temp, line, truncated, bytesWritten);
        }
        scanBreather(lines);
    }

    // read() returns -1 both at end of file and on a card error. Only "position reached size"
    // proves the whole source was copied; without this a read error mid-file would produce a
    // short .tmp that then replaces the original and silently loses every later command.
    bool readAll = source.position() == source.size();
    source.close();
    temp.flush();
    temp.close();

    File check = SD.open(tmpPath);
    bool sizeOk = check && check.size() == bytesWritten;
    if (check)
    {
        check.close();
    }

    if (!writeOk || !sizeOk)
    {
        SD.remove(tmpPath.c_str());
        reason = "Write failed, file untouched";
        return -1;
    }
    if (!readAll) // checked second: a failed write also stops the loop before the end
    {
        SD.remove(tmpPath.c_str());
        reason = "Read failed, file untouched";
        return -1;
    }
    if (!found)
    {
        SD.remove(tmpPath.c_str());
        reason = "Command not found";
        return -1;
    }
    return kept;
}

bool deleteFlipperCommandByIndex(const String &path, int targetIndex, String &deletedName, bool &fileRemoved,
                                 const char *&reason)
{
    deletedName = "";
    fileRemoved = false;
    reason = nullptr;

    if (targetIndex <= 0)
    {
        reason = "No command selected";
        return false;
    }

    String tmpPath = path + ".tmp";
    String backupPath = path + ".bak";

    lockSD();
    recoverInterruptedDeleteLocked(); // once per boot, before a new journal replaces an old one
    int kept = rewriteWithoutCommand(path, tmpPath, targetIndex, deletedName, reason);
    if (kept < 0)
    {
        unlockSD();
        return false;
    }

    if (!writeDeleteJournal(path))
    {
        SD.remove(tmpPath.c_str());
        clearDeleteJournal();
        unlockSD();
        reason = "Journal write failed";
        return false;
    }

    bool ok = false;
    bool keepJournal = false;
    if (kept == 0)
    {
        ok = SD.remove(path.c_str());
        SD.remove(tmpPath.c_str());
        fileRemoved = ok;
        if (!ok)
        {
            reason = "Remove failed";
        }
    }
    else
    {
        SD.remove(backupPath.c_str());
        if (!SD.rename(path.c_str(), backupPath.c_str()))
        {
            SD.remove(tmpPath.c_str());
            reason = "Rename failed, file untouched";
        }
        else if (!SD.rename(tmpPath.c_str(), path.c_str()))
        {
            bool restored = SD.rename(backupPath.c_str(), path.c_str());
            SD.remove(tmpPath.c_str());
            reason = restored ? "Rename failed, restored" : "RESTORE FAILED: see .bak";
            keepJournal = !restored; // the next mount will try the restore again from .bak
        }
        else
        {
            SD.remove(backupPath.c_str());
            ok = true;
        }
    }

    if (!keepJournal)
    {
        clearDeleteJournal();
    }
    unlockSD();
    return ok;
}

// ---- Universal profiles ----------------------------------------------------------------
bool loadUniversalCommandList(const String &path)
{
    g_univCmdCount = 0;

    lockSD();
    File file = openSdForRead(path);
    if (!file)
    {
        unlockSD();
        setStatus("Missing " + path);
        return false;
    }

    char line[kMaxLineLen];
    bool truncated = false;
    int lines = 0;
    while (g_univCmdCount < kMaxUnivItems && readLineBounded(file, line, sizeof(line), truncated))
    {
        if (truncated)
        {
            skipRestOfLine(file);
        }
        if (startsWith(line, "name:"))
        {
            String groupName = skipSpaces(line + 5);
            bool alreadyExists = false;
            for (int i = 0; i < g_univCmdCount; i++)
            {
                if (g_univCmdItems[i].equalsIgnoreCase(groupName))
                {
                    alreadyExists = true;
                    break;
                }
            }
            if (!alreadyExists)
            {
                g_univCmdItems[g_univCmdCount++] = groupName;
            }
        }
        scanBreather(lines);
    }

    file.close();
    unlockSD();

    if (g_univCmdCount == 0)
    {
        setStatus("No commands in profile");
        return false;
    }
    return true;
}

// Byte offset of every command in the profile whose name matches groupName (case-insensitive).
// A universal profile can hold hundreds of entries per group, so this fills a CommandIndex
// (1000 slots) and not a small array. Returns the number of matches.
int buildGroupIndex(const String &path, const String &groupName, CommandIndex &index)
{
    index.count = 0;

    lockSD();
    File file = openSdForRead(path);
    if (!file)
    {
        unlockSD();
        return 0;
    }

    char line[kMaxLineLen];
    bool truncated = false;
    int lines = 0;
    while (index.count < kMaxCommandsPerFile)
    {
        uint32_t lineStart = file.position();
        if (!readLineBounded(file, line, sizeof(line), truncated))
        {
            break;
        }
        if (truncated)
        {
            skipRestOfLine(file);
        }
        if (startsWith(line, "name:") && groupName.equalsIgnoreCase(skipSpaces(line + 5)))
        {
            index.offsets[index.count++] = lineStart;
        }
        scanBreather(lines);
    }

    file.close();
    unlockSD();
    return index.count;
}

// ---- Learn mode ------------------------------------------------------------------------
int getNextLearnedIndex(const String &path)
{
    lockSD();
    File file = SD.open(path);
    int maxIndex = 0;

    if (!file)
    {
        unlockSD();
        return 1;
    }

    char line[kMaxLineLen];
    bool truncated = false;
    int lines = 0;
    while (readLineBounded(file, line, sizeof(line), truncated))
    {
        if (truncated)
        {
            skipRestOfLine(file);
        }
        if (startsWith(line, "name:"))
        {
            const char *name = skipSpaces(line + 5);
            if (startsWith(name, "Learned_"))
            {
                unsigned long parsed = strtoul(name + 8, nullptr, 10);
                int value = parsed > 99999 ? 99999 : (int)parsed;
                if (value > maxIndex)
                {
                    maxIndex = value;
                }
            }
        }
        scanBreather(lines);
    }

    file.close();
    unlockSD();
    return maxIndex + 1;
}

// irTask only. Saves the capture irTask is holding, which is the one the screen described.
bool saveLearnedCommandToSD(const IrPayload &p)
{
    if (p.isParsed || p.rawLen == 0)
    {
        setStatus("Nothing to save");
        setLearnPhase(LearnPhase::Error);
        return false;
    }

    lockSD();
    SD.mkdir(kLearnedDir);
    unlockSD();
    String path = kLearnedFilePath;
    int nextIdx = getNextLearnedIndex(path);

    char nameBuf[20];
    snprintf(nameBuf, sizeof(nameBuf), "Learned_%03d", nextIdx);

    lockSD();
    File file = SD.open(path, FILE_APPEND);
    if (!file)
    {
        unlockSD();
        setStatus("Save failed");
        setLearnPhase(LearnPhase::Error);
        return false;
    }

    size_t sizeBefore = file.size(); // size at open; FILE_APPEND starts writing here
    size_t written = 0;
    written += file.printf("name: %s\ntype: raw\nfrequency: %u\nduty_cycle: 0.33\ndata:", nameBuf,
                           (unsigned)p.freqKHz * 1000);
    for (uint16_t i = 0; i < p.rawLen; i++)
    {
        written += file.printf(" %u", (unsigned)p.raw[i]);
    }
    written += file.print("\n\n");
    file.close();

    // A write is not done until it has been read back: reopen and compare the size.
    File check = SD.open(path);
    size_t sizeAfter = check ? check.size() : 0;
    if (check)
    {
        check.close();
    }
    unlockSD();

    if (sizeAfter != sizeBefore + written)
    {
        setStatus("Save failed: short write");
        setLearnPhase(LearnPhase::Error);
        return false;
    }

    setStatus("Saved " + String(nameBuf));
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
