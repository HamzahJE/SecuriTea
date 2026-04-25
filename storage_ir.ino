// ==========================================
// SD CARD + IR ENGINES
// ==========================================
void rebuildDirectoryCache(const String &path)
{
    dir_cache_count = 0;
    dir_cache_path = path;
    dir_cache_valid = false;

    lockSD();
    File dir = SD.open(path);

    if (!dir || !dir.isDirectory())
    {
        if (dir)
            dir.close();
        unlockSD();
        return;
    }

    File file = dir.openNextFile();
    while (file && dir_cache_count < MAX_DIR_CACHE)
    {
        String fname = getBaseName(String(file.name()));
        String lowerFname = fname;
        lowerFname.toLowerCase();
        bool isFolder = file.isDirectory();
        bool isIrFile = lowerFname.endsWith(".ir");

        // Keep only folders and .ir files in the browse cache.
        if (!fname.startsWith("._") && !fname.startsWith("System") && fname != "univ_tv.ir" && (isFolder || isIrFile))
        {
            dir_cache_items[dir_cache_count] = fname;
            dir_cache_is_folder[dir_cache_count] = isFolder;
            dir_cache_count++;
        }

        file.close();
        yield();
        file = dir.openNextFile();
    }

    if (file)
        file.close();
    dir.close();
    unlockSD();

    dir_cache_valid = true;
}

void loadDirectory(String path)
{
    dir_item_count = 0;

    if (!dir_cache_valid || dir_cache_path != path)
        rebuildDirectoryCache(path);

    if (!dir_cache_valid)
        return;

    if (path != "/")
    {
        dir_items[dir_item_count] = ".. (Back)";
        dir_is_folder[dir_item_count] = true;
        dir_item_count++;
    }

    for (int i = 0; i < dir_cache_count && dir_item_count < MAX_DIR_ITEMS; i++)
    {
        if (matchesFileBrowserFilter(dir_cache_items[i]))
        {
            dir_items[dir_item_count] = dir_cache_items[i];
            dir_is_folder[dir_item_count] = dir_cache_is_folder[i];
            dir_item_count++;
        }
    }

    menu_index = 0;
}

bool loadFlipperCommandByIndex(String filepath, int target_index)
{
    lockSD();
    File file = SD.open(filepath);
    if (!file)
    {
        unlockSD();
        setStatus("File Missing");
        return false;
    }

    int command_count = 0;
    bool target_found = false;
    bool is_raw = false;
    bool is_parsed = false;
    uint16_t tempRaw[MAX_RAW_BUFFER];
    uint16_t tempRawLen = 0;
    uint16_t tempFrequency = 38;
    String tempBtnName = "None";
    String tempProtocol = "";
    uint32_t tempAddress = 0;
    uint32_t tempCommand = 0;

    while (file.available())
    {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.length() == 0)
            continue;

        if (line.startsWith("name: "))
        {
            command_count++;
            if (command_count == target_index)
            {
                target_found = true;
                is_raw = false;
                is_parsed = false;
                tempRawLen = 0;
                tempFrequency = 38;
                tempBtnName = line.substring(6);
                tempProtocol = "";
                tempAddress = 0;
                tempCommand = 0;
            }
            else
            {
                target_found = false;
            }
        }
        else if (target_found && line.startsWith("type: raw"))
        {
            is_raw = true;
            is_parsed = false;
        }
        else if (target_found && line.startsWith("type: parsed"))
        {
            is_parsed = true;
            is_raw = false;
        }
        else if (target_found && is_parsed && line.startsWith("protocol: "))
        {
            tempProtocol = line.substring(10);
            tempProtocol.trim();
        }
        else if (target_found && is_parsed && line.startsWith("address: "))
        {
            tempAddress = parseFlipperHex32(line.substring(9));
        }
        else if (target_found && is_parsed && line.startsWith("command: "))
        {
            tempCommand = parseFlipperHex32(line.substring(9));
            file.close();
            unlockSD();
            setParsedPayload(tempProtocol, tempAddress, tempCommand, tempBtnName);
            setStatus("Loaded: " + tempBtnName + " (parsed)");
            return true;
        }
        else if (target_found && is_raw && line.startsWith("frequency: "))
        {
            long parsed = line.substring(11).toInt();
            if (parsed >= 1000)
                tempFrequency = (uint16_t)(parsed / 1000);
            else if (parsed > 0)
                tempFrequency = (uint16_t)parsed;
        }
        else if (target_found && is_raw && line.startsWith("data: "))
        {
            String dataStr = line.substring(6);
            int startIdx = 0;
            int spaceIdx = dataStr.indexOf(' ');

            while (spaceIdx != -1)
            {
                if (tempRawLen < MAX_RAW_BUFFER)
                {
                    long value = dataStr.substring(startIdx, spaceIdx).toInt();
                    if (value > 0)
                        tempRaw[tempRawLen++] = (uint16_t)value;
                }
                startIdx = spaceIdx + 1;
                spaceIdx = dataStr.indexOf(' ', startIdx);
                yield();
            }
            if (startIdx < dataStr.length() && tempRawLen < MAX_RAW_BUFFER)
            {
                long value = dataStr.substring(startIdx).toInt();
                if (value > 0)
                    tempRaw[tempRawLen++] = (uint16_t)value;
            }

            if (tempRawLen == 0)
            {
                file.close();
                unlockSD();
                setStatus("Empty raw data");
                return false;
            }

            file.close();
            unlockSD();
            setPayload(tempRaw, tempRawLen, tempFrequency, tempBtnName);
            setStatus("Loaded: " + tempBtnName);
            return true;
        }
    }
    file.close();
    unlockSD();
    if (target_index == 1)
        setStatus("No IR commands in file");
    return false;
}

