#pragma once
// The App interface and the launcher.
//
// An app owns its own screens and state. uiTask drives the current app: it hands it joystick
// events and results from irTask, lets it do per-frame housekeeping, asks it whether the
// display may sleep, and asks it for a UiModel to draw. Apps never touch the display, the
// joystick or the SD card directly; they go through ui, input and storage.

#include "app_state.h"

class App
{
public:
    virtual ~App()
    {
    }
    virtual const char *name() const = 0;

    // A joystick event, after the display-wake rule has been applied.
    virtual void onInput(const JoyInput &in, unsigned long now) = 0;

    // A result from irTask.
    virtual void onEvent(const IrEvent &evt) = 0;

    // Once per frame: timeouts, state transitions.
    virtual void tick(unsigned long now) = 0;

    // True while the display must not sleep (learning, sending).
    virtual bool keepsAwake(const UiSnapshot &snap) const = 0;

    // Something changed since the last draw.
    virtual bool isDirty() const = 0;
    virtual void markDirty() = 0;
    virtual void clearDirty() = 0;

    // Fills everything the screens need for this frame.
    virtual void buildModel(UiModel &m, const UiSnapshot &snap) = 0;
};

// Holds the installed apps and which one is running. With a single app it hands that app
// straight to uiTask; a menu of apps comes when there is more than one.
constexpr int kMaxApps = 4;

class Launcher
{
public:
    void add(App &app);
    App &current();

private:
    App *apps_[kMaxApps] = {};
    int count_ = 0;
    int currentIndex_ = 0;
};
