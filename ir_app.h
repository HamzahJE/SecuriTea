#pragma once
// The infrared app: universal remote, file browser with remote view, and learn mode.
//
// Everything that used to be a UI-owned global is a member here. uiTask is the only caller,
// so the members need no lock. irTask never sees this object: it receives self-contained
// IrCommand structs through the queue and answers through IrEvent.

#include "app.h"

class IrApp : public App
{
public:
    const char *name() const override
    {
        return "Infrared";
    }
    void onInput(const JoyInput &in, unsigned long now) override;
    void onEvent(const IrEvent &evt) override;
    void tick(unsigned long now) override;
    bool keepsAwake(const UiSnapshot &snap) const override;
    bool isDirty() const override
    {
        return uiDirty_;
    }
    void markDirty() override
    {
        uiDirty_ = true;
    }
    void clearDirty() override
    {
        uiDirty_ = false;
    }
    void buildModel(UiModel &m, const UiSnapshot &snap) override;

private:
    // ---- Results from irTask
    void handleDeleteResult(const IrEvent &evt);

    // ---- Input
    int menuLengthForState();
    void disarmDelete();
    void showRemoteCommand(int index);
    void handleScroll(bool up);
    void handleRight();
    void handleLeft();
    void handlePressMainMenu();
    void handlePressUniversalMenu();
    void handlePressUniversalRemote();
    void openSelectedFile();
    void handlePressFileBrowser();
    void handlePressRemoteView(unsigned long now);
    void handlePressLearn();
    void handlePress(unsigned long now);
    void onStateChanged(AppState previous, AppState next);

    // ---- Commands to irTask
    bool queueCommand(IrCommandType type, int arg);

    // ---- Labels
    String getUniversalModeLabel();
    String getUniversalModeBadge();
    String getRemoteSendModeLabel();
    String getRemoteSendModeBadge();

    // ---- Directory view
    void rebuildDirectoryCache(const String &path);
    void loadDirectory(const String &path);
    bool dirRowIsBack(int row);
    bool dirRowIsFolder(int row);
    const char *dirRowName(int row);
    String dirRowLabel(int row);
    String getFileBrowserFilterLabel();
    void resetFileBrowserFilter();
    void cycleFileBrowserFilter();
    bool matchesFileBrowserFilter(const char *entry);

    // ---- Universal profiles
    bool loadUniversalCommandList(const String &path);

    // ---- State (uiTask only)
    AppState state_ = AppState::MainMenu;
    AppState previousState_ = AppState::MainMenu;
    int menuIndex_ = 0;
    bool uiDirty_ = true;

    String univCmdItems_[kMaxUnivItems];
    int univCmdCount_ = 0;
    String univProfilePath_ = "/univ_tv.ir";
    String targetDevice_ = "TV";
    UniversalMode universalMode_ = UniversalMode::Single;
    bool headerModeSelected_ = false;

    // File browser: one cache of the directory, one view of it after filtering.
    DirCache dirCache_;
    uint16_t dirView_[kMaxDirItems] = {};
    int dirViewCount_ = 0;
    String currentPath_ = "/";
    int fileFilterIndex_ = 0; // 0=ALL, 1-26=A-Z, 27=#

    // Remote view: the open file and its command index.
    String selectedFilePath_ = "";
    String selectedFileName_ = "";
    CommandIndex fileIndex_;
    int remoteCmdIndex_ = 1;
    int remoteCmdCount_ = 0;
    RemoteSendMode remoteSendMode_ = RemoteSendMode::One;
    bool remoteModeSelected_ = false;
    bool deleteArmed_ = false; // first press in Delete mode arms, second press deletes
    unsigned long deleteArmedMs_ = 0;
    bool deletePending_ = false; // a delete is queued or running; its result has not arrived
};