int countCommandsInFile(const String &path)
{
    lockSD();
    File file = SD.open(path);
    if (!file)
    {
        unlockSD();
        return 0;
    }

    int count = 0;
    while (file.available())
    {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.startsWith("name: "))
            count++;
        yield();
    }

    file.close();
    unlockSD();
    return count;
}

bool deleteFlipperCommandByIndex(const String &path, int target_index, String &deletedName, bool &fileRemoved)
{
    deletedName = "";
    fileRemoved = false;

    if (target_index <= 0)
        return false;

    String tmpPath = path + ".tmp";
    String backupPath = path + ".bak";

    lockSD();
    File source = SD.open(path);
    if (!source)
    {
        unlockSD();
        return false;
    }

    SD.remove(tmpPath.c_str());
    File temp = SD.open(tmpPath, FILE_WRITE);
    if (!temp)
    {
        source.close();
        unlockSD();
        return false;
    }

    int commandIndex = 0;
    int keptCommands = 0;
    bool skippingTarget = false;
    bool found = false;

    while (source.available())
    {
        String line = source.readStringUntil('\n');
        String trimmed = line;
        trimmed.trim();

        if (trimmed.startsWith("name: "))
        {
            commandIndex++;
            bool isTarget = (commandIndex == target_index);
            skippingTarget = isTarget;

            if (isTarget)
            {
                deletedName = trimmed.substring(6);
                deletedName.trim();
                found = true;
                continue;
            }

            keptCommands++;
        }

        if (!skippingTarget)
            temp.println(line);

        yield();
    }

    source.close();
    temp.close();

    if (!found)
    {
        SD.remove(tmpPath.c_str());
        unlockSD();
        return false;
    }

    if (keptCommands <= 0)
    {
        SD.remove(path.c_str());
        SD.remove(tmpPath.c_str());
        fileRemoved = true;
        unlockSD();
        return true;
    }

    SD.remove(backupPath.c_str());
    bool movedOriginal = SD.rename(path.c_str(), backupPath.c_str());
    if (!movedOriginal)
    {
        SD.remove(tmpPath.c_str());
        unlockSD();
        return false;
    }

    bool movedTemp = SD.rename(tmpPath.c_str(), path.c_str());
    if (!movedTemp)
    {
        SD.rename(backupPath.c_str(), path.c_str());
        unlockSD();
        return false;
    }

    SD.remove(backupPath.c_str());
    unlockSD();
    return true;
}

bool loadUniversalCommandList(const String &path)
{
    univ_cmd_count = 0;

    lockSD();
    File file = SD.open(path);
    if (!file)
    {
        unlockSD();
        setStatus("Missing " + path);
        return false;
    }

    while (file.available() && univ_cmd_count < MAX_UNIV_ITEMS)
    {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.startsWith("name: "))
        {
            String groupName = line.substring(6);
            groupName.trim();

            bool alreadyExists = false;
            for (int i = 0; i < univ_cmd_count; i++)
            {
                if (univ_cmd_items[i].equalsIgnoreCase(groupName))
                {
                    alreadyExists = true;
                    break;
                }
            }

            if (!alreadyExists)
            {
                univ_cmd_items[univ_cmd_count] = groupName;
                univ_cmd_count++;
            }
        }
        yield();
    }

    file.close();
    unlockSD();

    if (univ_cmd_count == 0)
    {
        setStatus("No commands in profile");
        return false;
    }

    return true;
}

