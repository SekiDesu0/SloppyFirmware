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

private:
#if defined(ESP32)
    Preferences _prefs;
#endif
    uint8_t     _hand = cfg::HAND_UNKNOWN;
};
