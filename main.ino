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
const String univ_menu[] = {"TVs", "ACs", "Projectors", "Audio"};
const int univ_menu_len = 4;

#define MAX_UNIV_ITEMS 60
String univ_cmd_items[MAX_UNIV_ITEMS];
int univ_cmd_count = 0;
String univ_profile_path = "/univ_tv.ir";

// File Browser Variables
#define MAX_DIR_ITEMS 20
String dir_items[MAX_DIR_ITEMS];
bool dir_is_folder[MAX_DIR_ITEMS];
int dir_item_count = 0;
String current_path = "/";
String selected_file_name = "";
String selected_file_path = "";
int remote_cmd_index = 1;
int remote_cmd_count = 1;

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
  bool payloadIsParsed;
  String parsedProtocol;
  uint32_t parsedAddress;
  uint32_t parsedCommand;
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

struct UiSnapshot
{
  String btnName;
  String status;
  String selectedFile;
  bool bruteActive;
  int bruteIndex;
  bool transmitting;
  LearnPhase learnPhase;
};

enum IrCommandType
{
  IR_CMD_TRANSMIT_CURRENT,
  IR_CMD_UNIV_SEND,
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
    false,
    "",
    0,
    0,
    "None",
    "Ready",
    "",
    "/univ_tv.ir",
    false,
    false,
    1,
    false,
    LEARN_IDLE};

String display_items_buffer[MAX_DIR_ITEMS];

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

void transmitCurrentPayload()
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
    return;
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
      irsend.sendSAMSUNG(data, 32);
    }
    else if (p == "NECEXT")
    {
      uint32_t data = ((uint32_t)cmd16 << 16) | (uint32_t)addr16;
      irsend.sendNEC(data, 32);
    }
    else if (p == "NEC")
    {
      uint32_t data = ((uint32_t)addr8) | ((uint32_t)(addr8 ^ 0xFF) << 8) |
                      ((uint32_t)cmd8 << 16) | ((uint32_t)(cmd8 ^ 0xFF) << 24);
      irsend.sendNEC(data, 32);
    }
    else if (p == "RC5")
    {
      uint16_t data = (uint16_t)(((addr8 & 0x1F) << 6) | (cmd8 & 0x3F));
      irsend.sendRC5(data, 12);
    }
    else if (p == "RC6")
    {
      uint32_t data = ((uint32_t)(addr8 & 0xFF) << 8) | (uint32_t)(cmd8 & 0xFF);
      irsend.sendRC6(data, 20);
    }
    else if (p == "SIRC" || p == "SONY")
    {
      uint16_t data = (uint16_t)((cmd8 & 0x7F) | ((addr8 & 0x1F) << 7));
      irsend.sendSony(data, 12);
    }
    else
    {
      markTransmitting(false);
      setStatus("Unsupported protocol");
      return;
    }
  }
  else
  {
    if (lenLocal == 0)
    {
      markTransmitting(false);
      setStatus("No raw payload");
      return;
    }
    irsend.sendRaw(rawLocal, lenLocal, freqLocal);
  }

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
        if (current_state == APP_REMOTE_VIEW)
        {
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
        if (current_state == APP_REMOTE_VIEW)
        {
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
        if (menu_index == 3)
          current_target_device = "Audio";

        univ_profile_path = bruteProfileForTarget(current_target_device);
        loadUniversalCommandList(univ_profile_path);
        setBruteStatus(false, 1, univ_profile_path);
        setStatus("Select command");

        current_state = APP_UNIV_BRUTE;
        menu_index = 0;
      }
      else if (current_state == APP_UNIV_BRUTE)
      {
        if (univ_cmd_count > 0)
        {
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
        queueCommand(IR_CMD_TRANSMIT_CURRENT);
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
        display.drawString(0, 0, "Path: " + current_path);
        display.drawLine(0, 12, 128, 12);
        display.drawString(0, 24, "(No items)");
        display.drawString(0, 52, "[<] Back");
        display.display();
      }
      else
      {
        drawMenu("Path: " + current_path, display_items_buffer, dir_item_count, menu_index);
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
        display.setTextAlignment(TEXT_ALIGN_LEFT);
        display.drawString(0, 0, current_target_device + " Remote");
        display.drawLine(0, 12, 128, 12);
        display.drawString(0, 24, "No commands found");
        display.drawString(0, 36, "File: " + univ_profile_path);
        display.drawString(0, 52, "[<] Back");
        display.display();
      }
      else
      {
        drawMenu(current_target_device + " Remote", univ_cmd_items, univ_cmd_count, menu_index);
        display.drawString(0, 52, snap.status);
        display.display();
      }
    }
    else if (current_state == APP_REMOTE_VIEW)
    {
      UiSnapshot snap = snapshotUi();
      display.clear();
      display.drawString(0, 0, "<- Back        Blaster");
      display.drawLine(0, 12, 128, 12);
      display.drawString(0, 20, "File: " + snap.selectedFile);
      display.drawString(0, 30, "Btn: " + snap.btnName);
      display.drawString(0, 40, "Cmd: " + String(remote_cmd_index) + "/" + String(remote_cmd_count));
      display.drawString(0, 50, snap.status);

      if (snap.transmitting)
        display.drawString(0, 58, "[ TRANSMITTING... ]");
      else
        display.drawString(0, 58, "[v^] Cmd | [BTN] Blast");
      display.display();
    }
    else if (current_state == APP_LEARN)
    {
      UiSnapshot snap = snapshotUi();
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
          }
          else
          {
            for (int i = 0; i < matchCount; i++)
            {
              int commandIndex = matchIndices[i];
              if (loadFlipperCommandByIndex(profilePath, commandIndex))
              {
                setStatus("Sending " + groupName + " " + String(i + 1) + "/" + String(matchCount));
                setBruteStatus(false, commandIndex, profilePath);
                transmitCurrentPayload();
                vTaskDelay(pdMS_TO_TICKS(180));
              }
            }
            setStatus("Sent group " + groupName);
          }
        }
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

  xTaskCreatePinnedToCore(uiTask, "UI_Task", 8192, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(irTask, "IR_Task", 6144, NULL, 2, NULL, 0);
}

void loop() { vTaskDelete(NULL); }