int getUniversalGroupCommandIndices(const String &path, const String &groupName, int *indices, int maxIndices)
{
    lockSD();
    File file = SD.open(path);
    if (!file)
    {
        unlockSD();
        return 0;
    }

    int commandIndex = 0;
    int matched = 0;

    while (file.available())
    {
        String line = file.readStringUntil('\n');
        line.trim();

        if (line.startsWith("name: "))
        {
            commandIndex++;
            String thisName = line.substring(6);
            thisName.trim();
            if (thisName.equalsIgnoreCase(groupName) && matched < maxIndices)
            {
                indices[matched++] = commandIndex;
            }
        }
        yield();
    }

    file.close();
    unlockSD();
    return matched;
}

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

    while (file.available())
    {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.startsWith("name: Learned_"))
        {
            int value = line.substring(14).toInt();
            if (value > maxIndex)
                maxIndex = value;
        }
        yield();
    }

    file.close();
    unlockSD();
    return maxIndex + 1;
}

bool saveLearnedCommandToSD()
{
    uint16_t rawLocal[MAX_RAW_BUFFER];
    uint16_t lenLocal = 0;
    uint16_t freqLocal = 38;
    String btnNameLocal = "Learned";

    if (!copyPayload(rawLocal, lenLocal, freqLocal, btnNameLocal) || lenLocal == 0)
    {
        setStatus("Nothing to save");
        return false;
    }

    lockSD();
    SD.mkdir("/captures");
    unlockSD();
    String path = "/captures/learned.ir";
    int nextIdx = getNextLearnedIndex(path);

    char nameBuf[20];
    snprintf(nameBuf, sizeof(nameBuf), "Learned_%03d", nextIdx);

    lockSD();
    File file = SD.open(path, FILE_APPEND);
    if (!file)
    {
        unlockSD();
        setStatus("Save failed");
        return false;
    }

    file.print("name: ");
    file.println(nameBuf);
    file.println("type: raw");
    file.print("frequency: ");
    file.println((int)freqLocal * 1000);
    file.print("data: ");
    for (uint16_t i = 0; i < lenLocal; i++)
    {
        file.print(rawLocal[i]);
        if (i + 1 < lenLocal)
            file.print(' ');
    }
    file.println();
    file.println();
    file.close();
    unlockSD();

    setStatus("Saved " + String(nameBuf));
    setLearnPhase(LEARN_SAVED);
    return true;
}

