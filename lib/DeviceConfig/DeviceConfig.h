#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "config.h"

class DeviceConfig {
public:
    void begin();           // load cached value from NVS
    uint8_t getHand() const { return _hand; }
    void setHand(uint8_t h); // 0=unknown, 1=left, 2=right (persists)
    const char* handString() const;

private:
    Preferences _prefs;
    uint8_t     _hand = cfg::HAND_UNKNOWN;
};