// ==========================================
// FREERTOS TASKS
// ==========================================
// uiTask (core 1, prio 1) owns the screen, the joystick and every UI global.
// irTask (core 0, prio 2) owns the IR LED, the receiver and its own working payload.
// They talk only through g_irCommandQueue (UI -> IR) and g_irEventQueue (IR -> UI).

// ---- uiTask private state -------------------------------------------------------------
bool g_uiDirty = true;             // something changed, draw on the next frame
bool g_displayAsleep = false;
unsigned long g_lastInputMs = 0;
bool g_deleteArmed = false;        // first press in Delete mode arms, second press deletes
unsigned long g_deleteArmedMs = 0;
bool g_deletePending = false;      // a delete is queued or running; its result has not arrived

// ---- Results from irTask --------------------------------------------------------------
// Runs on uiTask. uiTask owns g_state, the directory view and the command index, so only
// uiTask may change them in response to a delete.
void handleDeleteResult(const IrEvent &evt)
{
    g_deletePending = false;
    if (!evt.success)
    {
        setStatus(evt.text[0] ? String(evt.text) : String("Delete failed"));
        return;
    }

    g_dirCache.valid = false; // the folder listing may have changed
    String deletedName = evt.text;

    // The result is about evt.path. If the user has opened a different file since, that file's
    // index and counters are not ours to touch: refresh the listing and stop.
    if (g_selectedFilePath != evt.path)
    {
        if (g_state == AppState::FileBrowser)
        {
            loadDirectory(g_currentPath);
        }
        setStatus("Deleted " + deletedName);
        return;
    }

    int remaining = evt.fileRemoved ? 0 : buildCommandIndex(g_selectedFilePath, g_fileIndex);
    if (remaining <= 0)
    {
        g_remoteCmdCount = 0;
        g_fileIndex.count = 0;
        if (g_state == AppState::RemoteView)
        {
            g_state = AppState::FileBrowser;
        }
        if (g_state == AppState::FileBrowser)
        {
            loadDirectory(g_currentPath);
        }
        setStatus("Deleted " + deletedName + (evt.fileRemoved ? " (file empty)" : ""));
        return;
    }

    g_remoteCmdCount = remaining;
    if (g_remoteCmdIndex > g_remoteCmdCount)
    {
        g_remoteCmdIndex = g_remoteCmdCount;
    }
    if (g_remoteCmdIndex < 1)
    {
        g_remoteCmdIndex = 1;
    }
    showRemoteCommand(g_remoteCmdIndex);
    setStatus("Deleted " + deletedName);
}

void drainIrEvents()
{
    IrEvent evt;
    while (g_irEventQueue && xQueueReceive(g_irEventQueue, &evt, 0) == pdTRUE)
    {
        if (evt.type == IrEventType::DeleteResult)
        {
            handleDeleteResult(evt);
        }
        g_uiDirty = true;
    }
}

// ---- Input -----------------------------------------------------------------------------
// The joystick itself lives in input.h/.cpp (class Joystick); this section maps its events
// onto the current screen.
int menuLengthForState()
{
    switch (g_state)
    {
        case AppState::MainMenu:
            return kMainMenuLen;
        case AppState::UniversalMenu:
            return kUnivMenuLen;
        case AppState::UniversalRemote:
            return g_univCmdCount;
        case AppState::FileBrowser:
            return g_dirViewCount;
        default:
            return 0;
    }
}

void disarmDelete()
{
    g_deleteArmed = false;
}

void showRemoteCommand(int index)
{
    if (index < 1 || index > g_fileIndex.count)
    {
        return;
    }
    g_remoteCmdIndex = index;
    publishCommandAt(g_selectedFilePath, g_fileIndex.offsets[index - 1]);
}

