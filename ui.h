#pragma once
// ui module: draws the screens of the IR app.
//
// The only input is a UiModel, filled by uiTask once per frame. This file knows nothing about
// the task, the SD card or the IR hardware; it turns a model into pixels through ui_layout.

#include "app_state.h"

// Draws the screen for m.state and sends it to the display.
void drawScreen(const UiModel &m);
