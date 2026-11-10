#pragma once
// storage module: everything that talks to the SD card.
//
// Owns the SPI bus object, the SD lock and the mount flag. Every function here takes the lock
// itself, so callers never hold it. Nothing here writes shared state, the status line or a UI
// global: results and reasons go back to the caller, which decides what the user sees. The
// card is mounted in setup() and otherwise touched only when the user asks for a file (see
// "SD card rules" in CLAUDE.md).

#include "app_state.h"

// ---- Bus and mount ---------------------------------------------------------------------------
// Creates the lock and starts the SPI bus. Call once, early in setup(). False if the lock
// could not be created.
bool storageInitBus();

// One mount attempt. Returns true if the card is mounted afterwards.
bool storageMount();

bool storageMounted();

enum class StorageTick
{
    NoChange,
    CameBack,    // the card was away and is mounted again
    RetryFailed  // the card is still away; a retry was just made and failed
};

// Call every UI frame. Does nothing while the card is mounted. While it is not, retries the
// mount every kSdLateMountRetryMs and reports what happened.
StorageTick storageMountRetryTick(unsigned long now);

// ---- Reading -------------------------------------------------------------------------------
// Fills `cache` with the folders and .ir files of `path`. False if the folder could not be
// opened (cache.valid is then false).
bool storageListDirectory(const String &path, DirCache &cache);

// Byte offset of every "name:" line in a .ir file. Returns the count, or -1 if the file could
// not be opened. `capped` is set when the file holds more than kMaxCommandsPerFile commands.
int storageIndexCommands(const String &path, CommandIndex &index, bool &capped);

// Reads the command that starts at byte `offset` into `out`. nullptr on success, else a short
// reason ("File missing", "Pulse of 0 us", ...).
const char *storageLoadCommand(const String &path, uint32_t offset, IrPayload &out);

// Distinct group names of a universal profile, in file order, at most maxItems. Returns the
// count, or -1 if the file could not be opened.
int storageListGroups(const String &path, String *items, int maxItems);

// Byte offset of every command whose name matches groupName (case-insensitive). Returns the
// count (at most kMaxCommandsPerFile).
int storageIndexGroup(const String &path, const String &groupName, CommandIndex &index);

// ---- Writing -------------------------------------------------------------------------------
// Deletes command targetIndex (1-based) from the file, safely (rewrite, verify, journal,
// rename). On success deletedName holds the name and fileRemoved says whether the whole file
// is gone. On failure `reason` says why; the original file is untouched unless the reason says
// otherwise.
bool storageDeleteCommand(const String &path, int targetIndex, String &deletedName, bool &fileRemoved,
                          const char *&reason);

// Appends a learned raw signal to kLearnedFilePath as the next "Learned_NNN". nullptr on
// success with savedName set, else a short reason.
const char *storageSaveLearned(const IrPayload &p, String &savedName);