void handleScroll(bool up)
{
    int step = up ? -1 : 1;

    if (g_state == AppState::UniversalMenu)
    {
        g_headerModeSelected = false;
    }

    if (g_state == AppState::RemoteView)
    {
        g_remoteModeSelected = false;
        disarmDelete();
        if (g_deletePending)
        {
            return; // the command numbers are about to change; wait for the result
        }
        int next = g_remoteCmdIndex + step;
        if (next >= 1 && next <= g_remoteCmdCount)
        {
            showRemoteCommand(next);
        }
        return;
    }

    int len = menuLengthForState();
    if (len <= 0)
    {
        g_menuIndex = 0;
        return;
    }
    if (g_state == AppState::FileBrowser)
    {
        g_menuIndex = constrain(g_menuIndex + step, 0, len - 1); // lists do not wrap
    }
    else
    {
        g_menuIndex = (g_menuIndex + step + len) % len; // menus wrap
    }
}

void handleRight()
{
    if (g_state == AppState::UniversalMenu)
    {
        g_headerModeSelected = true;
    }
    else if (g_state == AppState::FileBrowser)
    {
        cycleFileBrowserFilter();
        loadDirectory(g_currentPath);
        setStatus("Filter: " + getFileBrowserFilterLabel());
    }
    else if (g_state == AppState::RemoteView)
    {
        g_remoteModeSelected = true;
        disarmDelete();
    }
}

void handleLeft()
{
    if (g_state == AppState::UniversalMenu && g_headerModeSelected)
    {
        g_headerModeSelected = false;
    }
    else if (g_state == AppState::RemoteView && g_remoteModeSelected)
    {
        g_remoteModeSelected = false;
    }
    else if (g_state == AppState::FileBrowser && g_fileFilterIndex != 0)
    {
        resetFileBrowserFilter();
        loadDirectory(g_currentPath);
        setStatus("Filter: ALL");
    }
    else if (g_state == AppState::UniversalMenu || g_state == AppState::Learn || g_state == AppState::FileBrowser)
    {
        g_state = AppState::MainMenu;
        g_menuIndex = 0;
    }
    else if (g_state == AppState::UniversalRemote)
    {
        cancelSendJobs(); // irTask clears the progress popup when its job stops
        g_state = AppState::UniversalMenu;
        g_menuIndex = 0;
    }
    else if (g_state == AppState::RemoteView)
    {
        cancelSendJobs(); // stops a running loop send and skips queued ones
        disarmDelete();
        g_state = AppState::FileBrowser;
    }
}

void handlePressMainMenu()
{
    if (g_menuIndex == 0)
    {
        g_state = AppState::UniversalMenu;
    }
    else if (g_menuIndex == 1)
    {
        g_state = AppState::FileBrowser;
        g_currentPath = "/";
        resetFileBrowserFilter();
        g_dirCache.valid = false;
        loadDirectory(g_currentPath);
    }
    else if (g_menuIndex == 2)
    {
        g_state = AppState::Learn;
    }
    g_menuIndex = 0;
}

void handlePressUniversalMenu()
{
    if (g_headerModeSelected)
    {
        g_universalMode =
            (g_universalMode == UniversalMode::Single) ? UniversalMode::Aggressive : UniversalMode::Single;
        setStatus("Mode: " + getUniversalModeLabel());
        g_headerModeSelected = false;
        return;
    }

    static const char *const kTargets[] = {"TV", "AC", "Proj", "Audio"};
    g_targetDevice = kTargets[constrain(g_menuIndex, 0, kUnivMenuLen - 1)];
    g_univProfilePath = bruteProfileForTarget(g_targetDevice);
    if (loadUniversalCommandList(g_univProfilePath))
    {
        setStatus("Select command"); // on failure the loader's own message stays
    }

    g_state = AppState::UniversalRemote;
    g_menuIndex = 0;
}

void handlePressUniversalRemote()
{
    if (g_univCmdCount <= 0)
    {
        setStatus("No commands loaded");
        return;
    }
    queueCommand(IrCommandType::UniversalSend, g_menuIndex);
}

