#include "ui_layout.h"

#include "SSD1306Wire.h"

extern SSD1306Wire g_display;

int listWindowStart(int selected, int len)
{
    int start = max(0, selected - 1);
    if (start > len - 3 && len >= 3)
    {
        start = len - 3;
    }
    return start;
}

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
    g_display.setTextAlignment(TEXT_ALIGN_LEFT);

    if (rightBadge.length() == 0)
    {
        g_display.drawString(0, kUiHeaderY, title);
        g_display.drawLine(0, kUiDividerY, 128, kUiDividerY);
        return;
    }

    const int textW = (int)rightBadge.length() * 6;
    const int boxW = min(58, max(28, textW + 10));
    const int boxH = 11;
    const int boxX = 128 - boxW;
    const int boxY = 0;
    const int titleMaxChars = max(6, (boxX - 2) / 6);

    g_display.drawString(0, kUiHeaderY, trimToWidth(title, titleMaxChars));

    if (badgeSelected)
        g_display.drawRect(boxX, boxY, boxW, boxH);

    g_display.setTextAlignment(TEXT_ALIGN_CENTER);
    g_display.drawString(boxX + boxW / 2, 1, rightBadge);
    g_display.setTextAlignment(TEXT_ALIGN_LEFT);
    g_display.drawLine(0, kUiDividerY, 128, kUiDividerY);
}

void drawFooter(const String &text)
{
    g_display.setTextAlignment(TEXT_ALIGN_LEFT);

    const int kVisibleChars = 21;
    if ((int)text.length() <= kVisibleChars)
    {
        g_display.drawString(0, kUiFooterY, text);
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
    g_display.drawString(0, kUiFooterY, shown);
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
    g_display.clear();
    drawHeader(title, headerBadge, headerBadgeSelected);

    int start_idx = listWindowStart(selected, len);

    for (int i = 0; i < 3; i++)
    {
        int item_idx = start_idx + i;
        if (item_idx >= len)
            break;

        int y_pos = kUiListStartY + (i * kUiListStepY);
        if (item_idx == selected)
            g_display.drawString(0, y_pos, "> " + items[item_idx]);
        else
            g_display.drawString(8, y_pos, items[item_idx]);
    }

    drawFooter(footerText);
    if (autoDisplay)
        g_display.display();
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

    g_display.setColor(BLACK);
    g_display.fillRect(x, y, w, h);
    g_display.setColor(WHITE);
    g_display.drawRect(x, y, w, h);
    g_display.setTextAlignment(TEXT_ALIGN_CENTER);
    g_display.drawString(64, 28, text);
    g_display.setTextAlignment(TEXT_ALIGN_LEFT);
}
