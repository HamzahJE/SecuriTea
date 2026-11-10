#include "ui.h"

#include "SSD1306Wire.h"
#include "ui_layout.h"

extern SSD1306Wire g_display;

namespace
{
const String kMainMenu[kMainMenuLen] = {"Universal Remote", "File Browser", "Learn New"};
const String kUnivMenu[kUnivMenuLen] = {"TVs", "ACs", "Projectors", "Audio"};

void drawFileBrowserScreen(const UiModel &m)
{
    String title = "Path: " + m.browserPath;
    if (m.browserCount == 0)
    {
        g_display.clear();
        drawHeader(title, m.browserFilterLabel);
        g_display.drawString(0, kUiLine1Y + 4, "(No items)");
        drawFooter("[>]Filter [<]Clr/Back");
        g_display.display();
        return;
    }

    drawMenu(title, m.browserRows, m.browserShown, m.menuIndex - m.browserWindowStart,
             "[>]Filter [BTN]Open [<]Clr/Back", m.browserFilterLabel);
}

void drawUniversalRemoteScreen(const UiModel &m)
{
    String title = m.targetDevice + " Remote";
    if (m.univCount <= 0 || m.univItems == nullptr)
    {
        g_display.clear();
        drawHeader(title, m.universalBadge);
        g_display.drawString(0, kUiLine1Y + 4, "No commands found");
        g_display.drawString(0, kUiLine2Y + 4, "File: " + m.univProfilePath);
        drawFooter("[<] Back");
        g_display.display();
        return;
    }

    drawMenu(title, m.univItems, m.univCount, m.menuIndex, "[<] Back | [BTN] Select", m.universalBadge, false);
    if (m.shared.univSending)
    {
        drawUniversalProgressPopup(m.shared.univProgressName, m.shared.univProgressCurrent,
                                   m.shared.univProgressTotal);
    }
    g_display.display();
}

void drawRemoteViewScreen(const UiModel &m)
{
    g_display.clear();
    drawHeader("<- Back   Blaster", m.sendModeBadge, m.modeSelected);
    g_display.drawString(0, kUiLine1Y, "File: " + m.fileName);
    // While a send is running, show the name going out (irTask's field). Otherwise show the
    // selected command (uiTask's field). The two never overwrite each other.
    bool sending = m.shared.transmitting && m.shared.sendingName.length() > 0;
    g_display.drawString(0, kUiLine2Y, sending ? "Tx: " + m.shared.sendingName : "Btn: " + m.shared.btnName);
    g_display.drawString(0, kUiLine3Y, "Cmd: " + String(m.cmdIndex) + "/" + String(m.cmdCount));

    if (m.deletePending)
    {
        drawFooter("[ DELETING... ]");
    }
    else if (m.shared.transmitting)
    {
        drawFooter("[ TRANSMITTING... ]");
    }
    else if (m.deleteMode)
    {
        drawFooter(m.deleteArmed ? "[BTN] Confirm delete" : "[v^]Cmd [>]Mode [BTN]Delete");
    }
    else
    {
        drawFooter("[v^]Cmd [>]Mode [BTN]Send");
    }
    g_display.display();
}

void drawLearnScreen(const UiModel &m)
{
    const UiSnapshot &snap = m.shared;
    g_display.clear();
    drawHeader("Learn New Remote");
    g_display.drawString(0, kUiLine1Y, snap.status);

    switch (snap.learnPhase)
    {
        case LearnPhase::Listening:
            g_display.drawString(0, kUiLine2Y, "Waiting for signal...");
            drawFooter("[<] Back");
            break;
        case LearnPhase::Captured:
        {
            String proto = snap.learnProtocol.length() ? snap.learnProtocol : "UNKNOWN";
            g_display.drawString(0, kUiLine2Y, trimToWidth("Proto: " + proto, 21));
            if (snap.learnDetail.length())
            {
                g_display.drawString(0, kUiLine3Y, trimToWidth(snap.learnDetail, 21));
            }
            drawFooter("[BTN] Save | [<] Back");
            break;
        }
        case LearnPhase::Saved:
            g_display.drawString(0, kUiLine2Y, "Saved to /captures/");
            if (snap.learnProtocol.length())
            {
                g_display.drawString(0, kUiLine3Y, trimToWidth("Proto: " + snap.learnProtocol, 21));
            }
            drawFooter("[BTN] Capture Again");
            break;
        case LearnPhase::Error:
            g_display.drawString(0, kUiLine2Y, "Capture error");
            drawFooter("[BTN] Retry | [<] Back");
            break;
        default:
            g_display.drawString(0, kUiLine2Y, "Preparing listener...");
            drawFooter("[<] Back");
            break;
    }
    g_display.display();
}
} // namespace

void drawScreen(const UiModel &m)
{
    switch (m.state)
    {
        case AppState::MainMenu:
            drawMenu("SecuriTea OS", kMainMenu, kMainMenuLen, m.menuIndex);
            break;
        case AppState::UniversalMenu:
            drawMenu("Universal Remote", kUnivMenu, kUnivMenuLen, m.menuIndex,
                     "[v^]Scroll [>]Mode [BTN]Toggle/Pick [<]Back", m.universalBadge, true, m.headerModeSelected);
            break;
        case AppState::FileBrowser:
            drawFileBrowserScreen(m);
            break;
        case AppState::UniversalRemote:
            drawUniversalRemoteScreen(m);
            break;
        case AppState::RemoteView:
            drawRemoteViewScreen(m);
            break;
        case AppState::Learn:
            drawLearnScreen(m);
            break;
    }
}