void openSelectedFile()
{
    const char *name = dirRowName(g_menuIndex);
    String fullPath = joinPath(g_currentPath, name);
    if (fullPath.length() >= kMaxPathLen)
    {
        setStatus("Path too long");
        return;
    }

    int count = buildCommandIndex(fullPath, g_fileIndex);
    if (count <= 0)
    {
        setStatus("No commands in file");
        return;
    }

    g_selectedFilePath = fullPath;
    g_selectedFileName = name;
    g_remoteCmdCount = count;
    g_remoteCmdIndex = 1;
    showRemoteCommand(1);
    g_state = AppState::RemoteView;
}

void handlePressFileBrowser()
{
    if (g_dirViewCount == 0)
    {
        setStatus("Folder empty");
        return;
    }

    if (!dirRowIsFolder(g_menuIndex))
    {
        openSelectedFile();
        return;
    }

    if (dirRowIsBack(g_menuIndex))
    {
        g_currentPath = getParentDir(g_currentPath);
    }
    else
    {
        g_currentPath = joinPath(g_currentPath, dirRowName(g_menuIndex));
        resetFileBrowserFilter(); // first-letter filter applies to the current level only
    }
    loadDirectory(g_currentPath);
}

void handlePressRemoteView(unsigned long now)
{
    if (g_remoteModeSelected)
    {
        if (g_remoteSendMode == RemoteSendMode::One)
        {
            g_remoteSendMode = RemoteSendMode::Loop;
        }
        else if (g_remoteSendMode == RemoteSendMode::Loop)
        {
            g_remoteSendMode = RemoteSendMode::Delete;
        }
        else
        {
            g_remoteSendMode = RemoteSendMode::One;
        }
        setStatus("Remote Mode: " + getRemoteSendModeLabel());
        g_remoteModeSelected = false;
        disarmDelete();
        return;
    }

    // A delete renumbers the commands. Until its result arrives the index on screen may be
    // stale, so nothing that names a command by number or offset is queued.
    if (g_deletePending)
    {
        setStatus("Deleting, please wait");
        return;
    }

    if (g_remoteSendMode == RemoteSendMode::Loop)
    {
        queueCommand(IrCommandType::RemoteSendAll, 0);
    }
    else if (g_remoteSendMode == RemoteSendMode::One)
    {
        queueCommand(IrCommandType::RemoteSendOne, 0);
    }
    else if (g_deleteArmed && now - g_deleteArmedMs <= kDeleteConfirmWindowMs)
    {
        disarmDelete();
        g_deletePending = queueCommand(IrCommandType::RemoteDelete, 0);
    }
    else
    {
        g_deleteArmed = true;
        g_deleteArmedMs = now;
        setStatus("Press again to delete");
    }
}

void handlePressLearn()
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

