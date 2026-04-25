// ==========================================
// FREERTOS TASKS
// ==========================================
void uiTask(void *pvParameters)
{
    pinMode(JOY_BTN, INPUT_PULLUP);
    bool lastBtnState = HIGH;
    AppState previousState = current_state;

    while (1)
    {
        int xVal = analogRead(JOY_X);
        int yVal = analogRead(JOY_Y);
        bool currentBtnState = digitalRead(JOY_BTN);
        long current_time = millis();

        if (current_time - last_joy_time > 250)
        {
            if (yVal < 1000)
            {
                if (current_state == MENU_UNIV_REMOTE)
                    headerModeSelected = false;
                if (current_state == APP_REMOTE_VIEW)
                {
                    remoteModeSelected = false;
                    if (remote_cmd_index > 1)
                    {
                        remote_cmd_index--;
                        loadFlipperCommandByIndex(selected_file_path, remote_cmd_index);
                    }
                }
                else if (current_state == APP_FILE_BROWSER && menu_index > 0)
                    menu_index--;
                else if (current_state != APP_FILE_BROWSER)
                    menu_index--;
                last_joy_time = current_time;
            }
            if (yVal > 3000)
            {
                if (current_state == MENU_UNIV_REMOTE)
                    headerModeSelected = false;
                if (current_state == APP_REMOTE_VIEW)
                {
                    remoteModeSelected = false;
                    if (remote_cmd_index < remote_cmd_count)
                    {
                        remote_cmd_index++;
                        loadFlipperCommandByIndex(selected_file_path, remote_cmd_index);
                    }
                }
                else if (current_state == APP_FILE_BROWSER && menu_index < dir_item_count - 1)
                    menu_index++;
                else if (current_state != APP_FILE_BROWSER)
                    menu_index++;
                last_joy_time = current_time;
            }
            if (xVal > 3000)
            {
                if (current_state == MENU_UNIV_REMOTE)
                {
                    headerModeSelected = true;
                    last_joy_time = current_time;
                }
                else if (current_state == APP_FILE_BROWSER)
                {
                    cycleFileBrowserFilter();
                    loadDirectory(current_path);
                    setStatus("Filter: " + getFileBrowserFilterLabel());
                    last_joy_time = current_time;
                }
                else if (current_state == APP_REMOTE_VIEW)
                {
                    remoteModeSelected = true;
                    last_joy_time = current_time;
                }
            }
            if (xVal < 1000)
            {
                if (current_state == MENU_UNIV_REMOTE && headerModeSelected)
                {
                    headerModeSelected = false;
                }
                else if (current_state == APP_REMOTE_VIEW && remoteModeSelected)
                {
                    remoteModeSelected = false;
                }
                else if (current_state == APP_FILE_BROWSER && file_filter_index != 0)
                {
                    resetFileBrowserFilter();
                    loadDirectory(current_path);
                    setStatus("Filter: ALL");
                }
                else if (current_state == MENU_UNIV_REMOTE || current_state == APP_LEARN || current_state == APP_FILE_BROWSER)
                {
                    if (current_state == APP_LEARN)
                        queueCommand(IR_CMD_LEARN_STOP);
                    current_state = MENU_MAIN;
                    menu_index = 0;
                }
                else if (current_state == APP_UNIV_BRUTE)
                {
                    requestUniversalCancel(true);
                    setUniversalProgress(false, 0, 0, "");
                    current_state = MENU_UNIV_REMOTE;
                    menu_index = 0;
                }
                else if (current_state == APP_REMOTE_VIEW)
                {
                    current_state = APP_FILE_BROWSER;
                }
                last_joy_time = current_time;
            }
        }

        if (currentBtnState == LOW && lastBtnState == HIGH)
        {
            if (current_state == MENU_MAIN)
            {
                if (menu_index == 0)
                    current_state = MENU_UNIV_REMOTE;
                if (menu_index == 1)
                {
                    current_state = APP_FILE_BROWSER;
                    current_path = "/";
                    resetFileBrowserFilter();
                    dir_cache_valid = false;
                    loadDirectory(current_path);
                }
                if (menu_index == 2)
                    current_state = APP_LEARN;
                menu_index = 0;
            }
            else if (current_state == MENU_UNIV_REMOTE)
            {
                if (headerModeSelected)
                {
                    if (universal_mode == UNIV_MODE_SINGLE)
                        universal_mode = UNIV_MODE_AGGRESSIVE;
                    else
                        universal_mode = UNIV_MODE_SINGLE;

                    applyUniversalModeSettings();
                    setStatus("Mode: " + getUniversalModeLabel());
                    headerModeSelected = false;
                    lastBtnState = currentBtnState;
                    vTaskDelay(pdMS_TO_TICKS(33));
                    continue;
                }

                if (menu_index == 0)
                    current_target_device = "TV";
                if (menu_index == 1)
                    current_target_device = "AC";
                if (menu_index == 2)
                    current_target_device = "Proj";
                if (menu_index == 3)
                    current_target_device = "Audio";

                univ_profile_path = bruteProfileForTarget(current_target_device);
                loadUniversalCommandList(univ_profile_path);
                setBruteStatus(false, 1, univ_profile_path);
                requestUniversalCancel(false);
                setUniversalProgress(false, 0, 0, "");
                setStatus("Select command");

                current_state = APP_UNIV_BRUTE;
                menu_index = 0;
            }
            else if (current_state == APP_UNIV_BRUTE)
            {
                if (univ_cmd_count > 0)
                {
                    requestUniversalCancel(false);
                    queueCommand(IR_CMD_UNIV_SEND, menu_index);
                }
                else
                {
                    setStatus("No commands loaded");
                }
            }
            else if (current_state == APP_FILE_BROWSER)
            {
                if (dir_item_count == 0)
                {
                    setStatus("Folder empty");
                    lastBtnState = currentBtnState;
                    vTaskDelay(pdMS_TO_TICKS(33));
                    continue;
                }

                if (dir_is_folder[menu_index])
                {
                    if (dir_items[menu_index] == ".. (Back)")
                    {
                        current_path = getParentDir(current_path);
                    }
                    else
                    {
                        if (current_path == "/")
                            current_path = "/" + dir_items[menu_index];
                        else
                            current_path = current_path + "/" + dir_items[menu_index];

                        // Apply first-letter filtering only at the current level.
                        resetFileBrowserFilter();
                    }
                    loadDirectory(current_path);
                }
                else
                {
                    String full_path = "";
                    if (current_path == "/")
                        full_path = "/" + dir_items[menu_index];
                    else
                        full_path = current_path + "/" + dir_items[menu_index];

                    selected_file_path = full_path;
                    remote_cmd_count = countCommandsInFile(full_path);
                    if (remote_cmd_count <= 0)
                    {
                        setStatus("No commands in file");
                        lastBtnState = currentBtnState;
                        vTaskDelay(pdMS_TO_TICKS(33));
                        continue;
                    }
                    remote_cmd_index = 1;
                    setSelectedFileName(dir_items[menu_index]);
                    loadFlipperCommandByIndex(full_path, remote_cmd_index);
                    current_state = APP_REMOTE_VIEW;
                }
            }
            else if (current_state == APP_REMOTE_VIEW)
            {
                if (remoteModeSelected)
                {
                    if (remote_send_mode == REMOTE_SEND_ONE)
                        remote_send_mode = REMOTE_SEND_LOOP;
                    else if (remote_send_mode == REMOTE_SEND_LOOP)
                        remote_send_mode = REMOTE_SEND_DELETE;
                    else
                        remote_send_mode = REMOTE_SEND_ONE;

                    setStatus("Remote Mode: " + getRemoteSendModeLabel());
                    remoteModeSelected = false;
                }
                else
                {
                    if (remote_send_mode == REMOTE_SEND_LOOP)
                        queueCommand(IR_CMD_REMOTE_SEND_ALL);
                    else if (remote_send_mode == REMOTE_SEND_ONE)
                        queueCommand(IR_CMD_TRANSMIT_CURRENT);
                    else
                        queueCommand(IR_CMD_REMOTE_DELETE);
                }
            }
            else if (current_state == APP_LEARN)
            {
                LearnPhase phase = getLearnPhaseSnapshot();
                if (phase == LEARN_CAPTURED)
                {
                    setLearnPhase(LEARN_SAVING);
                    queueCommand(IR_CMD_LEARN_SAVE);
                }
                else if (phase == LEARN_ERROR || phase == LEARN_IDLE || phase == LEARN_SAVED)
                {
                    queueCommand(IR_CMD_LEARN_START);
                }
            }
        }
        lastBtnState = currentBtnState;

        if (previousState != current_state)
        {
            if (current_state == APP_LEARN)
            {
                queueCommand(IR_CMD_LEARN_START);
                setStatus("Point remote and press key");
            }
            if (current_state == MENU_UNIV_REMOTE)
            {
                headerModeSelected = false;
            }
            if (current_state == APP_REMOTE_VIEW)
            {
                remoteModeSelected = false;
            }
            if (previousState == APP_LEARN && current_state != APP_LEARN)
            {
                queueCommand(IR_CMD_LEARN_STOP);
            }
            previousState = current_state;
        }

        if (current_state == MENU_MAIN)
        {
            if (menu_index < 0)
                menu_index = main_menu_len - 1;
            if (menu_index >= main_menu_len)
                menu_index = 0;
            drawMenu("SecuriTea OS", main_menu, main_menu_len, menu_index);
        }
        else if (current_state == MENU_UNIV_REMOTE)
        {
            if (menu_index < 0)
                menu_index = univ_menu_len - 1;
            if (menu_index >= univ_menu_len)
                menu_index = 0;
            String local_univ_menu[MAX_UNIV_ITEMS];
            for (int i = 0; i < univ_menu_len; i++)
                local_univ_menu[i] = univ_menu[i];
            drawMenu("Universal Remote", local_univ_menu, univ_menu_len, menu_index, "[v^]Scroll [>]Mode [BTN]Toggle/Pick [<]Back", getUniversalModeBadge(), true, headerModeSelected);
        }
        else if (current_state == APP_FILE_BROWSER)
        {
            for (int i = 0; i < dir_item_count; i++)
            {
                if (dir_is_folder[i] && dir_items[i] != ".. (Back)")
                    display_items_buffer[i] = "[DIR] " + dir_items[i];
                else
                    display_items_buffer[i] = dir_items[i];
            }
            if (dir_item_count == 0)
            {
                display.clear();
                drawHeader("Path: " + current_path, getFileBrowserFilterLabel());
                display.drawString(0, UI_LINE_1_Y + 4, "(No items)");
                drawFooter("[>]Filter [<]Clr/Back");
                display.display();
            }
            else
            {
                drawMenu("Path: " + current_path, display_items_buffer, dir_item_count, menu_index, "[>]Filter [BTN]Open [<]Clr/Back", getFileBrowserFilterLabel());
            }
        }
        else if (current_state == APP_UNIV_BRUTE)
        {
            if (univ_cmd_count > 0)
            {
                if (menu_index < 0)
                    menu_index = univ_cmd_count - 1;
                if (menu_index >= univ_cmd_count)
                    menu_index = 0;
            }
            else
            {
                menu_index = 0;
            }
            UiSnapshot snap = snapshotUi();

            if (univ_cmd_count <= 0)
            {
                display.clear();
                drawHeader(current_target_device + " Remote", getUniversalModeBadge());
                display.drawString(0, UI_LINE_1_Y + 4, "No commands found");
                display.drawString(0, UI_LINE_2_Y + 4, "File: " + univ_profile_path);
                drawFooter("[<] Back");
                display.display();
            }
            else
            {
                drawMenu(current_target_device + " Remote", univ_cmd_items, univ_cmd_count, menu_index, "[<] Back | [BTN] Select", getUniversalModeBadge(), false);
                if (snap.univSending)
                    drawUniversalProgressPopup(snap.univProgressName, snap.univProgressCurrent, snap.univProgressTotal);
                display.display();
            }
        }
        else if (current_state == APP_REMOTE_VIEW)
        {
            UiSnapshot snap = snapshotUi();
            display.clear();
            drawHeader("<- Back   Blaster", getRemoteSendModeBadge(), remoteModeSelected);
            display.drawString(0, UI_LINE_1_Y, "File: " + snap.selectedFile);
            display.drawString(0, UI_LINE_2_Y, "Btn: " + snap.btnName);
            display.drawString(0, UI_LINE_3_Y, "Cmd: " + String(remote_cmd_index) + "/" + String(remote_cmd_count));

            if (snap.transmitting)
                drawFooter("[ TRANSMITTING... ]");
            else if (remote_send_mode == REMOTE_SEND_DELETE)
                drawFooter("[v^]Cmd [>]Mode [BTN]Delete");
            else
                drawFooter("[v^]Cmd [>]Mode [BTN]Send");
            display.display();
        }
        else if (current_state == APP_LEARN)
        {
            UiSnapshot snap = snapshotUi();
            display.clear();
            drawHeader("Learn New Remote");
            display.drawString(0, UI_LINE_1_Y, snap.status);

            if (snap.learnPhase == LEARN_LISTENING)
            {
                display.drawString(0, UI_LINE_2_Y, "Waiting for signal...");
                drawFooter("[<] Back");
            }
            else if (snap.learnPhase == LEARN_CAPTURED)
            {
                String proto = snap.learnProtocol.length() ? snap.learnProtocol : "UNKNOWN";
                display.drawString(0, UI_LINE_2_Y, trimToWidth("Proto: " + proto, 21));
                if (snap.learnDetail.length())
                    display.drawString(0, UI_LINE_3_Y, trimToWidth(snap.learnDetail, 21));
                drawFooter("[BTN] Save | [<] Back");
            }
            else if (snap.learnPhase == LEARN_SAVED)
            {
                display.drawString(0, UI_LINE_2_Y, "Saved to /captures/");
                if (snap.learnProtocol.length())
                    display.drawString(0, UI_LINE_3_Y, trimToWidth("Proto: " + snap.learnProtocol, 21));
                drawFooter("[BTN] Capture Again");
            }
            else if (snap.learnPhase == LEARN_ERROR)
            {
                display.drawString(0, UI_LINE_2_Y, "Capture error");
                drawFooter("[BTN] Retry | [<] Back");
            }
            else
            {
                display.drawString(0, UI_LINE_2_Y, "Preparing listener...");
                drawFooter("[<] Back");
            }
            display.display();
        }

        vTaskDelay(pdMS_TO_TICKS(33));
    }
}

