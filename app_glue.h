#pragma once
// Functions and objects defined in the sketch (.ino files) that app code in .cpp files may use.
// The sketch is one translation unit with generated prototypes; a .cpp needs these spelled out.

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <atomic>
#include "app_state.h"

// Shared state (helpers.ino). Each takes g_stateMutex briefly; never call while holding another lock.
void setStatus(const String &msg);
void setLearnPhase(LearnPhase phase);
void setButtonName(const String &name);
LearnPhase getLearnPhaseSnapshot();

// Cancel (helpers.ino): bumps the generation every queued send job is stamped with.
void cancelSendJobs();

// Storage glue (storage_ir.ino): storage results turned into status text.
int buildCommandIndex(const String &path, CommandIndex &index);
bool publishCommandAt(const String &path, uint32_t offset);

// RTOS objects (securitea.ino).
extern QueueHandle_t g_irCommandQueue;
extern std::atomic<uint32_t> g_sendGeneration;
