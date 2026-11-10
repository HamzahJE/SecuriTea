#include "ir_app.h"

#include "app_glue.h"
#include "storage.h"
#include "ui_layout.h"

// The handlers and the screen model of the infrared app. The app's state lives in the members
// of IrApp; only uiTask calls anything here.

namespace
{
// ---- Universal send timing -------------------------------------------------------------
IrTiming irTimingForMode(UniversalMode mode)
{
    IrTiming t;
    if (mode == UniversalMode::Aggressive)
    {
        t.sendDelayMs = 180;
        t.repeatGapMs = 80;
        t.transmitRepeats = 2;
        t.loadRetries = 3;
        t.frameRepeat = 1;
        t.aggressive = true;
    }
    else
    {
        t.sendDelayMs = 120;
        t.repeatGapMs = 60;
        t.transmitRepeats = 1;
        t.loadRetries = 2;
        t.frameRepeat = 0;
        t.aggressive = false;
    }
    return t;
}

String bruteProfileForTarget(const String &target)
{
    if (target == "AC")
    {
        return "/univ_ac.ir";
    }
    if (target == "Proj")
    {
        return "/univ_proj.ir";
    }
    if (target == "Audio")
    {
        return "/univ_audio.ir";
    }
    return "/univ_tv.ir";
}

// ---- Paths and file browser ------------------------------------------------------------
String joinPath(const String &dir, const char *name)
{
    if (dir == "/")
    {
        return "/" + String(name);
    }
    return dir + "/" + name;
}

String getParentDir(const String &path)
{
    if (path == "/" || path == "")
    {
        return "/";
    }
    int lastSlash = path.lastIndexOf('/');
    if (lastSlash <= 0)
    {
        return "/";
    }
    return path.substring(0, lastSlash);
}
} // namespace

// ---- App interface ---------------------------------------------------------------------------
void IrApp::onEvent(const IrEvent &evt)
{
    if (evt.type == IrEventType::DeleteResult)
    {
        handleDeleteResult(evt);
    }
    uiDirty_ = true;
}

void IrApp::tick(unsigned long now)
{
    if (previousState_ != state_)
    {
        onStateChanged(previousState_, state_);
        previousState_ = state_;
        uiDirty_ = true;
    }
    if (deleteArmed_ && now - deleteArmedMs_ > kDeleteConfirmWindowMs)
    {
        disarmDelete();
        uiDirty_ = true;
    }
}

bool IrApp::keepsAwake(const UiSnapshot &snap) const
{
    return state_ == AppState::Learn || snap.univSending || snap.transmitting;
}

// ---- Moved code ------------------------------------------------------------------------------
// ---- Results from irTask --------------------------------------------------------------
// Runs on uiTask. uiTask owns state_, the directory view and the command index, so only
// uiTask may change them in response to a delete.
void IrApp::handleDeleteResult(const IrEvent &evt)
{
    deletePending_ = false;
    if (!evt.success)
    {
        setStatus(evt.text[0] ? String(evt.text) : String("Delete failed"));
        return;
    }

    dirCache_.valid = false; // the folder listing may have changed
    String deletedName = evt.text;

    // The result is about evt.path. If the user has opened a different file since, that file's
    // index and counters are not ours to touch: refresh the listing and stop.
    if (selectedFilePath_ != evt.path)
    {
        if (state_ == AppState::FileBrowser)
        {
            loadDirectory(currentPath_);
        }
        setStatus("Deleted " + deletedName);
        return;
    }

    int remaining = evt.fileRemoved ? 0 : buildCommandIndex(selectedFilePath_, fileIndex_);
    if (remaining <= 0)
    {
        remoteCmdCount_ = 0;
        fileIndex_.count = 0;
        if (state_ == AppState::RemoteView)
        {
            state_ = AppState::FileBrowser;
        }
        if (state_ == AppState::FileBrowser)
        {
            loadDirectory(currentPath_);
        }
        setStatus("Deleted " + deletedName + (evt.fileRemoved ? " (file empty)" : ""));
        return;
    }

    remoteCmdCount_ = remaining;
    if (remoteCmdIndex_ > remoteCmdCount_)
    {
        remoteCmdIndex_ = remoteCmdCount_;
    }
    if (remoteCmdIndex_ < 1)
    {
        remoteCmdIndex_ = 1;
    }
    showRemoteCommand(remoteCmdIndex_);
    setStatus("Deleted " + deletedName);
}