bool transmitCurrentPayload()
{
    uint16_t rawLocal[MAX_RAW_BUFFER];
    uint16_t lenLocal = 0;
    uint16_t freqLocal = 38;
    String btnNameLocal = "None";
    bool isParsed = false;
    String protocolLocal = "";
    uint32_t addressLocal = 0;
    uint32_t commandLocal = 0;

    lockState();
    if (!shared.hasPayload)
    {
        unlockState();
        setStatus("No payload to send");
        return false;
    }

    isParsed = shared.payloadIsParsed;
    btnNameLocal = shared.btnName;
    if (isParsed)
    {
        protocolLocal = shared.parsedProtocol;
        addressLocal = shared.parsedAddress;
        commandLocal = shared.parsedCommand;
    }
    else
    {
        lenLocal = shared.rawLen;
        freqLocal = shared.freqKHz;
        for (uint16_t i = 0; i < lenLocal; i++)
            rawLocal[i] = shared.raw[i];
    }
    unlockState();

    markTransmitting(true);
    setStatus("Sending " + btnNameLocal);

    if (isParsed)
    {
        String p = protocolLocal;
        p.toUpperCase();

        uint8_t addr8 = (uint8_t)(addressLocal & 0xFF);
        uint8_t cmd8 = (uint8_t)(commandLocal & 0xFF);
        uint16_t addr16 = (uint16_t)(addressLocal & 0xFFFF);
        uint16_t cmd16 = (uint16_t)(commandLocal & 0xFFFF);

        if (p == "SAMSUNG32" || p == "SAMSUNG")
        {
            uint32_t data = ((uint32_t)addr8) | ((uint32_t)(addr8 ^ 0xFF) << 8) |
                            ((uint32_t)cmd8 << 16) | ((uint32_t)(cmd8 ^ 0xFF) << 24);
            irsend.sendSAMSUNG(data, 32, currentProtocolFrameRepeat);
        }
        else if (p == "NECEXT")
        {
            uint32_t data = ((uint32_t)cmd16 << 16) | (uint32_t)addr16;
            irsend.sendNEC(data, 32, currentProtocolFrameRepeat);

            if (universal_mode == UNIV_MODE_AGGRESSIVE)
            {
                vTaskDelay(pdMS_TO_TICKS(25));
                uint32_t std = ((uint32_t)addr8) | ((uint32_t)(addr8 ^ 0xFF) << 8) |
                               ((uint32_t)cmd8 << 16) | ((uint32_t)(cmd8 ^ 0xFF) << 24);
                irsend.sendNEC(std, 32, currentProtocolFrameRepeat);
            }
        }
        else if (p == "NEC")
        {
            uint32_t std = ((uint32_t)addr8) | ((uint32_t)(addr8 ^ 0xFF) << 8) |
                           ((uint32_t)cmd8 << 16) | ((uint32_t)(cmd8 ^ 0xFF) << 24);
            irsend.sendNEC(std, 32, currentProtocolFrameRepeat);

            if (universal_mode == UNIV_MODE_AGGRESSIVE)
            {
                vTaskDelay(pdMS_TO_TICKS(25));
                uint32_t ext = ((uint32_t)cmd16 << 16) | (uint32_t)addr16;
                irsend.sendNEC(ext, 32, currentProtocolFrameRepeat);
            }
        }
        else if (p == "RC5")
        {
            uint16_t data = (uint16_t)(((addr8 & 0x1F) << 6) | (cmd8 & 0x3F));
            irsend.sendRC5(data, 12, currentProtocolFrameRepeat);
        }
        else if (p == "RC6")
        {
            uint32_t data = ((uint32_t)(addr8 & 0xFF) << 8) | (uint32_t)(cmd8 & 0xFF);
            irsend.sendRC6(data, 20, currentProtocolFrameRepeat);
        }
        else if (p == "SIRC" || p == "SONY")
        {
            uint16_t data = (uint16_t)((cmd8 & 0x7F) | ((addr8 & 0x1F) << 7));
            irsend.sendSony(data, 12, currentProtocolFrameRepeat);
        }
        else
        {
            markTransmitting(false);
            setStatus("Unsupported protocol");
            return false;
        }
    }
    else
    {
        if (lenLocal == 0)
        {
            markTransmitting(false);
            setStatus("No raw payload");
            return false;
        }
        irsend.sendRaw(rawLocal, lenLocal, freqLocal);
    }

    vTaskDelay(pdMS_TO_TICKS(30));
    markTransmitting(false);
    setStatus("Sent " + btnNameLocal);
    return true;
}

bool captureLearnSignal()
{
    if (!irrecv.decode(&results))
        return false;

    uint16_t localRaw[MAX_RAW_BUFFER];
    uint16_t localLen = 0;
    uint16_t localFreq = 38;

    uint16_t srcLen = results.rawlen;
    if (srcLen > MAX_RAW_BUFFER)
        srcLen = MAX_RAW_BUFFER;

    for (uint16_t i = 1; i < srcLen; i++)
    {
        localRaw[localLen++] = results.rawbuf[i] * kRawTick;
    }

    String protocol = typeToString(results.decode_type, false);
    if (protocol.length() == 0)
        protocol = "UNKNOWN";
    protocol.toUpperCase();

    String detail = "Bits:" + String(results.bits);
    if (results.address != 0 || results.command != 0)
    {
        String addrHex = String((uint32_t)results.address, HEX);
        String cmdHex = String((uint32_t)results.command, HEX);
        addrHex.toUpperCase();
        cmdHex.toUpperCase();
        detail = "A:0x" + addrHex + " C:0x" + cmdHex;
    }
    else if (results.value != 0)
    {
        String valueHex = uint64ToString(results.value, 16);
        valueHex.toUpperCase();
        detail = "V:0x" + valueHex + " B:" + String(results.bits);
    }

    Serial.println("=== LEARN CAPTURE ===");
    Serial.println(resultToHumanReadableBasic(&results));
    Serial.println(resultToSourceCode(&results));

    irrecv.resume();

    if (localLen == 0)
    {
        setLearnDetails("", "");
        setStatus("Capture failed");
        setLearnPhase(LEARN_ERROR);
        return false;
    }

    setPayload(localRaw, localLen, localFreq, "Captured");
    setLearnDetails(protocol, detail);
    setStatus("Captured " + protocol);
    setLearnPhase(LEARN_CAPTURED);
    return true;
}
