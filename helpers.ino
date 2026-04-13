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

void lockState(void)
{
    xSemaphoreTake(stateMutex, portMAX_DELAY);
}

void unlockState(void)
{
    xSemaphoreGive(stateMutex);
}

void lockSD(void)
{
    xSemaphoreTake(sdMutex, portMAX_DELAY);
}

void unlockSD(void)
{
    xSemaphoreGive(sdMutex);
}

void setStatus(const String &msg)
{
    lockState();
    shared.status = msg;
    status_message = msg;
    unlockState();
}

void setLearnPhase(LearnPhase phase)
{
    lockState();
    shared.learnPhase = phase;
    unlockState();
}

void setBruteStatus(bool active, int idx, const String &profilePath)
{
    lockState();
    shared.bruteActive = active;
    shared.bruteIndex = idx;
    shared.bruteProfilePath = profilePath;
    unlockState();
}

void setUniversalProgress(bool active, int current, int total, const String &name)
{
    lockState();
    shared.univSending = active;
    shared.univProgressCurrent = current;
    shared.univProgressTotal = total;
    shared.univProgressName = name;
    unlockState();
}

void requestUniversalCancel(bool value)
{
    lockState();
    univ_cancel_requested = value;
    unlockState();
}

bool isUniversalCancelRequested()
{
    bool v;
    lockState();
    v = univ_cancel_requested;
    unlockState();
    return v;
}

String getUniversalModeLabel()
{
    if (universal_mode == UNIV_MODE_AGGRESSIVE)
        return "Aggressive";
    return "Single";
}

String getUniversalModeBadge()
{
    if (universal_mode == UNIV_MODE_AGGRESSIVE)
        return "AGGR";
    return "SING";
}

void applyUniversalModeSettings()
{
    if (universal_mode == UNIV_MODE_AGGRESSIVE)
    {
        currentSendDelayMs = AGGR_SEND_DELAY_MS;
        currentTransmitRepeats = AGGR_TRANSMIT_REPEATS;
        currentLoadRetries = AGGR_LOAD_RETRIES;
        currentRepeatGapMs = AGGR_REPEAT_GAP_MS;
        currentProtocolFrameRepeat = AGGR_PROTOCOL_FRAME_REPEAT;
    }
    else
    {
        currentSendDelayMs = SINGLE_SEND_DELAY_MS;
        currentTransmitRepeats = SINGLE_TRANSMIT_REPEATS;
        currentLoadRetries = SINGLE_LOAD_RETRIES;
        currentRepeatGapMs = SINGLE_REPEAT_GAP_MS;
        currentProtocolFrameRepeat = SINGLE_PROTOCOL_FRAME_REPEAT;
    }
}

void setPayload(const uint16_t *raw, uint16_t len, uint16_t freqKHz, const String &btnName)
{
    uint16_t safeLen = min((uint16_t)MAX_RAW_BUFFER, len);

    lockState();
    for (uint16_t i = 0; i < safeLen; i++)
    {
        shared.raw[i] = raw[i];
        capturedRaw[i] = raw[i];
    }
    shared.rawLen = safeLen;
    shared.freqKHz = freqKHz;
    shared.payloadIsParsed = false;
    shared.parsedProtocol = "";
    shared.parsedAddress = 0;
    shared.parsedCommand = 0;
    shared.btnName = btnName;
    shared.hasPayload = safeLen > 0;

    capturedRawLen = safeLen;
    current_frequency = freqKHz;
    current_btn_name = btnName;
    unlockState();
}

void setParsedPayload(const String &protocol, uint32_t address, uint32_t command, const String &btnName)
{
    lockState();
    shared.rawLen = 0;
    shared.freqKHz = 38;
    shared.payloadIsParsed = true;
    shared.parsedProtocol = protocol;
    shared.parsedAddress = address;
    shared.parsedCommand = command;
    shared.btnName = btnName;
    shared.hasPayload = true;

    capturedRawLen = 0;
    current_frequency = 38;
    current_btn_name = btnName;
    unlockState();
}

bool copyPayload(uint16_t *outRaw, uint16_t &outLen, uint16_t &outFreqKHz, String &outBtnName)
{
    bool ok = false;

    lockState();
    if (shared.hasPayload && shared.rawLen > 0)
    {
        outLen = shared.rawLen;
        outFreqKHz = shared.freqKHz;
        outBtnName = shared.btnName;
        for (uint16_t i = 0; i < outLen; i++)
            outRaw[i] = shared.raw[i];
        ok = true;
    }
    unlockState();

    return ok;
}