// ---- Input -----------------------------------------------------------------------------
// The joystick itself lives in input.h/.cpp (class Joystick); this section maps its events
// onto the current screen.
int IrApp::menuLengthForState()
{
    switch (state_)
    {
        case AppState::MainMenu:
            return kMainMenuLen;
        case AppState::UniversalMenu:
            return kUnivMenuLen;
        case AppState::UniversalRemote:
            return univCmdCount_;
        case AppState::FileBrowser:
            return dirViewCount_;
        default:
            return 0;
    }
}

void IrApp::disarmDelete()
{
    deleteArmed_ = false;
}

void IrApp::showRemoteCommand(int index)
{
    if (index < 1 || index > fileIndex_.count)
    {
        return;
    }
    remoteCmdIndex_ = index;
    publishCommandAt(selectedFilePath_, fileIndex_.offsets[index - 1]);
}

void IrApp::handleScroll(bool up)
{
    int step = up ? -1 : 1;

    if (state_ == AppState::UniversalMenu)
    {
        headerModeSelected_ = false;
    }

    if (state_ == AppState::RemoteView)
    {
        remoteModeSelected_ = false;
        disarmDelete();
        if (deletePending_)
        {
            return; // the command numbers are about to change; wait for the result
        }
        int next = remoteCmdIndex_ + step;
        if (next >= 1 && next <= remoteCmdCount_)
        {
            showRemoteCommand(next);
        }
        return;
    }

    int len = menuLengthForState();
    if (len <= 0)
    {
        menuIndex_ = 0;
        return;
    }
    if (state_ == AppState::FileBrowser)
    {
        menuIndex_ = constrain(menuIndex_ + step, 0, len - 1); // lists do not wrap
    }
    else
    {
        menuIndex_ = (menuIndex_ + step + len) % len; // menus wrap
    }
}

void IrApp::handleRight()
{
    if (state_ == AppState::UniversalMenu)
    {
        headerModeSelected_ = true;
    }
    else if (state_ == AppState::FileBrowser)
    {
        cycleFileBrowserFilter();
        loadDirectory(currentPath_);
        setStatus("Filter: " + getFileBrowserFilterLabel());
    }
    else if (state_ == AppState::RemoteView)
    {
        remoteModeSelected_ = true;
        disarmDelete();
    }
}

void IrApp::handleLeft()
{
    if (state_ == AppState::UniversalMenu && headerModeSelected_)
    {
        headerModeSelected_ = false;
    }
    else if (state_ == AppState::RemoteView && remoteModeSelected_)
    {
        remoteModeSelected_ = false;
    }
    else if (state_ == AppState::FileBrowser && fileFilterIndex_ != 0)
    {
        resetFileBrowserFilter();
        loadDirectory(currentPath_);
        setStatus("Filter: ALL");
    }
    else if (state_ == AppState::UniversalMenu || state_ == AppState::Learn || state_ == AppState::FileBrowser)
    {
        state_ = AppState::MainMenu;
        menuIndex_ = 0;
    }
    else if (state_ == AppState::UniversalRemote)
    {
        cancelSendJobs(); // irTask clears the progress popup when its job stops
        state_ = AppState::UniversalMenu;
        menuIndex_ = 0;
    }
    else if (state_ == AppState::RemoteView)
    {
        cancelSendJobs(); // stops a running loop send and skips queued ones
        disarmDelete();
        state_ = AppState::FileBrowser;
    }
}

void IrApp::handlePressMainMenu()
{
    if (menuIndex_ == 0)
    {
        state_ = AppState::UniversalMenu;
    }
    else if (menuIndex_ == 1)
    {
        state_ = AppState::FileBrowser;
        currentPath_ = "/";
        resetFileBrowserFilter();
        dirCache_.valid = false;
        loadDirectory(currentPath_);
    }
    else if (menuIndex_ == 2)
    {
        state_ = AppState::Learn;
    }
    menuIndex_ = 0;
}

