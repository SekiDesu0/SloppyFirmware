#include "StatusLED.h"
#include "config.h"

void StatusLED::begin(uint8_t pin) {
    _pin = pin;
    _pixels.setPin(pin);
    _pixels.begin();
    setState(DeviceState::Provisioning);
}

void StatusLED::setState(DeviceState s) {
    _state = s;
    tick();
}

void StatusLED::setSensorAbsent(bool absent) {
    _sensorAbsent = absent;
    _morseStep = 0;
    _morseLastMs = 0;
}

void StatusLED::tick() {
    if (_sensorAbsent) {
        morseSOS();
        return;
    }

    uint32_t now = millis();
    if (now - _lastTick < cfg::LED_TICK_INTERVAL_MS) return;
    _lastTick = now;

    switch (_state) {
        case DeviceState::Provisioning: pulse(0,  0,  10); break;
        case DeviceState::Connecting:   solid(10, 0,  0);  break;
        case DeviceState::Discovering:  pulse(10, 10, 0);   break;
        case DeviceState::Streaming:    solid(0,  10, 0);   break;
    }
}

void StatusLED::solid(uint8_t r, uint8_t g, uint8_t b) {
    _pixels.setPixelColor(0, _pixels.Color(r, g, b));
    _pixels.show();
}

void StatusLED::pulse(uint8_t r, uint8_t g, uint8_t b) {
    _phase = !_phase;
    if (_phase) {
        _pixels.setPixelColor(0, _pixels.Color(r, g, b));
    } else {
        _pixels.setPixelColor(0, _pixels.Color(r / 4, g / 4, b / 4));
    }
    _pixels.show();
}

void StatusLED::morseSOS() {
    struct Step { uint16_t ms; uint8_t r, g, b; };
    static const Step seq[] = {
        {200, 15, 0, 20},   // S dot   (purple)
        {200, 0,  0,  0},
        {200, 15, 0, 20},   // S dot
        {200, 0,  0,  0},
        {200, 15, 0, 20},   // S dot
        {600, 0,  0,  0},   // inter-letter gap
        {600, 15, 0, 20},   // O dash
        {200, 0,  0,  0},
        {600, 15, 0, 20},   // O dash
        {200, 0,  0,  0},
        {600, 15, 0, 20},   // O dash
        {600, 0,  0,  0},   // inter-letter gap
        {200, 15, 0, 20},   // S dot
        {200, 0,  0,  0},
        {200, 15, 0, 20},   // S dot
        {200, 0,  0,  0},
        {200, 15, 0, 20},   // S dot
        {1400, 0, 0, 0},    // pause before repeat
    };
    static constexpr uint8_t N = sizeof(seq) / sizeof(seq[0]);

    uint32_t now = millis();
    if (now - _morseLastMs < seq[_morseStep].ms) return;
    _morseLastMs = now;
    _pixels.setPixelColor(0, _pixels.Color(seq[_morseStep].r, seq[_morseStep].g, seq[_morseStep].b));
    _pixels.show();
    _morseStep = (_morseStep + 1) % N;
}
