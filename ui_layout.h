#pragma once

#include <Arduino.h>

String trimToWidth(const String &s, int maxChars);

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

void drawStatusPopup(const String &status);
void drawUniversalProgressPopup(const String &name, int current, int total);
