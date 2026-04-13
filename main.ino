#include <Arduino.h>
#include <Wire.h>
#include "SSD1306Wire.h"
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRutils.h>
#include <IRsend.h>
#include <SPI.h>
#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

// ==========================================
// PIN DEFINITIONS
// ==========================================
#define JOY_X 3
#define JOY_Y 4
#define JOY_BTN 5
const uint16_t kIrRecvPin = 6;
const uint16_t kIrLedPin = 7;

// SD Card SPI Pins (Conflict-Free Right Side)
#define SD_MISO 34 // 11
#define SD_SCK 33  // 12
#define SD_MOSI 47 // 13
#define SD_CS 48   // 14

// ==========================================
// GLOBALS & OBJECTS
// ==========================================
SSD1306Wire display(0x3c, SDA_OLED, SCL_OLED);
IRrecv irrecv(kIrRecvPin, 1024, 50, true);
decode_results results;
IRsend irsend(kIrLedPin);

// Create a solid global SPI highway (Prevents pointer leaks)
SPIClass sdSPI(FSPI);

enum AppState
{
  MENU_MAIN,
  MENU_UNIV_REMOTE,
  APP_UNIV_BRUTE,
  APP_LEARN,
  APP_FILE_BROWSER,
  APP_REMOTE_VIEW
};
AppState current_state = MENU_MAIN;

int menu_index = 0;
long last_joy_time = 0;

const String main_menu[] = {"Universal Remote", "File Browser", "Learn New"};
const int main_menu_len = 3;
const String univ_menu[] = {"TVs", "ACs", "Projectors"};
const int univ_menu_len = 3;
const String univ_commands[] = {"Power", "Vol +", "Vol -", "Ch +", "Ch -"};
const int univ_commands_len = 5;

// File Browser Variables
#define MAX_DIR_ITEMS 20
String dir_items[MAX_DIR_ITEMS];
bool dir_is_folder[MAX_DIR_ITEMS];
int dir_item_count = 0;
String current_path = "/";
String selected_file_name = "";

// IR Variables
String status_message = "Ready";
int current_frequency = 38;
String current_btn_name = "None";
String current_target_device = "TV";

#define MAX_RAW_BUFFER 500
uint16_t capturedRaw[MAX_RAW_BUFFER];
uint16_t capturedRawLen = 0;

enum LearnPhase
{
  LEARN_IDLE,
  LEARN_LISTENING,
  LEARN_CAPTURED,
  LEARN_SAVING,
  LEARN_SAVED,
  LEARN_ERROR
};

struct SharedState
{
  uint16_t raw[MAX_RAW_BUFFER];
  uint16_t rawLen;
  uint16_t freqKHz;
  String btnName;
  String status;
  String selectedFile;
  String bruteProfilePath;
  bool hasPayload;
  bool bruteActive;
  int bruteIndex;
  bool transmitting;
  LearnPhase learnPhase;
};

enum IrCommandType
{
  IR_CMD_TRANSMIT_CURRENT,
  IR_CMD_BRUTE_START,
  IR_CMD_BRUTE_STOP,
  IR_CMD_LEARN_START,
  IR_CMD_LEARN_STOP,
  IR_CMD_LEARN_SAVE
};

struct IrCommand
{
  IrCommandType type;
  int arg;
};

QueueHandle_t irCommandQueue = NULL;
SemaphoreHandle_t stateMutex = NULL;
SemaphoreHandle_t sdMutex = NULL;

SharedState shared = {
    {0},
    0,
    38,
    "None",
    "Ready",
    "",
    "/univ_tv.ir",
    false,
    false,
    1,
    false,
    LEARN_IDLE};

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
  shared.btnName = btnName;
  shared.hasPayload = safeLen > 0;

  capturedRawLen = safeLen;
  current_frequency = freqKHz;
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