void IrApp::handlePressUniversalMenu()
{
    if (headerModeSelected_)
    {
        universalMode_ =
            (universalMode_ == UniversalMode::Single) ? UniversalMode::Aggressive : UniversalMode::Single;
        setStatus("Mode: " + getUniversalModeLabel());
        headerModeSelected_ = false;
        return;
    }

    static const char *const kTargets[] = {"TV", "AC", "Proj", "Audio"};
    targetDevice_ = kTargets[constrain(menuIndex_, 0, kUnivMenuLen - 1)];
    univProfilePath_ = bruteProfileForTarget(targetDevice_);
    if (loadUniversalCommandList(univProfilePath_))
    {
        setStatus("Select command"); // on failure the loader's own message stays
    }

    state_ = AppState::UniversalRemote;
    menuIndex_ = 0;
}

void IrApp::handlePressUniversalRemote()
{
    if (univCmdCount_ <= 0)
    {
        setStatus("No commands loaded");
        return;
    }
    queueCommand(IrCommandType::UniversalSend, menuIndex_);
}

void IrApp::openSelectedFile()
{
    const char *entryName = dirRowName(menuIndex_);
    String fullPath = joinPath(currentPath_, entryName);
    if (fullPath.length() >= kMaxPathLen)
    {
        setStatus("Path too long");
        return;
    }

    int count = buildCommandIndex(fullPath, fileIndex_);
    if (count <= 0)
    {
        setStatus("No commands in file");
        return;
    }

    selectedFilePath_ = fullPath;
    selectedFileName_ = entryName;
    remoteCmdCount_ = count;
    remoteCmdIndex_ = 1;
    showRemoteCommand(1);
    state_ = AppState::RemoteView;
}

void IrApp::handlePressFileBrowser()
{
    if (dirViewCount_ == 0)
    {
        setStatus("Folder empty");
        return;
    }

    if (!dirRowIsFolder(menuIndex_))
    {
        openSelectedFile();
        return;
    }

    if (dirRowIsBack(menuIndex_))
    {
        currentPath_ = getParentDir(currentPath_);
    }
    else
    {
        currentPath_ = joinPath(currentPath_, dirRowName(menuIndex_));
        resetFileBrowserFilter(); // first-letter filter applies to the current level only
    }
    loadDirectory(currentPath_);
}

void IrApp::handlePressRemoteView(unsigned long now)
{
    if (remoteModeSelected_)
    {
        if (remoteSendMode_ == RemoteSendMode::One)
        {
            remoteSendMode_ = RemoteSendMode::Loop;
        }
        else if (remoteSendMode_ == RemoteSendMode::Loop)
        {
            remoteSendMode_ = RemoteSendMode::Delete;
        }
        else
        {
            remoteSendMode_ = RemoteSendMode::One;
        }
        setStatus("Remote Mode: " + getRemoteSendModeLabel());
        remoteModeSelected_ = false;
        disarmDelete();
        return;
    }

    // A delete renumbers the commands. Until its result arrives the index on screen may be
    // stale, so nothing that names a command by number or offset is queued.
    if (deletePending_)
    {
        setStatus("Deleting, please wait");
        return;
    }

    if (remoteSendMode_ == RemoteSendMode::Loop)
    {
        queueCommand(IrCommandType::RemoteSendAll, 0);
    }
    else if (remoteSendMode_ == RemoteSendMode::One)
    {
        queueCommand(IrCommandType::RemoteSendOne, 0);
    }
    else if (deleteArmed_ && now - deleteArmedMs_ <= kDeleteConfirmWindowMs)
    {
        disarmDelete();
        deletePending_ = queueCommand(IrCommandType::RemoteDelete, 0);
    }
    else
    {
        deleteArmed_ = true;
        deleteArmedMs_ = now;
        setStatus("Press again to delete");
    }
}

void IrApp::handlePressLearn()
{
    LearnPhase phase = getLearnPhaseSnapshot();
    if (phase == LearnPhase::Captured)
    {
        if (queueCommand(IrCommandType::LearnSave, 0))
        {
            setLearnPhase(LearnPhase::Saving);
        }
    }
    else if (phase == LearnPhase::Error || phase == LearnPhase::Idle || phase == LearnPhase::Saved)
    {
        queueCommand(IrCommandType::LearnStart, 0);
    }
}

void IrApp::handlePress(unsigned long now)
{
    switch (state_)
    {
        case AppState::MainMenu:
            handlePressMainMenu();
            break;
        case AppState::UniversalMenu:
            handlePressUniversalMenu();
            break;
        case AppState::UniversalRemote:
            handlePressUniversalRemote();
            break;
        case AppState::FileBrowser:
            handlePressFileBrowser();
            break;
        case AppState::RemoteView:
            handlePressRemoteView(now);
            break;
        case AppState::Learn:
            handlePressLearn();
            break;
    }
}