int parseHexByte(const String &s)
{
    const char *text = s.c_str();
    return (int)strtol(text, NULL, 16);
}

uint32_t parseFlipperHex32(const String &field)
{
    String tmp = field;
    tmp.trim();

    uint32_t bytes[4] = {0, 0, 0, 0};
    int idx = 0;
    int start = 0;

    while (start < tmp.length() && idx < 4)
    {
        int end = tmp.indexOf(' ', start);
        String token;
        if (end == -1)
        {
            token = tmp.substring(start);
            start = tmp.length();
        }
        else
        {
            token = tmp.substring(start, end);
            start = end + 1;
            while (start < tmp.length() && tmp.charAt(start) == ' ')
                start++;
        }

        token.trim();
        if (token.length() > 0)
            bytes[idx++] = (uint32_t)(parseHexByte(token) & 0xFF);
    }

    return (bytes[0]) | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
}

void queueCommand(IrCommandType type, int arg = 0)
{
    if (!irCommandQueue)
        return;
    IrCommand cmd;
    cmd.type = type;
    cmd.arg = arg;
    xQueueSend(irCommandQueue, &cmd, pdMS_TO_TICKS(30));
}

String bruteProfileForTarget(const String &target)
{
    if (target == "AC")
        return "/univ_ac.ir";
    if (target == "Proj")
        return "/univ_proj.ir";
    if (target == "Audio")
        return "/univ_audio.ir";
    return "/univ_tv.ir";
}

String bruteProfileForCode(int deviceCode)
{
    if (deviceCode == 1)
        return "/univ_ac.ir";
    if (deviceCode == 2)
        return "/univ_proj.ir";
    return "/univ_tv.ir";
}

void setSelectedFileName(const String &name)
{
    lockState();
    shared.selectedFile = name;
    selected_file_name = name;
    unlockState();
}

void markTransmitting(bool value)
{
    lockState();
    shared.transmitting = value;
    unlockState();
}

UiSnapshot snapshotUi()
{
    UiSnapshot snap;
    lockState();
    snap.btnName = shared.btnName;
    snap.status = shared.status;
    snap.selectedFile = shared.selectedFile;
    snap.bruteActive = shared.bruteActive;
    snap.bruteIndex = shared.bruteIndex;
    snap.transmitting = shared.transmitting;
    snap.univSending = shared.univSending;
    snap.univProgressCurrent = shared.univProgressCurrent;
    snap.univProgressTotal = shared.univProgressTotal;
    snap.univProgressName = shared.univProgressName;
    snap.learnPhase = shared.learnPhase;
    unlockState();
    return snap;
}

LearnPhase getLearnPhaseSnapshot()
{
    LearnPhase phase;
    lockState();
    phase = shared.learnPhase;
    unlockState();
    return phase;
}

String getUniversalProfilePathSnapshot()
{
    String profile;
    lockState();
    profile = shared.bruteProfilePath;
    unlockState();
    return profile;
}

String getBaseName(String path)
{
    int lastSlash = path.lastIndexOf('/');
    if (lastSlash >= 0)
        return path.substring(lastSlash + 1);
    return path;
}

String getParentDir(String path)
{
    if (path == "/" || path == "")
        return "/";
    int lastSlash = path.lastIndexOf('/');
    if (lastSlash == 0)
        return "/";
    return path.substring(0, lastSlash);
}

String getFileBrowserFilterLabel()
{
    if (file_filter_index == 0)
        return "ALL";
    if (file_filter_index == 27)
        return "#";
    char c = (char)('A' + (file_filter_index - 1));
    return String(c);
}

void resetFileBrowserFilter()
{
    file_filter_index = 0;
}

void cycleFileBrowserFilter()
{
    file_filter_index++;
    if (file_filter_index > 27)
        file_filter_index = 0;
}

bool matchesFileBrowserFilter(const String &name)
{
    if (file_filter_index == 0)
        return true;
    if (name.length() == 0)
        return false;

    char c = name.charAt(0);
    if (c >= 'a' && c <= 'z')
        c = (char)(c - ('a' - 'A'));

    if (file_filter_index == 27)
        return (c >= '0' && c <= '9');

    char wanted = (char)('A' + (file_filter_index - 1));
    return c == wanted;
}