SharedState snapshotShared()
{
  SharedState snap;
  lockState();
  snap = shared;
  unlockState();
  return snap;
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

void drawMenu(String title, const String items[], int len, int selected)
{
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(0, 0, title);
  display.drawLine(0, 12, 128, 12);

  int start_idx = max(0, selected - 1);
  if (start_idx > len - 3 && len >= 3)
    start_idx = len - 3;

  for (int i = 0; i < 3; i++)
  {
    int item_idx = start_idx + i;
    if (item_idx >= len)
      break;

    int y_pos = 16 + (i * 12);
    if (item_idx == selected)
      display.drawString(0, y_pos, "> " + items[item_idx]);
    else
      display.drawString(8, y_pos, items[item_idx]);
  }
  display.drawString(0, 52, "[<] Back | [BTN] Select");
  display.display();
}

// ==========================================
// SD CARD ENGINES (MEMORY SAFE)
// ==========================================
void loadDirectory(String path)
{
  dir_item_count = 0;

  lockSD();
  File dir = SD.open(path);

  if (!dir || !dir.isDirectory())
  {
    if (dir)
      dir.close();
    unlockSD();
    return;
  }

  if (path != "/")
  {
    dir_items[dir_item_count] = ".. (Back)";
    dir_is_folder[dir_item_count] = true;
    dir_item_count++;
  }

  File file = dir.openNextFile();
  while (file && dir_item_count < MAX_DIR_ITEMS)
  {
    String fname = getBaseName(String(file.name()));

    if (!fname.startsWith("._") && !fname.startsWith("System") && fname != "univ_tv.ir")
    {
      dir_items[dir_item_count] = fname;
      dir_is_folder[dir_item_count] = file.isDirectory();
      dir_item_count++;
    }

    file.close();
    yield();
    file = dir.openNextFile();
  }

  if (file)
    file.close();
  dir.close();
  unlockSD();

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
  uint16_t tempRaw[MAX_RAW_BUFFER];
  uint16_t tempRawLen = 0;
  uint16_t tempFrequency = 38;
  String tempBtnName = "None";

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
        tempRawLen = 0;
        tempFrequency = 38;
        tempBtnName = line.substring(6);
      }
      else
      {
        target_found = false;
      }
    }
    else if (target_found && line.startsWith("type: raw"))
    {
      is_raw = true;
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

void transmitCurrentPayload()
{
  uint16_t rawLocal[MAX_RAW_BUFFER];
  uint16_t lenLocal = 0;
  uint16_t freqLocal = 38;
  String btnNameLocal = "None";

  if (!copyPayload(rawLocal, lenLocal, freqLocal, btnNameLocal) || lenLocal == 0)
  {
    setStatus("No payload to send");
    return;
  }

  markTransmitting(true);
  setStatus("Sending " + btnNameLocal);
  irsend.sendRaw(rawLocal, lenLocal, freqLocal);
  vTaskDelay(pdMS_TO_TICKS(250));
  markTransmitting(false);
  setStatus("Sent " + btnNameLocal);
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

  irrecv.resume();

  if (localLen == 0)
  {
    setStatus("Capture failed");
    setLearnPhase(LEARN_ERROR);
    return false;
  }

  setPayload(localRaw, localLen, localFreq, "Captured");
  setStatus("Signal captured");
  setLearnPhase(LEARN_CAPTURED);
  return true;
}

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
        if (current_state == APP_FILE_BROWSER && menu_index > 0)
          menu_index--;
        else if (current_state != APP_FILE_BROWSER)
          menu_index--;
        last_joy_time = current_time;
      }
      if (yVal > 3000)
      {
        if (current_state == APP_FILE_BROWSER && menu_index < dir_item_count - 1)
          menu_index++;
        else if (current_state != APP_FILE_BROWSER)
          menu_index++;
        last_joy_time = current_time;
      }
      if (xVal < 1000)
      {
        if (current_state == MENU_UNIV_REMOTE || current_state == APP_LEARN || current_state == APP_FILE_BROWSER)
        {
          if (current_state == APP_LEARN)
            queueCommand(IR_CMD_LEARN_STOP);
          current_state = MENU_MAIN;
          menu_index = 0;
        }
        else if (current_state == APP_UNIV_BRUTE)
        {
          queueCommand(IR_CMD_BRUTE_STOP);
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
          loadDirectory(current_path);
        }
        if (menu_index == 2)
          current_state = APP_LEARN;
        menu_index = 0;
      }
      else if (current_state == MENU_UNIV_REMOTE)
      {
        if (menu_index == 0)
          current_target_device = "TV";
        if (menu_index == 1)
          current_target_device = "AC";
        if (menu_index == 2)
          current_target_device = "Proj";
        current_state = APP_UNIV_BRUTE;
        menu_index = 0;
      }
      else if (current_state == APP_UNIV_BRUTE)
      {
        if (menu_index < 0)
          menu_index = 0;
        int targetCode = 0;
        if (current_target_device == "AC")
          targetCode = 1;
        else if (current_target_device == "Proj")
          targetCode = 2;
        queueCommand(IR_CMD_BRUTE_START, targetCode);
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

          setSelectedFileName(dir_items[menu_index]);
          loadFlipperCommandByIndex(full_path, 1);
          current_state = APP_REMOTE_VIEW;
        }
      }
      else if (current_state == APP_REMOTE_VIEW)
      {
        queueCommand(IR_CMD_TRANSMIT_CURRENT);
      }
      else if (current_state == APP_LEARN)
      {
        SharedState snap = snapshotShared();
        if (snap.learnPhase == LEARN_CAPTURED)
        {
          setLearnPhase(LEARN_SAVING);
          queueCommand(IR_CMD_LEARN_SAVE);
        }
        else if (snap.learnPhase == LEARN_ERROR || snap.learnPhase == LEARN_IDLE || snap.learnPhase == LEARN_SAVED)
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
      drawMenu("Universal Remote", univ_menu, univ_menu_len, menu_index);
    }
    else if (current_state == APP_FILE_BROWSER)
    {
      String display_items[MAX_DIR_ITEMS];
      for (int i = 0; i < dir_item_count; i++)
      {
        if (dir_is_folder[i] && dir_items[i] != ".. (Back)")
          display_items[i] = "[DIR] " + dir_items[i];
        else
          display_items[i] = dir_items[i];
      }
      if (dir_item_count == 0)
      {
        display.clear();
        display.drawString(0, 0, "Path: " + current_path);
        display.drawLine(0, 12, 128, 12);
        display.drawString(0, 24, "(No items)");
        display.drawString(0, 52, "[<] Back");
        display.display();
      }
      else
      {
        drawMenu("Path: " + current_path, display_items, dir_item_count, menu_index);
      }
    }
    else if (current_state == APP_UNIV_BRUTE)
    {
      if (menu_index < 0)
        menu_index = univ_commands_len - 1;
      if (menu_index >= univ_commands_len)
        menu_index = 0;
      SharedState snap = snapshotShared();

      display.clear();
      display.setTextAlignment(TEXT_ALIGN_LEFT);
      display.drawString(0, 0, current_target_device + " Univ: " + univ_commands[menu_index]);
      display.drawLine(0, 12, 128, 12);

      if (snap.bruteActive)
      {
        display.drawString(0, 25, ">> BLASTING <<");
        display.drawString(0, 35, "Sent: " + snap.btnName);
      }
      else
      {
        display.drawString(0, 25, "Target " + current_target_device + " and press");
        display.drawString(0, 35, snap.status);
      }
      display.drawString(0, 52, "[<] Back | [v^] Change");
      display.display();
    }
    else if (current_state == APP_REMOTE_VIEW)
    {
      SharedState snap = snapshotShared();
      display.clear();
      display.drawString(0, 0, "<- Back        Blaster");
      display.drawLine(0, 12, 128, 12);
      display.drawString(0, 20, "File: " + snap.selectedFile);
      display.drawString(0, 30, "Btn: " + snap.btnName);

      if (snap.transmitting)
        display.drawString(0, 52, "[ TRANSMITTING... ]");
      else
        display.drawString(0, 52, "[<] Back | [BTN] Blast");
      display.display();
    }
    else if (current_state == APP_LEARN)
    {
      SharedState snap = snapshotShared();
      display.clear();
      display.drawString(0, 0, "Learn New Remote");
      display.drawLine(0, 12, 128, 12);
      display.drawString(0, 20, snap.status);

      if (snap.learnPhase == LEARN_LISTENING)
      {
        display.drawString(0, 32, "Waiting for signal...");
        display.drawString(0, 52, "[<] Back");
      }
      else if (snap.learnPhase == LEARN_CAPTURED)
      {
        display.drawString(0, 32, "Captured " + snap.btnName);
        display.drawString(0, 52, "[BTN] Save | [<] Back");
      }
      else if (snap.learnPhase == LEARN_SAVED)
      {
        display.drawString(0, 32, "Saved to /captures/");
        display.drawString(0, 52, "[BTN] Capture Again");
      }
      else if (snap.learnPhase == LEARN_ERROR)
      {
        display.drawString(0, 32, "Capture error");
        display.drawString(0, 52, "[BTN] Retry | [<] Back");
      }
      else
      {
        display.drawString(0, 32, "Preparing listener...");
        display.drawString(0, 52, "[<] Back");
      }
      display.display();
    }

    vTaskDelay(pdMS_TO_TICKS(33));
  }
}

