#include "storage.h"

#include <SPI.h>
#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "board_config.h"
#include "ir_file.h"

// Everything that talks to the SD card. Owns the SPI bus object, the SD mutex and the mount
// flag. Rules, in force since 2026-11-09 (see "SD card rules" in CLAUDE.md):
//   - the card is mounted in setup() and otherwise touched only when the user asks for a file;
//   - nothing here writes shared state or UI globals; results and reasons go back to the caller,
//     which also means no shared-state lock can ever be taken while the SD lock is held;
//   - every public function takes the SD lock itself, so callers never hold it.

namespace
{
SPIClass s_spi(FSPI);
SemaphoreHandle_t s_mutex = NULL;
bool s_mounted = false; // guarded by s_mutex

void lockSD()
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

void unlockSD()
{
    xSemaphoreGive(s_mutex);
}

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
bool addDirEntry(DirCache &cache, const char *name, bool isFolder)
{
    size_t len = strlen(name) + 1;
    if (cache.count >= kMaxDirItems || cache.poolUsed + (int)len > kDirNamePoolBytes)
    {
        return false;
    }
    memcpy(cache.pool + cache.poolUsed, name, len);
    cache.nameOffset[cache.count] = (uint16_t)cache.poolUsed;
    cache.isFolder[cache.count] = isFolder;
    cache.poolUsed += (int)len;
    cache.count++;
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

// If the boot mount failed (for example no card in the slot), try again when a file is next
// needed, at most once every kSdLateMountRetryMs. Never runs on a mounted card.
// Caller holds the SD lock.
bool ensureSdMountedLocked()
{
    static unsigned long lastTryMs = 0;
    static bool tried = false;
    if (s_mounted)
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
    s_mounted = SD.begin(board::kSdCsPin, s_spi, board::kSdSpiHz);
    return s_mounted;
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
    s_mounted = SD.begin(board::kSdCsPin, s_spi, board::kSdSpiHz);
    Serial.printf("[sd] mounted again: %s\n", s_mounted ? "yes" : "no");
    return s_mounted ? SD.open(path) : File();
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
    if (done || !s_mounted)
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
} // namespace

// ---- Bus and mount ---------------------------------------------------------------------------
bool storageInitBus()
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex)
    {
        return false;
    }
    // These two lines are the original firmware's, call for call (the pull-up request on MISO is
    // undone by begin(), which re-configures the pin as a plain input; the original behaved the
    // same, so it is left alone).
    pinMode(board::kExtSpiMisoPin, INPUT_PULLUP);
    s_spi.begin(board::kExtSpiSckPin, board::kExtSpiMisoPin, board::kExtSpiMosiPin, board::kSdCsPin);
    return true;
}

bool storageMount()
{
    lockSD();
    s_mounted = SD.begin(board::kSdCsPin, s_spi, board::kSdSpiHz);
    unlockSD();
    return s_mounted;
}

bool storageMounted()
{
    lockSD();
    bool mounted = s_mounted;
    unlockSD();
    return mounted;
}

// uiTask, every frame. While the card is not mounted (the boot mount failed, or the card stopped
// answering and the one restart in openSdForRead() failed), try to mount it again every
// kSdLateMountRetryMs without waiting for the user, and say so on the status line. Prints one
// line per try with the time, so the serial log shows how long an episode lasts.
// Never runs on a mounted card.
StorageTick storageMountRetryTick(unsigned long now)
{
    static unsigned long lastTryMs = 0;
    static bool wasAway = false;

    lockSD();
    bool mounted = s_mounted;
    unlockSD();
    if (mounted)
    {
        if (wasAway)
        {
            wasAway = false;
            Serial.printf("[sd] card is back at t=%lus\n", now / 1000);
            return StorageTick::CameBack;
        }
        return StorageTick::NoChange;
    }

    wasAway = true;
    if (now - lastTryMs < kSdLateMountRetryMs)
    {
        return StorageTick::NoChange;
    }
    lastTryMs = now;
    lockSD();
    s_mounted = SD.begin(board::kSdCsPin, s_spi, board::kSdSpiHz);
    unlockSD();
    Serial.printf("[sd] mount retry at t=%lus: %s\n", now / 1000, s_mounted ? "ok" : "failed");
    return s_mounted ? StorageTick::CameBack : StorageTick::RetryFailed;
}

