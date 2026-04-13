#include <Arduino.h>
#include <Wire.h>
#include "SSD1306Wire.h"
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRutils.h>
#include <IRsend.h>
#include <SPI.h>
#include <SD.h>

// ==========================================
// PIN DEFINITIONS
// ==========================================
#define JOY_X 3       
#define JOY_Y 4       
#define JOY_BTN 5     
const uint16_t kIrRecvPin = 6; 
const uint16_t kIrLedPin = 7;   

// SD Card SPI Pins (Conflict-Free Right Side)
#define SD_MISO 34 //11
#define SD_SCK 33 //12
#define SD_MOSI 47 //13
#define SD_CS 48 //14

// ==========================================
// GLOBALS & OBJECTS
// ==========================================
SSD1306Wire display(0x3c, SDA_OLED, SCL_OLED); 
IRrecv irrecv(kIrRecvPin, 1024, 50, true); 
decode_results results;
IRsend irsend(kIrLedPin);

// Create a solid global SPI highway (Prevents pointer leaks)
SPIClass sdSPI(FSPI); 

enum AppState { MENU_MAIN, MENU_UNIV_REMOTE, APP_UNIV_BRUTE, APP_LEARN, APP_FILE_BROWSER, APP_REMOTE_VIEW };
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
volatile bool is_brute_forcing = false;
volatile int current_brute_index = 1; 
volatile bool transmit_requested = false;

String status_message = "Ready";
int current_frequency = 38; 
String current_btn_name = "None";
String current_target_device = "TV";

#define MAX_RAW_BUFFER 300
uint16_t capturedRaw[MAX_RAW_BUFFER];
uint16_t capturedRawLen = 0;

// ==========================================
// HELPER FUNCTIONS 
// ==========================================
void VextON(void) { pinMode(Vext, OUTPUT); digitalWrite(Vext, LOW); }
void displayReset(void) {
  pinMode(RST_OLED, OUTPUT); digitalWrite(RST_OLED, HIGH); delay(1); 
  digitalWrite(RST_OLED, LOW); delay(1); digitalWrite(RST_OLED, HIGH); delay(1);
}

String getBaseName(String path) {
  int lastSlash = path.lastIndexOf('/');
  if (lastSlash >= 0) return path.substring(lastSlash + 1);
  return path;
}

String getParentDir(String path) {
  if (path == "/" || path == "") return "/";
  int lastSlash = path.lastIndexOf('/');
  if (lastSlash == 0) return "/"; 
  return path.substring(0, lastSlash);
}

void drawMenu(String title, const String items[], int len, int selected) {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(0, 0, title);
  display.drawLine(0, 12, 128, 12);
  
  int start_idx = max(0, selected - 1);
  if (start_idx > len - 3 && len >= 3) start_idx = len - 3;
  
  for (int i = 0; i < 3; i++) {
    int item_idx = start_idx + i;
    if (item_idx >= len) break;
    
    int y_pos = 16 + (i * 12);
    if (item_idx == selected) display.drawString(0, y_pos, "> " + items[item_idx]);
    else display.drawString(8, y_pos, items[item_idx]);
  }
  display.drawString(0, 52, "[<] Back | [BTN] Select");
  display.display();
}

// ==========================================
// SD CARD ENGINES (MEMORY SAFE)
// ==========================================
void loadDirectory(String path) {
  dir_item_count = 0;
  File dir = SD.open(path);
  
  if (!dir || !dir.isDirectory()) {
    if(dir) dir.close();
    return;
  }

  if (path != "/") {
    dir_items[dir_item_count] = ".. (Back)";
    dir_is_folder[dir_item_count] = true;
    dir_item_count++;
  }

  File file = dir.openNextFile();
  while (file && dir_item_count < MAX_DIR_ITEMS) {
    String fname = getBaseName(String(file.name()));
    
    if (!fname.startsWith("._") && !fname.startsWith("System") && fname != "univ_tv.ir") {
       dir_items[dir_item_count] = fname;
       dir_is_folder[dir_item_count] = file.isDirectory();
       dir_item_count++;
    }
    
    file.close(); 
    yield(); 
    file = dir.openNextFile();
  }
  
  if(file) file.close();
  dir.close(); 
  
  menu_index = 0; 
}