void IrApp::onInput(const JoyInput &in, unsigned long now)
{
    if (in.up || in.down)
    {
        handleScroll(in.up);
    }
    if (in.right)
    {
        handleRight();
    }
    if (in.left)
    {
        handleLeft();
    }
    if (in.press)
    {
        handlePress(now);
    }
}

void IrApp::onStateChanged(AppState previous, AppState next)
{
    if (next == AppState::Learn)
    {
        queueCommand(IrCommandType::LearnStart, 0);
        setStatus("Point remote and press key");
    }
    if (next == AppState::UniversalMenu)
    {
        headerModeSelected_ = false;
    }
    if (next == AppState::RemoteView)
    {
        remoteModeSelected_ = false;
        disarmDelete();
    }
    if (previous == AppState::Learn)
    {
        queueCommand(IrCommandType::LearnStop, 0);
    }
}

// ---- Screen model ------------------------------------------------------------------------
// Gathers what the screens need from the UI-owned globals. The drawing itself is in ui.cpp.
void IrApp::buildModel(UiModel &m, const UiSnapshot &snap)
{
    m.state = state_;
    m.menuIndex = menuIndex_;

    m.headerModeSelected = headerModeSelected_;
    m.universalBadge = getUniversalModeBadge();
    m.targetDevice = targetDevice_;
    m.univProfilePath = univProfilePath_;
    m.univItems = univCmdItems_;
    m.univCount = univCmdCount_;

    if (state_ == AppState::FileBrowser)
    {
        m.browserPath = currentPath_;
        m.browserFilterLabel = getFileBrowserFilterLabel();
        m.browserCount = dirViewCount_;
        m.browserWindowStart = listWindowStart(menuIndex_, dirViewCount_);
        // Build only the rows on screen, not all 260.
        for (int i = 0; i < kMenuVisibleRows && m.browserWindowStart + i < dirViewCount_; i++)
        {
            m.browserRows[m.browserShown++] = dirRowLabel(m.browserWindowStart + i);
        }
    }

    m.fileName = selectedFileName_;
    m.cmdIndex = remoteCmdIndex_;
    m.cmdCount = remoteCmdCount_;
    m.sendModeBadge = getRemoteSendModeBadge();
    m.deleteMode = remoteSendMode_ == RemoteSendMode::Delete;
    m.modeSelected = remoteModeSelected_;
    m.deleteArmed = deleteArmed_;
    m.deletePending = deletePending_;

    m.shared = snap;
}

String IrApp::getUniversalModeLabel()
{
    return universalMode_ == UniversalMode::Aggressive ? "Aggressive" : "Single";
}

String IrApp::getUniversalModeBadge()
{
    return universalMode_ == UniversalMode::Aggressive ? "AGGR" : "SING";
}

String IrApp::getRemoteSendModeLabel()
{
    if (remoteSendMode_ == RemoteSendMode::Loop)
    {
        return "Loop";
    }
    if (remoteSendMode_ == RemoteSendMode::Delete)
    {
        return "Delete";
    }
    return "One";
}

String IrApp::getRemoteSendModeBadge()
{
    if (remoteSendMode_ == RemoteSendMode::Loop)
    {
        return "LOOP";
    }
    if (remoteSendMode_ == RemoteSendMode::Delete)
    {
        return "DEL";
    }
    return "ONE";
}

