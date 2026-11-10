// ==========================================
// HELPER FUNCTIONS
// ==========================================
void VextON(void)
{
    pinMode(Vext, OUTPUT);
    digitalWrite(Vext, LOW);
}

void displayReset(void)
{
    pinMode(RST_OLED, OUTPUT);
    digitalWrite(RST_OLED, HIGH);
    delay(1);
    digitalWrite(RST_OLED, LOW);
    delay(1);
    digitalWrite(RST_OLED, HIGH);
    delay(1);
}

// ---- Locks -----------------------------------------------------------------------------
// g_stateMutex guards g_shared only: hold it for copies, never across SD, IR or drawing.
// The SD lock lives inside storage.cpp and is never held outside it.
void lockState(void)
{
    xSemaphoreTake(g_stateMutex, portMAX_DELAY);
}

void unlockState(void)
{
    xSemaphoreGive(g_stateMutex);
}

// ---- Shared state setters (any task) -------------------------------------------------
void setStatus(const String &msg)
{
    lockState();
    g_shared.status = msg;
    unlockState();
}

void setLearnPhase(LearnPhase phase)
{
    lockState();
    g_shared.learnPhase = phase;
    unlockState();
}

void setLearnDetails(const String &protocol, const String &detail)
{
    lockState();
    g_shared.learnProtocol = protocol;
    g_shared.learnDetail = detail;
    unlockState();
}

void setUniversalProgress(bool active, int current, int total, const String &name)
{
    lockState();
    g_shared.univSending = active;
    g_shared.univProgressCurrent = current;
    g_shared.univProgressTotal = total;
    g_shared.univProgressName = name;
    unlockState();
}

// irTask only. `name` is what is going out of the LED; pass "" when the send ends.
void markTransmitting(bool value, const String &name)
{
    lockState();
    g_shared.transmitting = value;
    g_shared.sendingName = name;
    unlockState();
}

// uiTask only. Name of the command the remote view has selected.
void setButtonName(const String &name)
{
    lockState();
    g_shared.btnName = name;
    unlockState();
}

UiSnapshot snapshotUi()
{
    UiSnapshot snap;
    lockState();
    snap.btnName = g_shared.btnName;
    snap.status = g_shared.status;
    snap.transmitting = g_shared.transmitting;
    snap.sendingName = g_shared.sendingName;
    snap.univSending = g_shared.univSending;
    snap.univProgressCurrent = g_shared.univProgressCurrent;
    snap.univProgressTotal = g_shared.univProgressTotal;
    snap.univProgressName = g_shared.univProgressName;
    snap.learnPhase = g_shared.learnPhase;
    snap.learnProtocol = g_shared.learnProtocol;
    snap.learnDetail = g_shared.learnDetail;
    unlockState();
    return snap;
}

bool uiSnapshotChanged(const UiSnapshot &a, const UiSnapshot &b)
{
    return a.btnName != b.btnName || a.status != b.status || a.transmitting != b.transmitting ||
           a.sendingName != b.sendingName ||
           a.univSending != b.univSending || a.univProgressCurrent != b.univProgressCurrent ||
           a.univProgressTotal != b.univProgressTotal || a.univProgressName != b.univProgressName ||
           a.learnPhase != b.learnPhase || a.learnProtocol != b.learnProtocol ||
           a.learnDetail != b.learnDetail;
}

LearnPhase getLearnPhaseSnapshot()
{
    lockState();
    LearnPhase phase = g_shared.learnPhase;
    unlockState();
    return phase;
}

// ---- Cancel ----------------------------------------------------------------------------
// Every command is stamped with the value of g_sendGeneration at the moment it is queued.
// Cancelling adds one to the counter. The running send job and every send job still waiting
// in the queue now carry an older number, see the mismatch, and stop.
//
// Why a counter and not a true/false flag: a flag has to be set back to false before the next
// job, and that "un-cancel" can land before irTask has looked at the flag, so the cancel is
// lost. A counter is never set back. A cancel cannot be lost, a new job cannot be cancelled by
// an old Back press, and only uiTask ever writes it.
//
// The queue is not emptied on cancel: stale send jobs are skipped when irTask takes them out,
// while LearnStop, LearnSave and a confirmed delete still run.
void cancelSendJobs()
{
    g_sendGeneration.fetch_add(1);
}

bool isJobCancelled(const IrCommand &cmd)
{
    return cmd.jobGeneration != g_sendGeneration.load();
}

// Called from irTask only. Delivers a result to uiTask; the UI owns every decision after this.
// irTask holds no lock here and uiTask empties the queue every frame, so a long wait is safe
// and a result is effectively never dropped.
void queueIrEvent(const IrEvent &evt)
{
    if (!g_irEventQueue)
    {
        return;
    }
    if (xQueueSend(g_irEventQueue, &evt, pdMS_TO_TICKS(2000)) != pdTRUE)
    {
        setStatus("UI busy, result lost");
    }
}

// ==========================================
// DIAGNOSTICS (serial only, no behaviour change)
// ==========================================
const char *resetReasonName(esp_reset_reason_t reason)
{
    switch (reason)
    {
        case ESP_RST_POWERON:
            return "power-on";
        case ESP_RST_EXT:
            return "external reset pin";
        case ESP_RST_SW:
            return "software restart";
        case ESP_RST_PANIC:
            return "PANIC (firmware crashed)";
        case ESP_RST_INT_WDT:
            return "INTERRUPT WATCHDOG";
        case ESP_RST_TASK_WDT:
            return "TASK WATCHDOG (a task hogged a core)";
        case ESP_RST_WDT:
            return "other watchdog";
        case ESP_RST_DEEPSLEEP:
            return "deep sleep wake";
        case ESP_RST_BROWNOUT:
            return "BROWNOUT (supply voltage dipped)";
        case ESP_RST_SDIO:
            return "SDIO";
        default:
            return "unknown";
    }
}

// Printed once at boot. The reset reason tells you *why* the previous run ended.
void logBootDiagnostics(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    Serial.printf("[boot] SecuriTea  reset reason: %s (%d)\n", resetReasonName(reason), (int)reason);
    Serial.printf("[boot] heap free=%u largest block=%u  chip=%s rev%d  cpu=%u MHz\n", ESP.getFreeHeap(),
                  ESP.getMaxAllocHeap(), ESP.getChipModel(), ESP.getChipRevision(), ESP.getCpuFreqMHz());
}

// Called from uiTask every loop when kSerialDiagnostics is on; prints every kDiagnosticsIntervalMs.
// "stack free" is the lowest free stack ever seen (high-water mark) in bytes. Near 0 = overflow soon.
// It reads memory counters only. It never touches the SD card.
void logRuntimeDiagnostics(void)
{
    static unsigned long lastLogMs = 0;
    unsigned long now = millis();
    if (now - lastLogMs < kDiagnosticsIntervalMs)
    {
        return;
    }
    lastLogMs = now;

    unsigned irStackFree = g_irTaskHandle ? (unsigned)uxTaskGetStackHighWaterMark(g_irTaskHandle) : 0;
    Serial.printf("[diag] t=%lus heap free=%u min=%u largest=%u | stack free ui=%u ir=%u | app=%s\n",
                  now / 1000, ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap(),
                  (unsigned)uxTaskGetStackHighWaterMark(NULL), irStackFree, g_launcher.current().name());
}
