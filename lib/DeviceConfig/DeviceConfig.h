#pragma once
#include <Arduino.h>
#if defined(ESP32)
#include <Preferences.h>
#endif
#include "config.h"

class DeviceConfig {
public:
    void begin();
    uint8_t getHand() const { return _hand; }
    void setHand(uint8_t h);
    const char* handString() const;

    uint8_t getSensorMode() const { return _sensorMode; }
    void setSensorMode(uint8_t m);
    const char* sensorModeString() const;

private:
#if defined(ESP32)
    Preferences _prefs;
#endif
    uint8_t     _hand = cfg::HAND_UNKNOWN;
    uint8_t     _sensorMode = cfg::SENSOR_MODE_DEFAULT;
};
