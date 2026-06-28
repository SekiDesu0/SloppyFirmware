#include "StatusLED.h"
#include "config.h"

void StatusLED::begin(uint8_t pin) {
    _pin = pin;
    setState(DeviceState::Provisioning);
}

void StatusLED::setState(DeviceState s) {
    _state = s;
    // immediate refresh so color change is felt right away
    tick();
}

void StatusLED::tick() {
    uint32_t now = millis();
    if (now - _lastTick < cfg::LED_TICK_INTERVAL_MS) return;
    _lastTick = now;

    switch (_state) {
        case DeviceState::Provisioning: pulse(0,  0,  10); break;  // blue pulse
        case DeviceState::Connecting:   solid(10, 0,  0);  break;  // solid red
        case DeviceState::Discovering:  pulse(10, 10, 0);   break;  // yellow pulse
        case DeviceState::Streaming:    solid(0,  10, 0);   break;  // solid green
    }
}

void StatusLED::solid(uint8_t r, uint8_t g, uint8_t b) {
    neopixelWrite(_pin, r, g, b);
}

void StatusLED::pulse(uint8_t r, uint8_t g, uint8_t b) {
    // toggle between color and dim each tick window -> breathe effect
    _phase = !_phase;
    if (_phase) neopixelWrite(_pin, r, g, b);
    else        neopixelWrite(_pin, r / 4, g / 4, b / 4);
}