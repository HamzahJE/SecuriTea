#include <Arduino.h>
#include <Wire.h>
#include "SSD1306Wire.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "board_config.h"
#include "app_state.h"
#include "ir_file.h"
#include "ir_engine.h"
#include "storage.h"
#include "ui_layout.h"
#include "ui.h"
#include "input.h"
#include "app.h"
#include "ir_app.h"

// ==========================================
// HARDWARE OBJECTS
// ==========================================
SSD1306Wire g_display(board::kOledAddress, SDA_OLED, SCL_OLED);

// ==========================================
// APPS (uiTask owns their state; see app.h)
// ==========================================
IrApp g_irApp;
Launcher g_launcher;

// ==========================================
// IR-TASK-OWNED STATE (written by irTask only)
// ==========================================
CommandIndex g_irIndex; // index of the file a send-all job is working through

// ==========================================
// SHARED STATE AND RTOS OBJECTS
// ==========================================
SharedState g_shared;
std::atomic<uint32_t> g_sendGeneration{0}; // bumped by cancelSendJobs(); see helpers.ino

QueueHandle_t g_irCommandQueue = NULL;
QueueHandle_t g_irEventQueue = NULL;
SemaphoreHandle_t g_stateMutex = NULL; // the SD lock lives inside storage.cpp
TaskHandle_t g_uiTaskHandle = NULL;
TaskHandle_t g_irTaskHandle = NULL;

constexpr uint32_t kUiTaskStackBytes = 8192;
constexpr uint32_t kIrTaskStackBytes = 8192;
constexpr UBaseType_t kUiTaskPriority = 1;
constexpr UBaseType_t kIrTaskPriority = 2;
constexpr int kIrCommandQueueLen = 12;
constexpr int kIrEventQueueLen = 8;

// ==========================================
// MAIN SETUP & LOOP
// ==========================================
void setup()
{
    Serial.begin(115200);

    // Power up OLED
    VextON();
    displayReset();
    g_display.init();
    g_display.flipScreenVertically();
    g_display.setFont(ArialMT_Plain_10);

    // Boot IR
    irEngineBegin();

    // Sync primitives
    g_stateMutex = xSemaphoreCreateMutex();
    g_irCommandQueue = xQueueCreate(kIrCommandQueueLen, sizeof(IrCommand));
    g_irEventQueue = xQueueCreate(kIrEventQueueLen, sizeof(IrEvent));

    if (!g_stateMutex || !g_irCommandQueue || !g_irEventQueue)
    {
        Serial.println("RTOS init failed");
        while (true)
        {
            delay(1000);
        }
    }

    // Boot SD. The sequence is the original firmware's, call for call: bus up, one second,
    // one mount. storageInitBus() and storageMount() contain exactly those calls.
    if (!storageInitBus())
    {
        Serial.println("SD lock creation failed");
        while (true)
        {
            delay(1000);
        }
    }
    delay(kSdPowerSettleMs);
    bool mounted = storageMount();

    // The first boot after an upload often finds the card still recovering: during the upload
    // the chip's pins float for twenty seconds or more, the card's chip select among them, and
    // a card with no pull-up on that line is clocked with noise the whole time. Pressing RST a
    // moment later always worked. So instead of giving up after one try, wait and try again,
    // a few times, with the card left alone in between. This never runs on a mounted card.
    for (int attempt = 2; !mounted && attempt <= kSdBootMountTries; attempt++)
    {
        Serial.printf("SD mount failed, waiting %lu ms before try %d of %d\n", kSdBootMountRetryMs, attempt,
                      kSdBootMountTries);
        g_display.clear();
        g_display.drawString(0, 0, "Waiting for SD card");
        g_display.drawString(0, 14, "try " + String(attempt) + " of " + String(kSdBootMountTries));
        g_display.display();
        delay(kSdBootMountRetryMs);
        mounted = storageMount();
    }

    if (!mounted)
    {
        Serial.println("SD Failed");
        setStatus("SD mount failed");
    }
    else
    {
        Serial.println("SD Mounted");
        setStatus("Ready");
    }

    logBootDiagnostics(); // after the mount, so nothing new runs before it
    g_launcher.add(g_irApp);

    BaseType_t uiOk =
        xTaskCreatePinnedToCore(uiTask, "UI_Task", kUiTaskStackBytes, NULL, kUiTaskPriority, &g_uiTaskHandle, 1);
    BaseType_t irOk =
        xTaskCreatePinnedToCore(irTask, "IR_Task", kIrTaskStackBytes, NULL, kIrTaskPriority, &g_irTaskHandle, 0);
    if (uiOk != pdPASS || irOk != pdPASS)
    {
        Serial.println("Task creation failed");
        while (true)
        {
            delay(1000);
        }
    }
}

void loop()
{
    vTaskDelete(NULL);
}
