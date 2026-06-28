#include "DeviceConfig.h"

void DeviceConfig::begin() {
    _prefs.begin("device", true);
    _hand = _prefs.getUChar("hand", cfg::HAND_UNKNOWN);
    _prefs.end();
}

void DeviceConfig::setHand(uint8_t h) {
    if (h > cfg::HAND_RIGHT) h = cfg::HAND_UNKNOWN;
    _hand = h;
    _prefs.begin("device", false);
    _prefs.putUChar("hand", h);
    _prefs.end();
}

const char* DeviceConfig::handString() const {
    switch (_hand) {
        case cfg::HAND_LEFT:  return "left";
        case cfg::HAND_RIGHT: return "right";
        default:              return "unknown";
    }
}