bool loadFlipperCommandByIndex(String filepath, int target_index) {
  File file = SD.open(filepath);
  if (!file) { status_message = "File Missing"; return false; }

  int command_count = 0;
  bool target_found = false;
  bool is_raw = false;
  capturedRawLen = 0;
  current_frequency = 38; 

  while(file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();

    if (line.startsWith("name: ")) {
      command_count++;
      if (command_count == target_index) {
        target_found = true;
        current_btn_name = line.substring(6);
      } else {
        target_found = false; 
      }
    }
    else if (target_found && line.startsWith("type: raw")) {
      is_raw = true;
    }
    else if (target_found && is_raw && line.startsWith("frequency: ")) {
      current_frequency = line.substring(11).toInt() / 1000; 
    }
    else if (target_found && is_raw && line.startsWith("data: ")) {
      String dataStr = line.substring(6);
      int startIdx = 0;
      int spaceIdx = dataStr.indexOf(' ');
      
      while (spaceIdx != -1) {
        if(capturedRawLen < MAX_RAW_BUFFER) {
          capturedRaw[capturedRawLen++] = dataStr.substring(startIdx, spaceIdx).toInt();
        }
        startIdx = spaceIdx + 1;
        spaceIdx = dataStr.indexOf(' ', startIdx);
        yield(); 
      }
      if(startIdx < dataStr.length() && capturedRawLen < MAX_RAW_BUFFER) {
        capturedRaw[capturedRawLen++] = dataStr.substring(startIdx).toInt();
      }
      
      file.close();
      return true; 
    }
  }
  file.close();
  return false; 
}

// ==========================================
// FREERTOS TASKS
// ==========================================
void uiTask(void *pvParameters) {
  pinMode(JOY_BTN, INPUT_PULLUP); 
  bool lastBtnState = HIGH;

  while(1) {
    int xVal = analogRead(JOY_X);
    int yVal = analogRead(JOY_Y);
    bool currentBtnState = digitalRead(JOY_BTN); 
    long current_time = millis();

    if (current_time - last_joy_time > 250) {
      if (yVal < 1000) { 
        if (current_state == APP_FILE_BROWSER && menu_index > 0) menu_index--;
        else if (current_state != APP_FILE_BROWSER) menu_index--; 
        last_joy_time = current_time;
      }
      if (yVal > 3000) { 
        if (current_state == APP_FILE_BROWSER && menu_index < dir_item_count - 1) menu_index++;
        else if (current_state != APP_FILE_BROWSER) menu_index++; 
        last_joy_time = current_time;
      }
      if (xVal < 1000) { 
        if (current_state == MENU_UNIV_REMOTE || current_state == APP_LEARN || current_state == APP_FILE_BROWSER) {
          current_state = MENU_MAIN; menu_index = 0;
        } 
        else if (current_state == APP_UNIV_BRUTE) {
          current_state = MENU_UNIV_REMOTE; menu_index = 0; is_brute_forcing = false; 
        }
        else if (current_state == APP_REMOTE_VIEW) {
          current_state = APP_FILE_BROWSER; 
        }
        last_joy_time = current_time;
      }
    }

    if (currentBtnState == LOW && lastBtnState == HIGH) {
      if (current_state == MENU_MAIN) {
        if (menu_index == 0) current_state = MENU_UNIV_REMOTE;
        if (menu_index == 1) { 
          current_state = APP_FILE_BROWSER; 
          current_path = "/"; 
          loadDirectory(current_path); 
        }
        if (menu_index == 2) current_state = APP_LEARN;
        menu_index = 0;
      } 
      else if (current_state == MENU_UNIV_REMOTE) {
        if (menu_index == 0) current_target_device = "TV";
        if (menu_index == 1) current_target_device = "AC";
        if (menu_index == 2) current_target_device = "Proj";
        current_state = APP_UNIV_BRUTE;
        menu_index = 0;
      }
      else if (current_state == APP_UNIV_BRUTE) {
        is_brute_forcing = true;
        current_brute_index = 1; 
      }
      else if (current_state == APP_FILE_BROWSER) {
        if (dir_is_folder[menu_index]) {
          if (dir_items[menu_index] == ".. (Back)") {
            current_path = getParentDir(current_path);
          } else {
            if (current_path == "/") current_path = "/" + dir_items[menu_index];
            else current_path = current_path + "/" + dir_items[menu_index];
          }
          loadDirectory(current_path);
        } 
        else {
          String full_path = "";
          if (current_path == "/") full_path = "/" + dir_items[menu_index];
          else full_path = current_path + "/" + dir_items[menu_index];
          
          selected_file_name = dir_items[menu_index];
          loadFlipperCommandByIndex(full_path, 1); 
          current_state = APP_REMOTE_VIEW;
        }
      }
      else if (current_state == APP_REMOTE_VIEW) {
        if (capturedRawLen > 0) transmit_requested = true;
      }
    }
    lastBtnState = currentBtnState;

    if (current_state == MENU_MAIN) {
      if (menu_index < 0) menu_index = main_menu_len - 1;
      if (menu_index >= main_menu_len) menu_index = 0;
      drawMenu("SecuriTea OS", main_menu, main_menu_len, menu_index);
    } 
    else if (current_state == MENU_UNIV_REMOTE) {
      if (menu_index < 0) menu_index = univ_menu_len - 1;
      if (menu_index >= univ_menu_len) menu_index = 0;
      drawMenu("Universal Remote", univ_menu, univ_menu_len, menu_index);
    }
    else if (current_state == APP_FILE_BROWSER) {
      String display_items[MAX_DIR_ITEMS];
      for(int i=0; i<dir_item_count; i++) {
        if (dir_is_folder[i] && dir_items[i] != ".. (Back)") display_items[i] = "[DIR] " + dir_items[i];
        else display_items[i] = dir_items[i];
      }
      drawMenu("Path: " + current_path, display_items, dir_item_count, menu_index);
    }
    else if (current_state == APP_UNIV_BRUTE) {
      if (menu_index < 0) menu_index = univ_commands_len - 1;
      if (menu_index >= univ_commands_len) menu_index = 0;
      
      display.clear();
      display.setTextAlignment(TEXT_ALIGN_LEFT);
      display.drawString(0, 0, current_target_device + " Univ: " + univ_commands[menu_index]);
      display.drawLine(0, 12, 128, 12);
      
      if (is_brute_forcing) {
        display.drawString(0, 25, ">> BLASTING <<");
        display.drawString(0, 35, "Sent: " + current_btn_name);
      } else {
        display.drawString(0, 25, "Target " + current_target_device + " and press");
        display.drawString(0, 35, "button to brute force.");
      }
      display.drawString(0, 52, "[<] Back | [v^] Change");
      display.display();
    }
    else if (current_state == APP_REMOTE_VIEW) {
      display.clear();
      display.drawString(0, 0, "<- Back        Blaster");
      display.drawLine(0, 12, 128, 12);
      display.drawString(0, 20, "File: " + selected_file_name);
      display.drawString(0, 30, "Btn: " + current_btn_name);
      
      if (transmit_requested) display.drawString(0, 52, "[ TRANSMITTING... ]");
      else display.drawString(0, 52, "[<] Back | [BTN] Blast");
      display.display();
    }
    vTaskDelay(pdMS_TO_TICKS(33)); 
  }
}

