#pragma once
#include <Arduino.h>

enum class DeviceState : uint8_t {
    Provisioning = 0,
    Connecting   = 1,
    Discovering  = 2,
    Streaming    = 3
};

class StatusLED {
public:
    void begin(uint8_t pin);
    void setState(DeviceState s);
    void tick();  // call from loop(); non-blocking

private:
    uint8_t  _pin = 0;
    DeviceState _state = DeviceState::Provisioning;
    uint32_t _lastTick = 0;
    bool     _phase = false;

    void solid(uint8_t r, uint8_t g, uint8_t b);
    void pulse(uint8_t r, uint8_t g, uint8_t b);
};