void handlePress(unsigned long now)
{
    switch (g_state)
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

void handleInput(const JoyInput &in, unsigned long now)
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

void onStateChanged(AppState previous, AppState next)
{
    if (next == AppState::Learn)
    {
        queueCommand(IrCommandType::LearnStart, 0);
        setStatus("Point remote and press key");
    }
    if (next == AppState::UniversalMenu)
    {
        g_headerModeSelected = false;
    }
    if (next == AppState::RemoteView)
    {
        g_remoteModeSelected = false;
        disarmDelete();
    }
    if (previous == AppState::Learn)
    {
        queueCommand(IrCommandType::LearnStop, 0);
    }
}

// ---- Display power -----------------------------------------------------------------------
void wakeDisplay(unsigned long now)
{
    g_lastInputMs = now;
    if (g_displayAsleep)
    {
        g_display.displayOn();
        g_displayAsleep = false;
    }
    g_uiDirty = true;
}

void sleepDisplayIfIdle(unsigned long now, const UiSnapshot &snap)
{
    bool busy = g_state == AppState::Learn || snap.univSending || snap.transmitting;
    if (!g_displayAsleep && !busy && now - g_lastInputMs >= kIdleDisplayOffMs)
    {
        g_display.displayOff();
        g_displayAsleep = true;
    }
}

// ---- Screens ----------------------------------------------------------------------------
void drawFileBrowserScreen()
{
    String title = "Path: " + g_currentPath;
    if (g_dirViewCount == 0)
    {
        g_display.clear();
        drawHeader(title, getFileBrowserFilterLabel());
        g_display.drawString(0, kUiLine1Y + 4, "(No items)");
        drawFooter("[>]Filter [<]Clr/Back");
        g_display.display();
        return;
    }

    // Build only the rows on screen, not all 260.
    int start = listWindowStart(g_menuIndex, g_dirViewCount);
    String rows[kMenuVisibleRows];
    int shown = 0;
    for (int i = 0; i < kMenuVisibleRows && start + i < g_dirViewCount; i++)
    {
        rows[shown++] = dirRowLabel(start + i);
    }
    drawMenu(title, rows, shown, g_menuIndex - start, "[>]Filter [BTN]Open [<]Clr/Back",
             getFileBrowserFilterLabel());
}

void drawUniversalRemoteScreen(const UiSnapshot &snap)
{
    String title = g_targetDevice + " Remote";
    if (g_univCmdCount <= 0)
    {
        g_display.clear();
        drawHeader(title, getUniversalModeBadge());
        g_display.drawString(0, kUiLine1Y + 4, "No commands found");
        g_display.drawString(0, kUiLine2Y + 4, "File: " + g_univProfilePath);
        drawFooter("[<] Back");
        g_display.display();
        return;
    }

    drawMenu(title, g_univCmdItems, g_univCmdCount, g_menuIndex, "[<] Back | [BTN] Select",
             getUniversalModeBadge(), false);
    if (snap.univSending)
    {
        drawUniversalProgressPopup(snap.univProgressName, snap.univProgressCurrent, snap.univProgressTotal);
    }
    g_display.display();
}

void drawRemoteViewScreen(const UiSnapshot &snap)
{
    g_display.clear();
    drawHeader("<- Back   Blaster", getRemoteSendModeBadge(), g_remoteModeSelected);
    g_display.drawString(0, kUiLine1Y, "File: " + g_selectedFileName);
    // While a send is running, show the name going out (irTask's field). Otherwise show the
    // selected command (uiTask's field). The two never overwrite each other.
    bool sending = snap.transmitting && snap.sendingName.length() > 0;
    g_display.drawString(0, kUiLine2Y, sending ? "Tx: " + snap.sendingName : "Btn: " + snap.btnName);
    g_display.drawString(0, kUiLine3Y, "Cmd: " + String(g_remoteCmdIndex) + "/" + String(g_remoteCmdCount));

    if (g_deletePending)
    {
        drawFooter("[ DELETING... ]");
    }
    else if (snap.transmitting)
    {
        drawFooter("[ TRANSMITTING... ]");
    }
    else if (g_remoteSendMode == RemoteSendMode::Delete)
    {
        drawFooter(g_deleteArmed ? "[BTN] Confirm delete" : "[v^]Cmd [>]Mode [BTN]Delete");
    }
    else
    {
        drawFooter("[v^]Cmd [>]Mode [BTN]Send");
    }
    g_display.display();
}

void drawLearnScreen(const UiSnapshot &snap)
{
    g_display.clear();
    drawHeader("Learn New Remote");
    g_display.drawString(0, kUiLine1Y, snap.status);

    switch (snap.learnPhase)
    {
        case LearnPhase::Listening:
            g_display.drawString(0, kUiLine2Y, "Waiting for signal...");
            drawFooter("[<] Back");
            break;
        case LearnPhase::Captured:
        {
            String proto = snap.learnProtocol.length() ? snap.learnProtocol : "UNKNOWN";
            g_display.drawString(0, kUiLine2Y, trimToWidth("Proto: " + proto, 21));
            if (snap.learnDetail.length())
            {
                g_display.drawString(0, kUiLine3Y, trimToWidth(snap.learnDetail, 21));
            }
            drawFooter("[BTN] Save | [<] Back");
            break;
        }
        case LearnPhase::Saved:
            g_display.drawString(0, kUiLine2Y, "Saved to /captures/");
            if (snap.learnProtocol.length())
            {
                g_display.drawString(0, kUiLine3Y, trimToWidth("Proto: " + snap.learnProtocol, 21));
            }
            drawFooter("[BTN] Capture Again");
            break;
        case LearnPhase::Error:
            g_display.drawString(0, kUiLine2Y, "Capture error");
            drawFooter("[BTN] Retry | [<] Back");
            break;
        default:
            g_display.drawString(0, kUiLine2Y, "Preparing listener...");
            drawFooter("[<] Back");
            break;
    }
    g_display.display();
}

void drawScreen(const UiSnapshot &snap)
{
    switch (g_state)
    {
        case AppState::MainMenu:
            drawMenu("SecuriTea OS", kMainMenu, kMainMenuLen, g_menuIndex);
            break;
        case AppState::UniversalMenu:
            drawMenu("Universal Remote", kUnivMenu, kUnivMenuLen, g_menuIndex,
                     "[v^]Scroll [>]Mode [BTN]Toggle/Pick [<]Back", getUniversalModeBadge(), true,
                     g_headerModeSelected);
            break;
        case AppState::FileBrowser:
            drawFileBrowserScreen();
            break;
        case AppState::UniversalRemote:
            drawUniversalRemoteScreen(snap);
            break;
        case AppState::RemoteView:
            drawRemoteViewScreen(snap);
            break;
        case AppState::Learn:
            drawLearnScreen(snap);
            break;
    }
}

// ---- uiTask --------------------------------------------------------------------------------
void uiTask(void *pvParameters)
{
    Joystick joystick; // input module: owns the pins, the debounce and the repeat timing
    joystick.begin();
    AppState previousState = g_state;
    UiSnapshot lastDrawn;
    unsigned long lastDrawMs = 0;
    g_lastInputMs = millis();

    while (true)
    {
        unsigned long now = millis();

        drainIrEvents();

        JoyInput in = joystick.read(now);
        if (in.any())
        {
            bool wasAsleep = g_displayAsleep;
            wakeDisplay(now);
            if (!wasAsleep)
            {
                handleInput(in, now); // the first touch on a dark screen only wakes it
            }
        }

        if (previousState != g_state)
        {
            onStateChanged(previousState, g_state);
            previousState = g_state;
            g_uiDirty = true;
        }

        if (g_deleteArmed && now - g_deleteArmedMs > kDeleteConfirmWindowMs)
        {
            disarmDelete();
            g_uiDirty = true;
        }

        UiSnapshot snap = snapshotUi();
        sleepDisplayIfIdle(now, snap);

        // Draw when something changed, or when kUiHeartbeatMs has passed (the footer marquee
        // needs the heartbeat). With 33 this is the original's redraw-every-frame behaviour.
        bool due = g_uiDirty || uiSnapshotChanged(snap, lastDrawn) || now - lastDrawMs >= kUiHeartbeatMs;
        if (!g_displayAsleep && due)
        {
            drawScreen(snap);
            lastDrawn = snap;
            lastDrawMs = now;
            g_uiDirty = false;
        }

        sdMountRetryTick(now); // only does anything while the card is not mounted
        if (kSerialDiagnostics)
        {
            logRuntimeDiagnostics();
        }
        vTaskDelay(pdMS_TO_TICKS(kUiFrameMs));
    }
}

// ---- irTask jobs ---------------------------------------------------------------------------
// Sends a loaded payload cmd.timing.transmitRepeats times. Stops between repeats if the job was
// cancelled. Returns true if at least one send went out.
bool transmitRepeated(const IrPayload &payload, const IrCommand &cmd)
{
    const IrTiming &t = cmd.timing;
    bool sentOnce = false;
    for (uint8_t rep = 0; rep < t.transmitRepeats; rep++)
    {
        if (isJobCancelled(cmd))
        {
            break;
        }
        if (transmitPayload(payload, t))
        {
            sentOnce = true;
        }
        if (rep + 1 < t.transmitRepeats)
        {
            vTaskDelay(pdMS_TO_TICKS(t.repeatGapMs));
        }
    }
    return sentOnce;
}

// One mode. The command carries the file and the byte offset the user had selected when they
// pressed, so irTask loads and sends exactly that command whatever the screen shows by now.
void irSendOne(const IrCommand &cmd, IrPayload &payload)
{
    if (cmd.path[0] == '\0' || cmd.commandCount <= 0)
    {
        setStatus("No command selected");
        return;
    }
    if (!loadFlipperCommandAt(cmd.path, cmd.offset, payload))
    {
        return; // the loader already put the reason on the status line
    }
    transmitPayload(payload, cmd.timing);
}

void irSendAll(const IrCommand &cmd, IrPayload &payload)
{
    String path = cmd.path;
    if (path.length() == 0 || cmd.commandCount <= 0)
    {
        setStatus("No file commands");
        return;
    }

    int count = buildCommandIndex(path, g_irIndex);
    if (count <= 0)
    {
        setStatus("No file commands");
        return;
    }

    int sentCount = 0;
    bool cancelled = false;
    for (int i = 0; i < count; i++)
    {
        if (isJobCancelled(cmd))
        {
            cancelled = true;
            break;
        }
        if (!loadFlipperCommandAt(path, g_irIndex.offsets[i], payload))
        {
            vTaskDelay(1); // a file of bad commands must still let the idle task run
            continue;
        }
        if (transmitRepeated(payload, cmd))
        {
            sentCount++;
        }
        vTaskDelay(pdMS_TO_TICKS(cmd.timing.sendDelayMs));
    }

    setStatus(cancelled ? "Canceled loop" : "Loop Done " + String(sentCount) + "/" + String(count));
}

void irDeleteCommand(const IrCommand &cmd)
{
    String path = cmd.path; // captured when the button was pressed
    IrEvent evt;
    evt.type = IrEventType::DeleteResult;
    evt.success = false;
    evt.fileRemoved = false;
    evt.text[0] = '\0';
    strlcpy(evt.path, cmd.path, sizeof(evt.path));

    if (path.length() == 0 || cmd.commandIndex <= 0)
    {
        strlcpy(evt.text, "No command selected", sizeof(evt.text));
        queueIrEvent(evt);
        return;
    }

    String deletedName;
    bool fileRemoved = false;
    const char *reason = nullptr;
    evt.success = deleteFlipperCommandByIndex(path, cmd.commandIndex, deletedName, fileRemoved, reason);
    evt.fileRemoved = fileRemoved;
    strlcpy(evt.text, evt.success ? deletedName.c_str() : (reason ? reason : "Delete failed"), sizeof(evt.text));
    queueIrEvent(evt); // uiTask decides the screen, the list and the index
}

void irUniversalSend(const IrCommand &cmd, IrPayload &payload)
{
    String profilePath = cmd.path;
    String groupName = cmd.name; // copied at press time; g_univCmdItems is UI-owned
    if (groupName.length() == 0)
    {
        setStatus("Invalid group");
        return;
    }

    // g_irIndex is irTask's own index. A group such as "Power" can have hundreds of entries.
    int matchCount = buildGroupIndex(profilePath, groupName, g_irIndex);
    if (matchCount <= 0)
    {
        setStatus("No cmds for " + groupName);
        setUniversalProgress(false, 0, 0, "");
        return;
    }

    int sentCount = 0;
    setUniversalProgress(true, 0, matchCount, groupName);
    for (int i = 0; i < matchCount; i++)
    {
        if (isJobCancelled(cmd))
        {
            break;
        }

        bool loaded = false;
        for (uint8_t attempt = 0; attempt < cmd.timing.loadRetries && !loaded; attempt++)
        {
            loaded = loadFlipperCommandAt(profilePath, g_irIndex.offsets[i], payload);
            if (!loaded)
            {
                vTaskDelay(pdMS_TO_TICKS(15));
            }
        }
        if (!loaded)
        {
            continue;
        }

        setUniversalProgress(true, i + 1, matchCount, groupName);
        if (transmitRepeated(payload, cmd))
        {
            sentCount++;
        }
        vTaskDelay(pdMS_TO_TICKS(cmd.timing.sendDelayMs));
    }

    // irTask is the only writer of the progress fields. Jobs run one after another, so this
    // clear always lands before the next job sets its own progress.
    setUniversalProgress(false, 0, 0, "");
    if (isJobCancelled(cmd))
    {
        setStatus("Canceled " + groupName);
    }
    else
    {
        setStatus("Done " + groupName + " " + String(sentCount) + "/" + String(matchCount));
    }
}

bool isSendJob(IrCommandType type)
{
    return type == IrCommandType::RemoteSendOne || type == IrCommandType::RemoteSendAll ||
           type == IrCommandType::UniversalSend;
}

void handleIrCommand(const IrCommand &cmd, IrPayload &payload, LearnSession &learn)
{
    if (isSendJob(cmd.type) && isJobCancelled(cmd))
    {
        return; // queued before the user backed out: they no longer want it
    }

    switch (cmd.type)
    {
        case IrCommandType::RemoteSendOne:
            irSendOne(cmd, payload);
            break;
        case IrCommandType::RemoteSendAll:
            irSendAll(cmd, payload);
            break;
        case IrCommandType::RemoteDelete:
            irDeleteCommand(cmd);
            break;
        case IrCommandType::UniversalSend:
            irUniversalSend(cmd, payload);
            break;
        case IrCommandType::LearnStart:
            // enableIRIn() allocates a hardware timer every time it is called and the chip has
            // four. Call it once per listening session; "capture again" only re-arms.
            if (!learn.listening)
            {
                g_irrecv.enableIRIn();
                learn.listening = true;
            }
            else
            {
                g_irrecv.resume();
            }
            learn.captured = false;
            setLearnPhase(LearnPhase::Listening);
            setLearnDetails("", "");
            setStatus("Listening...");
            break;
        case IrCommandType::LearnStop:
            // disableIRIn() frees that timer; calling it twice would free it twice.
            if (learn.listening)
            {
                g_irrecv.disableIRIn();
                learn.listening = false;
            }
            learn.captured = false;
            setLearnPhase(LearnPhase::Idle);
            setLearnDetails("", "");
            break;
        case IrCommandType::LearnSave:
            if (learn.captured)
            {
                saveLearnedCommandToSD(learn.signal);
            }
            else
            {
                setStatus("Nothing to save");
                setLearnPhase(LearnPhase::Error);
            }
            break;
    }
}

// ---- irTask ----------------------------------------------------------------------------------
void irTask(void *pvParameters)
{
    LearnSession learn; // receiver state and the frozen capture; never shared with the UI
    IrPayload payload;  // working copy for send jobs; never shared with the UI

    while (true)
    {
        // While learning, wake every kIrPollMs to poll the receiver. Otherwise block on the
        // queue until a command arrives (step 2 of the timing changes, under board test since
        // 2026-11-09; the original woke every 15 ms always).
        TickType_t wait = learn.listening ? pdMS_TO_TICKS(kIrPollMs) : portMAX_DELAY;
        IrCommand cmd;
        if (xQueueReceive(g_irCommandQueue, &cmd, wait) == pdTRUE)
        {
            handleIrCommand(cmd, payload, learn);
        }
        // Once a signal is captured it is frozen until the user asks to capture again, so a
        // held key or a stray remote cannot replace what the screen is describing.
        if (learn.listening && !learn.captured)
        {
            learn.captured = captureLearnSignal(learn.signal);
        }
    }
}
