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
#include "ui_layout.h"

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
#define MAX_DIR_ITEMS 260
String dir_items[MAX_DIR_ITEMS];
bool dir_is_folder[MAX_DIR_ITEMS];
int dir_item_count = 0;

#define MAX_DIR_CACHE 260
String dir_cache_items[MAX_DIR_CACHE];
bool dir_cache_is_folder[MAX_DIR_CACHE];
int dir_cache_count = 0;
String dir_cache_path = "";
bool dir_cache_valid = false;

String current_path = "/";
String selected_file_name = "";
String selected_file_path = "";
int remote_cmd_index = 1;
int remote_cmd_count = 1;
int file_filter_index = 0; // 0=ALL, 1-26=A-Z, 27=#

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
  bool univSending;
  int univProgressCurrent;
  int univProgressTotal;
  String univProgressName;
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
  bool univSending;
  int univProgressCurrent;
  int univProgressTotal;
  String univProgressName;
  LearnPhase learnPhase;
};

enum IrCommandType
{
  IR_CMD_TRANSMIT_CURRENT,
  IR_CMD_REMOTE_SEND_ALL,
  IR_CMD_UNIV_SEND,
  IR_CMD_LEARN_START,
  IR_CMD_LEARN_STOP,
  IR_CMD_LEARN_SAVE
};

enum RemoteSendMode
{
  REMOTE_SEND_ONE,
  REMOTE_SEND_LOOP
};
RemoteSendMode remote_send_mode = REMOTE_SEND_ONE;

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
    false,
    0,
    0,
    "",
    LEARN_IDLE};

const uint16_t UNIVERSAL_SEND_DELAY_MS = 120;
const uint8_t UNIVERSAL_TRANSMIT_REPEATS = 1;
const uint8_t UNIVERSAL_LOAD_RETRIES = 3;
const uint16_t UNIVERSAL_REPEAT_GAP_MS = 80;
const uint8_t PROTOCOL_FRAME_REPEAT = 1;

enum UniversalSendMode
{
  UNIV_MODE_SINGLE,
  UNIV_MODE_AGGRESSIVE
};
UniversalSendMode universal_mode = UNIV_MODE_SINGLE;

const uint16_t AGGR_SEND_DELAY_MS = 180;
const uint8_t AGGR_TRANSMIT_REPEATS = 2;
const uint8_t AGGR_LOAD_RETRIES = 3;
const uint16_t AGGR_REPEAT_GAP_MS = 80;
const uint8_t AGGR_PROTOCOL_FRAME_REPEAT = 1;

const uint16_t SINGLE_SEND_DELAY_MS = 120;
const uint8_t SINGLE_TRANSMIT_REPEATS = 1;
const uint8_t SINGLE_LOAD_RETRIES = 2;
const uint16_t SINGLE_REPEAT_GAP_MS = 60;
const uint8_t SINGLE_PROTOCOL_FRAME_REPEAT = 0;

volatile bool univ_cancel_requested = false;

String display_items_buffer[MAX_DIR_ITEMS];

const int UI_HEADER_Y = 0;
const int UI_DIVIDER_Y = 12;
const int UI_LIST_START_Y = 16;
const int UI_LIST_STEP_Y = 12;
const int UI_LINE_1_Y = 20;
const int UI_LINE_2_Y = 32;
const int UI_LINE_3_Y = 44;
const int UI_FOOTER_Y = 54;

bool headerModeSelected = false;
bool remoteModeSelected = false;

uint16_t currentSendDelayMs = UNIVERSAL_SEND_DELAY_MS;
uint8_t currentTransmitRepeats = UNIVERSAL_TRANSMIT_REPEATS;
uint8_t currentLoadRetries = UNIVERSAL_LOAD_RETRIES;
uint16_t currentRepeatGapMs = UNIVERSAL_REPEAT_GAP_MS;
uint8_t currentProtocolFrameRepeat = PROTOCOL_FRAME_REPEAT;

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
  applyUniversalModeSettings();

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