// ---- Queues ----------------------------------------------------------------------------
// Called from uiTask only. Snapshots everything the job depends on *now*, by value, so
// irTask works from what the user meant when they pressed, not from whatever the UI globals
// hold when the command is finally processed.
// Returns false if the command was not queued (the reason is on the status line), so callers
// that change UI state to "waiting for a result" only do so when a result will really come.
bool IrApp::queueCommand(IrCommandType type, int arg)
{
    if (!g_irCommandQueue)
    {
        return false;
    }

    IrCommand cmd;
    cmd.type = type;
    cmd.arg = arg;
    cmd.commandIndex = remoteCmdIndex_;
    cmd.commandCount = remoteCmdCount_;
    cmd.offset = 0;
    if (remoteCmdIndex_ >= 1 && remoteCmdIndex_ <= fileIndex_.count)
    {
        cmd.offset = fileIndex_.offsets[remoteCmdIndex_ - 1];
    }
    cmd.jobGeneration = g_sendGeneration.load();
    cmd.timing = irTimingForMode(universalMode_);
    cmd.name[0] = '\0';

    const String &path = (type == IrCommandType::UniversalSend) ? univProfilePath_ : selectedFilePath_;
    if (path.length() >= kMaxPathLen)
    {
        setStatus("Path too long");
        return false;
    }
    strlcpy(cmd.path, path.c_str(), sizeof(cmd.path));

    if (type == IrCommandType::UniversalSend && arg >= 0 && arg < univCmdCount_)
    {
        // A truncated name would match nothing in the profile, so refuse it up front.
        if (univCmdItems_[arg].length() >= kMaxNameLen)
        {
            setStatus("Name too long");
            return false;
        }
        strlcpy(cmd.name, univCmdItems_[arg].c_str(), sizeof(cmd.name));
    }

    if (xQueueSend(g_irCommandQueue, &cmd, pdMS_TO_TICKS(30)) != pdTRUE)
    {
        setStatus("Busy, try again");
        return false;
    }
    return true;
}

String IrApp::getFileBrowserFilterLabel()
{
    if (fileFilterIndex_ == 0)
    {
        return "ALL";
    }
    if (fileFilterIndex_ == 27)
    {
        return "#";
    }
    return String((char)('A' + (fileFilterIndex_ - 1)));
}

void IrApp::resetFileBrowserFilter()
{
    fileFilterIndex_ = 0;
}

void IrApp::cycleFileBrowserFilter()
{
    fileFilterIndex_++;
    if (fileFilterIndex_ > 27)
    {
        fileFilterIndex_ = 0;
    }
}

bool IrApp::matchesFileBrowserFilter(const char *entry)
{
    if (fileFilterIndex_ == 0)
    {
        return true;
    }
    char c = entry[0];
    if (c == '\0')
    {
        return false;
    }
    if (c >= 'a' && c <= 'z')
    {
        c = (char)(c - ('a' - 'A'));
    }
    if (fileFilterIndex_ == 27)
    {
        return c >= '0' && c <= '9';
    }
    return c == (char)('A' + (fileFilterIndex_ - 1));
}

// ---- Directory view ---------------------------------------------------------------------------
void IrApp::rebuildDirectoryCache(const String &path)
{
    storageListDirectory(path, dirCache_);
}

// Builds the filtered view (dirView_) from the cache, rebuilding the cache if needed.
void IrApp::loadDirectory(const String &path)
{
    dirViewCount_ = 0;

    if (!dirCache_.valid || path != dirCache_.path)
    {
        rebuildDirectoryCache(path);
    }
    if (!dirCache_.valid)
    {
        return;
    }

    if (path != "/")
    {
        dirView_[dirViewCount_++] = kDirViewBack;
    }
    for (int i = 0; i < dirCache_.count && dirViewCount_ < kMaxDirItems; i++)
    {
        if (matchesFileBrowserFilter(dirCache_.pool + dirCache_.nameOffset[i]))
        {
            dirView_[dirViewCount_++] = (uint16_t)i;
        }
    }
    menuIndex_ = 0;
}

bool IrApp::dirRowIsBack(int row)
{
    return dirView_[row] == kDirViewBack;
}

bool IrApp::dirRowIsFolder(int row)
{
    return dirRowIsBack(row) || dirCache_.isFolder[dirView_[row]];
}

const char *IrApp::dirRowName(int row)
{
    if (dirRowIsBack(row))
    {
        return ".. (Back)";
    }
    return dirCache_.pool + dirCache_.nameOffset[dirView_[row]];
}

String IrApp::dirRowLabel(int row)
{
    if (!dirRowIsBack(row) && dirRowIsFolder(row))
    {
        return "[DIR] " + String(dirRowName(row));
    }
    return String(dirRowName(row));
}

// ---- Universal profiles ------------------------------------------------------------------
bool IrApp::loadUniversalCommandList(const String &path)
{
    int count = storageListGroups(path, univCmdItems_, kMaxUnivItems);
    univCmdCount_ = count < 0 ? 0 : count;
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
