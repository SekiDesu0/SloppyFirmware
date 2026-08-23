#include "DeviceConfig.h"
#if !defined(ESP32)
#include <EEPROM.h>
#endif

void DeviceConfig::begin() {
#if defined(ESP32)
    _prefs.begin("device", true);
    _hand = _prefs.getUChar("hand", cfg::HAND_UNKNOWN);
    _sensorMode = _prefs.getUChar("sensor", cfg::SENSOR_MODE_DEFAULT);
    _prefs.end();
#else
    EEPROM.begin(128);
    _hand = EEPROM.read(120);
    if (_hand > cfg::HAND_RIGHT) _hand = cfg::HAND_UNKNOWN;
    _sensorMode = EEPROM.read(121);
    if (_sensorMode > cfg::SENSOR_MODE_FDC2214) _sensorMode = cfg::SENSOR_MODE_DEFAULT;
    EEPROM.end();
#endif
}

void DeviceConfig::setHand(uint8_t h) {
    if (h > cfg::HAND_RIGHT) h = cfg::HAND_UNKNOWN;
    _hand = h;
#if defined(ESP32)
    _prefs.begin("device", false);
    _prefs.putUChar("hand", h);
    _prefs.end();
#else
    EEPROM.begin(128);
    EEPROM.write(120, h);
    EEPROM.commit();
    EEPROM.end();
#endif
}

const char* DeviceConfig::handString() const {
    switch (_hand) {
        case cfg::HAND_LEFT:  return "left";
        case cfg::HAND_RIGHT: return "right";
        default:              return "unknown";
    }
}

void DeviceConfig::setSensorMode(uint8_t m) {
    if (m > cfg::SENSOR_MODE_FDC2214) m = cfg::SENSOR_MODE_DEFAULT;
    _sensorMode = m;
#if defined(ESP32)
    _prefs.begin("device", false);
    _prefs.putUChar("sensor", m);
    _prefs.end();
#else
    EEPROM.begin(128);
    EEPROM.write(121, m);
    EEPROM.commit();
    EEPROM.end();
#endif
}

const char* DeviceConfig::sensorModeString() const {
    switch (_sensorMode) {
        case cfg::SENSOR_MODE_MPR121:  return "mpr121";
        case cfg::SENSOR_MODE_FDC2214: return "fdc2214";
        default:                       return "auto";
    }
}