void irTask(void *pvParameters)
{
  bool bruteActive = false;
  int bruteIndex = 1;
  String brutePath = "/univ_tv.ir";
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
      else if (cmd.type == IR_CMD_BRUTE_START)
      {
        brutePath = bruteProfileForCode(cmd.arg);
        lockSD();
        bool exists = SD.exists(brutePath.c_str());
        unlockSD();

        if (!exists)
        {
          setStatus("Missing " + brutePath);
          bruteActive = false;
          setBruteStatus(false, 1, brutePath);
        }
        else
        {
          bruteActive = true;
          bruteIndex = 1;
          setStatus("Brute started");
          setBruteStatus(true, bruteIndex, brutePath);
        }
      }
      else if (cmd.type == IR_CMD_BRUTE_STOP)
      {
        bruteActive = false;
        setBruteStatus(false, bruteIndex, brutePath);
        setStatus("Brute stopped");
      }
      else if (cmd.type == IR_CMD_LEARN_START)
      {
        irrecv.enableIRIn();
        learnListening = true;
        setLearnPhase(LEARN_LISTENING);
        setStatus("Listening...");
      }
      else if (cmd.type == IR_CMD_LEARN_STOP)
      {
        learnListening = false;
        irrecv.disableIRIn();
        setLearnPhase(LEARN_IDLE);
      }
      else if (cmd.type == IR_CMD_LEARN_SAVE)
      {
        saveLearnedCommandToSD();
      }
    }

    if (bruteActive)
    {
      if (loadFlipperCommandByIndex(brutePath, bruteIndex))
      {
        transmitCurrentPayload();
        bruteIndex++;
        setBruteStatus(true, bruteIndex, brutePath);
        vTaskDelay(pdMS_TO_TICKS(350));
      }
      else
      {
        bruteActive = false;
        setBruteStatus(false, bruteIndex, brutePath);
        setStatus("Brute done");
      }
    }

    if (learnListening)
    {
      captureLearnSignal();
    }

    vTaskDelay(pdMS_TO_TICKS(15));
  }
}

