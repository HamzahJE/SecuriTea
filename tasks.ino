// ==========================================
// FREERTOS TASKS
// ==========================================
// uiTask (core 1, prio 1) owns the screen, the joystick and the running app (see app.h).
// irTask (core 0, prio 2) owns the IR hardware (through ir_engine) and its own working payload.
// They talk only through g_irCommandQueue (UI -> IR) and g_irEventQueue (IR -> UI).

// ---- uiTask private state -------------------------------------------------------------
bool g_displayAsleep = false;
unsigned long g_lastInputMs = 0;

// Hands every result from irTask to the running app.
void drainIrEvents(App &app)
{
    IrEvent evt;
    while (g_irEventQueue && xQueueReceive(g_irEventQueue, &evt, 0) == pdTRUE)
    {
        app.onEvent(evt);
    }
}

// ---- Display power -----------------------------------------------------------------------
void wakeDisplay(unsigned long now, App &app)
{
    g_lastInputMs = now;
    if (g_displayAsleep)
    {
        g_display.displayOn();
        g_displayAsleep = false;
    }
    app.markDirty();
}

void sleepDisplayIfIdle(unsigned long now, const UiSnapshot &snap, const App &app)
{
    bool busy = app.keepsAwake(snap);
    if (!g_displayAsleep && !busy && now - g_lastInputMs >= kIdleDisplayOffMs)
    {
        g_display.displayOff();
        g_displayAsleep = true;
    }
}

// ---- uiTask --------------------------------------------------------------------------------
void uiTask(void *pvParameters)
{
    Joystick joystick; // input module: owns the pins, the debounce and the repeat timing
    joystick.begin();
    App &app = g_launcher.current();
    UiSnapshot lastDrawn;
    unsigned long lastDrawMs = 0;
    String lastStatus;
    unsigned long statusChangedMs = 0;
    g_lastInputMs = millis();

    while (true)
    {
        unsigned long now = millis();

        drainIrEvents(app);

        JoyInput in = joystick.read(now);
        if (in.any())
        {
            bool wasAsleep = g_displayAsleep;
            wakeDisplay(now, app);
            if (!wasAsleep)
            {
                app.onInput(in, now); // the first touch on a dark screen only wakes it
            }
        }

        app.tick(now); // state transitions and timeouts inside the app

        UiSnapshot snap = snapshotUi();
        sleepDisplayIfIdle(now, snap, app);

        // Most screens have no status line, so a changed status is shown in the footer for a
        // moment on every screen. The heartbeat redraw takes it away again.
        if (snap.status != lastStatus)
        {
            lastStatus = snap.status;
            statusChangedMs = now;
        }

        // Draw when something changed, or when kUiHeartbeatMs has passed (the footer marquee
        // needs the heartbeat). With 33 this is the original's redraw-every-frame behaviour.
        bool due = app.isDirty() || uiSnapshotChanged(snap, lastDrawn) || now - lastDrawMs >= kUiHeartbeatMs;
        if (!g_displayAsleep && due)
        {
            UiModel model;
            app.buildModel(model, snap);
            model.statusFresh = snap.status.length() > 0 && now - statusChangedMs < kStatusShowMs;
            drawScreen(model);
            lastDrawn = snap;
            lastDrawMs = now;
            app.clearDirty();
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
    evt.success = storageDeleteCommand(path, cmd.commandIndex, deletedName, fileRemoved, reason);
    evt.fileRemoved = fileRemoved;
    strlcpy(evt.text, evt.success ? deletedName.c_str() : (reason ? reason : "Delete failed"), sizeof(evt.text));
    queueIrEvent(evt); // uiTask decides the screen, the list and the index
}

void irUniversalSend(const IrCommand &cmd, IrPayload &payload)
{
    String profilePath = cmd.path;
    String groupName = cmd.name; // copied at press time; the group list belongs to the app
    if (groupName.length() == 0)
    {
        setStatus("Invalid group");
        return;
    }

    // g_irIndex is irTask's own index. A group such as "Power" can have hundreds of entries.
    int matchCount = storageIndexGroup(profilePath, groupName, g_irIndex);
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
                irListenStart();
                learn.listening = true;
            }
            else
            {
                irListenRearm();
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
                irListenStop();
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
        // queue until a command arrives.
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