void irTask(void *pvParameters)
{
    bool learnListening = false;

    while (1)
    {
        IrCommand cmd;
        while (xQueueReceive(irCommandQueue, &cmd, 0) == pdTRUE)
        {
            if (cmd.type == IR_CMD_TRANSMIT_CURRENT)
            {
                transmitCurrentPayload();
            }
            else if (cmd.type == IR_CMD_REMOTE_SEND_ALL)
            {
                String path = selected_file_path;
                if (path.length() == 0 || remote_cmd_count <= 0)
                {
                    setStatus("No file commands");
                }
                else
                {
                    int sentCount = 0;
                    for (int i = 1; i <= remote_cmd_count; i++)
                    {
                        bool loaded = loadFlipperCommandByIndex(path, i);
                        if (!loaded)
                            continue;

                        bool sentOk = false;
                        for (uint8_t rep = 0; rep < currentTransmitRepeats; rep++)
                        {
                            if (transmitCurrentPayload())
                                sentOk = true;

                            if (rep + 1 < currentTransmitRepeats)
                                vTaskDelay(pdMS_TO_TICKS(currentRepeatGapMs));
                        }

                        if (sentOk)
                            sentCount++;

                        vTaskDelay(pdMS_TO_TICKS(currentSendDelayMs));
                    }
                    setStatus("Loop Done " + String(sentCount) + "/" + String(remote_cmd_count));
                }
            }
            else if (cmd.type == IR_CMD_REMOTE_DELETE)
            {
                String path = selected_file_path;
                int deleteIndex = remote_cmd_index;

                if (path.length() == 0 || deleteIndex <= 0)
                {
                    setStatus("No command selected");
                }
                else
                {
                    String deletedName = "";
                    bool fileRemoved = false;
                    bool deleted = deleteFlipperCommandByIndex(path, deleteIndex, deletedName, fileRemoved);

                    if (!deleted)
                    {
                        setStatus("Delete failed");
                    }
                    else
                    {
                        dir_cache_valid = false;
                        if (fileRemoved)
                        {
                            remote_cmd_count = 0;
                            current_state = APP_FILE_BROWSER;
                            loadDirectory(current_path);
                            setStatus("Deleted " + deletedName + " (file empty)");
                        }
                        else
                        {
                            remote_cmd_count = countCommandsInFile(path);
                            if (remote_cmd_count <= 0)
                            {
                                current_state = APP_FILE_BROWSER;
                                loadDirectory(current_path);
                                setStatus("Deleted " + deletedName);
                            }
                            else
                            {
                                if (remote_cmd_index > remote_cmd_count)
                                    remote_cmd_index = remote_cmd_count;

                                if (loadFlipperCommandByIndex(path, remote_cmd_index))
                                    setStatus("Deleted " + deletedName);
                                else
                                    setStatus("Deleted; reload failed");
                            }
                        }
                    }
                }
            }
            else if (cmd.type == IR_CMD_UNIV_SEND)
            {
                String profilePath = getUniversalProfilePathSnapshot();
                int groupIdx = cmd.arg;

                if (groupIdx < 0 || groupIdx >= univ_cmd_count)
                {
                    setStatus("Invalid group");
                    continue;
                }

                String groupName = univ_cmd_items[groupIdx];
                lockSD();
                bool exists = SD.exists(profilePath.c_str());
                unlockSD();

                if (!exists)
                {
                    setStatus("Missing " + profilePath);
                }
                else
                {
                    int matchIndices[MAX_UNIV_ITEMS];
                    int matchCount = getUniversalGroupCommandIndices(profilePath, groupName, matchIndices, MAX_UNIV_ITEMS);

                    if (matchCount <= 0)
                    {
                        setStatus("No cmds for " + groupName);
                        setUniversalProgress(false, 0, 0, "");
                    }
                    else
                    {
                        int sentCount = 0;
                        int failCount = 0;
                        setUniversalProgress(true, 0, matchCount, groupName);
                        for (int i = 0; i < matchCount; i++)
                        {
                            if (isUniversalCancelRequested())
                            {
                                setStatus("Canceled " + groupName);
                                break;
                            }

                            int commandIndex = matchIndices[i];
                            bool loaded = false;
                            for (uint8_t attempt = 0; attempt < currentLoadRetries; attempt++)
                            {
                                if (loadFlipperCommandByIndex(profilePath, commandIndex))
                                {
                                    loaded = true;
                                    break;
                                }
                                vTaskDelay(pdMS_TO_TICKS(15));
                            }

                            if (!loaded)
                            {
                                failCount++;
                                continue;
                            }

                            setUniversalProgress(true, i + 1, matchCount, groupName);
                            setBruteStatus(false, commandIndex, profilePath);

                            bool sentAtLeastOnce = false;
                            for (uint8_t rep = 0; rep < currentTransmitRepeats; rep++)
                            {
                                if (isUniversalCancelRequested())
                                {
                                    setStatus("Canceled " + groupName);
                                    break;
                                }

                                bool sentOk = transmitCurrentPayload();
                                if (sentOk)
                                    sentAtLeastOnce = true;

                                if (rep + 1 < currentTransmitRepeats)
                                    vTaskDelay(pdMS_TO_TICKS(currentRepeatGapMs));
                            }

                            if (sentAtLeastOnce)
                                sentCount++;
                            else
                                failCount++;

                            vTaskDelay(pdMS_TO_TICKS(currentSendDelayMs));
                        }
                        setUniversalProgress(false, 0, 0, "");
                        if (!isUniversalCancelRequested())
                            setStatus("Done " + groupName + " " + String(sentCount) + "/" + String(matchCount));
                        requestUniversalCancel(false);
                    }
                }
            }
            else if (cmd.type == IR_CMD_LEARN_START)
            {
                irrecv.enableIRIn();
                learnListening = true;
                setLearnPhase(LEARN_LISTENING);
                setLearnDetails("", "");
                setStatus("Listening...");
            }
            else if (cmd.type == IR_CMD_LEARN_STOP)
            {
                learnListening = false;
                irrecv.disableIRIn();
                setLearnPhase(LEARN_IDLE);
                setLearnDetails("", "");
            }
            else if (cmd.type == IR_CMD_LEARN_SAVE)
            {
                saveLearnedCommandToSD();
            }
        }

        if (learnListening)
        {
            captureLearnSignal();
        }

        vTaskDelay(pdMS_TO_TICKS(15));
    }
}