// ==========================================
// MAIN SETUP & LOOP
// ==========================================
void setup()
{
  Serial.begin(115200);

  // Power up OLED
  VextON();
  displayReset();
  display.init();
  display.flipScreenVertically();
  display.setFont(ArialMT_Plain_10);

  // Boot IR
  irsend.begin();

  // Sync primitives
  stateMutex = xSemaphoreCreateMutex();
  sdMutex = xSemaphoreCreateMutex();
  irCommandQueue = xQueueCreate(12, sizeof(IrCommand));

  if (!stateMutex || !sdMutex || !irCommandQueue)
  {
    Serial.println("RTOS init failed");
    while (1)
      delay(1000);
  }

  // Boot SD
  pinMode(SD_MISO, INPUT_PULLUP);
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  lockSD();
  bool sdOk = SD.begin(SD_CS, sdSPI, 4000000);
  unlockSD();

  if (!sdOk)
  {
    Serial.println("SD Failed");
    setStatus("SD mount failed");
  }
  else
  {
    Serial.println("SD Mounted");
    setStatus("Ready");
  }

  xTaskCreatePinnedToCore(uiTask, "UI_Task", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(irTask, "IR_Task", 4096, NULL, 2, NULL, 0);
}

void loop() { vTaskDelete(NULL); }