#pragma once

#include <Arduino.h>

// Screen layout for the 128x64 OLED with the 10 px font. The y coordinates are the top of
// each text row. Shared by the renderer below and by screens that draw lines directly.
constexpr int kUiHeaderY = 0;
constexpr int kUiDividerY = 12;
constexpr int kUiListStartY = 16;
constexpr int kUiListStepY = 12;
constexpr int kUiLine1Y = 20;
constexpr int kUiLine2Y = 32;
constexpr int kUiLine3Y = 44;
constexpr int kUiFooterY = 54;

String trimToWidth(const String &s, int maxChars);

// First row shown for a 3-row list window that keeps `selected` visible.
int listWindowStart(int selected, int len);

void drawHeader(const String &title, const String &rightBadge = "", bool badgeSelected = false);
void drawFooter(const String &text);

void drawMenu(const String &title,
              const String items[],
              int len,
              int selected,
              const String &footerText = "[<] Back | [BTN] Select",
              const String &headerBadge = "",
              bool autoDisplay = true,
              bool headerBadgeSelected = false);

void drawUniversalProgressPopup(const String &name, int current, int total);
