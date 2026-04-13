#include "ui_layout.h"

#include "SSD1306Wire.h"

extern SSD1306Wire display;

static constexpr int kUiHeaderY = 0;
static constexpr int kUiDividerY = 12;
static constexpr int kUiListStartY = 16;
static constexpr int kUiListStepY = 12;
static constexpr int kUiFooterY = 54;

String trimToWidth(const String &s, int maxChars)
{
    if ((int)s.length() <= maxChars)
        return s;
    if (maxChars <= 3)
        return s.substring(0, maxChars);
    return s.substring(0, maxChars - 3) + "...";
}

void drawHeader(const String &title, const String &rightBadge, bool badgeSelected)
{
    display.setTextAlignment(TEXT_ALIGN_LEFT);

    if (rightBadge.length() == 0)
    {
        display.drawString(0, kUiHeaderY, title);
        display.drawLine(0, kUiDividerY, 128, kUiDividerY);
        return;
    }

    const int textW = (int)rightBadge.length() * 6;
    const int boxW = min(58, max(28, textW + 10));
    const int boxH = 11;
    const int boxX = 128 - boxW;
    const int boxY = 0;
    const int titleMaxChars = max(6, (boxX - 2) / 6);

    display.drawString(0, kUiHeaderY, trimToWidth(title, titleMaxChars));

    if (badgeSelected)
        display.drawRect(boxX, boxY, boxW, boxH);

    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(boxX + boxW / 2, 1, rightBadge);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.drawLine(0, kUiDividerY, 128, kUiDividerY);
}

void drawFooter(const String &text)
{
    display.setTextAlignment(TEXT_ALIGN_LEFT);

    const int kVisibleChars = 21;
    if ((int)text.length() <= kVisibleChars)
    {
        display.drawString(0, kUiFooterY, text);
        return;
    }

    static String lastText = "";
    static int offset = 0;
    static unsigned long lastStepMs = 0;
    const unsigned long now = millis();

    if (text != lastText)
    {
        lastText = text;
        offset = 0;
        lastStepMs = now;
    }

    if (now - lastStepMs >= 220)
    {
        offset++;
        lastStepMs = now;
    }

    const String spacer = "   ";
    const String loopText = text + spacer + text;
    const int cycleLen = text.length() + spacer.length();
    if (offset >= cycleLen)
        offset = 0;

    String shown = loopText.substring(offset, offset + kVisibleChars);
    display.drawString(0, kUiFooterY, shown);
}

void drawMenu(const String &title,
              const String items[],
              int len,
              int selected,
              const String &footerText,
              const String &headerBadge,
              bool autoDisplay,
              bool headerBadgeSelected)
{
    display.clear();
    drawHeader(title, headerBadge, headerBadgeSelected);

    int start_idx = max(0, selected - 1);
    if (start_idx > len - 3 && len >= 3)
        start_idx = len - 3;

    for (int i = 0; i < 3; i++)
    {
        int item_idx = start_idx + i;
        if (item_idx >= len)
            break;

        int y_pos = kUiListStartY + (i * kUiListStepY);
        if (item_idx == selected)
            display.drawString(0, y_pos, "> " + items[item_idx]);
        else
            display.drawString(8, y_pos, items[item_idx]);
    }

    drawFooter(footerText);
    if (autoDisplay)
        display.display();
}

void drawStatusPopup(const String &status)
{
    if (status.length() == 0)
        return;

    String shown = status;
    if (shown.length() > 20)
        shown = shown.substring(0, 20) + "...";

    int x = 6;
    int y = 20;
    int w = 116;
    int h = 22;

    display.setColor(BLACK);
    display.fillRect(x, y, w, h);
    display.setColor(WHITE);
    display.drawRect(x, y, w, h);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 27, shown);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
}

void drawUniversalProgressPopup(const String &name, int current, int total)
{
    if (total <= 0)
        return;

    String shown = name;
    if (shown.length() > 12)
        shown = shown.substring(0, 12);

    String text = shown + " " + String(current) + "/" + String(total);

    int x = 12;
    int y = 20;
    int w = 104;
    int h = 24;

    display.setColor(BLACK);
    display.fillRect(x, y, w, h);
    display.setColor(WHITE);
    display.drawRect(x, y, w, h);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 28, text);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
}
