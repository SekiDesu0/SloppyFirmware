#pragma once
#include <Arduino.h>
#include <Adafruit_NeoPixel.h>

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
    void setSensorAbsent(bool absent);
    void tick();

private:
    uint8_t  _pin = 0;
    DeviceState _state = DeviceState::Provisioning;
    uint32_t _lastTick = 0;
    bool     _phase = false;
    bool     _sensorAbsent = false;
    uint8_t  _morseStep = 0;
    uint32_t _morseLastMs = 0;
    Adafruit_NeoPixel _pixels = Adafruit_NeoPixel(1, 0, NEO_GRB + NEO_KHZ800);

    void solid(uint8_t r, uint8_t g, uint8_t b);
    void pulse(uint8_t r, uint8_t g, uint8_t b);
    void morseSOS();
};