void irTask(void *pvParameters) {
  while(1) {
    if (is_brute_forcing) {
      if (loadFlipperCommandByIndex("/univ_tv.ir", current_brute_index)) {
        irsend.sendRaw(capturedRaw, capturedRawLen, current_frequency); 
        current_brute_index++;
        vTaskDelay(pdMS_TO_TICKS(400)); 
      } else {
        is_brute_forcing = false; 
        current_btn_name = "Done!";
      }
    }
    if (transmit_requested) {
      if (capturedRawLen > 0) irsend.sendRaw(capturedRaw, capturedRawLen, current_frequency); 
      vTaskDelay(pdMS_TO_TICKS(500)); 
      transmit_requested = false;
    }
    vTaskDelay(pdMS_TO_TICKS(15)); 
  }
}

// ==========================================
// MAIN SETUP & LOOP
// ==========================================
void setup() {
  Serial.begin(115200);

  // Power up OLED
  VextON();
  displayReset();
  display.init();
  display.flipScreenVertically();
  display.setFont(ArialMT_Plain_10);

  // Boot IR
  irsend.begin();
  irrecv.enableIRIn(); 

  // Boot SD
  pinMode(SD_MISO, INPUT_PULLUP);
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  if (!SD.begin(SD_CS, sdSPI, 4000000)) {
    Serial.println("SD Failed");
  } else {
    Serial.println("SD Mounted");
  }

  xTaskCreatePinnedToCore(uiTask, "UI_Task", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(irTask, "IR_Task", 4096, NULL, 2, NULL, 0); 
}

void loop() { vTaskDelete(NULL); }