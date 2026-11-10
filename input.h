#pragma once
// Input module: the joystick (two analog axes and a push button).
//
// The class owns the two pieces of state the old free function kept in globals: the last
// button level (for edge detection) and the time of the last direction event (for the repeat
// rate). One instance lives in uiTask; nothing else reads the joystick.

#include "app_state.h"

class Joystick
{
public:
    // Configures the pins. Call once, from the task that will call read().
    void begin();

    // Samples the stick once. A direction is reported at most every kJoyRepeatMs while the
    // stick is held; a press is reported once per push, on the press edge.
    JoyInput read(unsigned long now);

private:
    bool lastButtonLevel_ = true;    // pulled up: true means released
    unsigned long lastDirectionMs_ = 0;
};
