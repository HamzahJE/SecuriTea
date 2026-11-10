#include "input.h"

#include <Arduino.h>
#include "board_config.h"

void Joystick::begin()
{
    pinMode(board::kJoyButtonPin, INPUT_PULLUP);
    lastButtonLevel_ = true;
    lastDirectionMs_ = 0;
}

JoyInput Joystick::read(unsigned long now)
{
    JoyInput in;
    int x = analogRead(board::kJoyXPin);
    int y = analogRead(board::kJoyYPin);
    bool button = digitalRead(board::kJoyButtonPin);

    if (now - lastDirectionMs_ > kJoyRepeatMs)
    {
        in.up = y < kJoyLowThreshold;
        in.down = y > kJoyHighThreshold;
        in.right = x > kJoyHighThreshold;
        in.left = x < kJoyLowThreshold;
        if (in.up || in.down || in.left || in.right)
        {
            lastDirectionMs_ = now;
        }
    }

    in.press = (button == LOW && lastButtonLevel_ == HIGH);
    lastButtonLevel_ = button;
    return in;
}