// ---- Reading -------------------------------------------------------------------------------
bool storageListDirectory(const String &path, DirCache &cache)
{
    cache.count = 0;
    cache.poolUsed = 0;
    cache.valid = false;
    strlcpy(cache.path, path.c_str(), sizeof(cache.path));

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
        return false;
    }

    // Same calls, in the same order, as the original firmware's directory scan.
    int lines = 0;
    File file = dir.openNextFile();
    while (file)
    {
        const char *name = baseName(file.name());
        bool isFolder = file.isDirectory();
        if (keepInBrowser(name, isFolder) && !addDirEntry(cache, name, isFolder))
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
    cache.valid = true;
    return true;
}

// ---- Command index and loading -------------------------------------------------------
// One scan per file open. After this every command is one seek away.
int storageIndexCommands(const String &path, CommandIndex &index, bool &capped)
{
    index.count = 0;

    lockSD();
    File file = openSdForRead(path);
    if (!file)
    {
        unlockSD();
        return -1;
    }

    char line[kMaxLineLen];
    bool truncated = false;
    capped = false;
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

    return index.count;
}

// Reads the command that starts at byte `offset` into `out`. Returns nullptr on success or a
// short reason. Touches the SD card and the status line only; the caller decides what to do
// with the loaded signal.
const char *storageLoadCommand(const String &path, uint32_t offset, IrPayload &out)
{
    const char *err = nullptr;

    lockSD();
    File file = openSdForRead(path);
    if (!file)
    {
        unlockSD();
        return "File missing";
    }
    bool ok = file.seek(offset) && parseCommandAt(file, out, err);
    file.close();
    unlockSD();

    if (!ok)
    {
        return err ? err : "Seek failed";
    }
    return nullptr;
}

// ---- Universal profiles ----------------------------------------------------------------
int storageListGroups(const String &path, String *items, int maxItems)
{
    int count = 0;

    lockSD();
    File file = openSdForRead(path);
    if (!file)
    {
        unlockSD();
        return -1;
    }

    char line[kMaxLineLen];
    bool truncated = false;
    int lines = 0;
    while (count < maxItems && readLineBounded(file, line, sizeof(line), truncated))
    {
        if (truncated)
        {
            skipRestOfLine(file);
        }
        if (startsWith(line, "name:"))
        {
            String groupName = skipSpaces(line + 5);
            bool alreadyExists = false;
            for (int i = 0; i < count; i++)
            {
                if (items[i].equalsIgnoreCase(groupName))
                {
                    alreadyExists = true;
                    break;
                }
            }
            if (!alreadyExists)
            {
                items[count++] = groupName;
            }
        }
        scanBreather(lines);
    }

    file.close();
    unlockSD();

    return count;
}

// Byte offset of every command in the profile whose name matches groupName (case-insensitive).
// A universal profile can hold hundreds of entries per group, so this fills a CommandIndex
// (1000 slots) and not a small array. Returns the number of matches.
int storageIndexGroup(const String &path, const String &groupName, CommandIndex &index)
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

// ---- Writing -------------------------------------------------------------------------------
bool storageDeleteCommand(const String &path, int targetIndex, String &deletedName, bool &fileRemoved,
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

// irTask only. Saves the capture irTask is holding, which is the one the screen described.
const char *storageSaveLearned(const IrPayload &p, String &savedName)
{
    if (p.isParsed || p.rawLen == 0)
    {
        return "Nothing to save";
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
        return "Save failed";
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
        return "Save failed: short write";
    }

    savedName = nameBuf;
    return nullptr;
}
