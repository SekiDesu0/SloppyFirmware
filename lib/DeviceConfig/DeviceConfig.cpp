#include "DeviceConfig.h"
#if !defined(ESP32)
#include <EEPROM.h>
#endif

// EEPROM map (ESP8266):
//   0   ssid (48)  48 pass (48)  112 magic  [WifiManager]
//   120 hand         121 sensorMode
//   122 transport    123 pairChannel
//   124..129 pair MAC
//   130 hasPair flag
#define EE_TRANSPORT_OFF   122
#define EE_PAIRCH_OFF      123
#define EE_PAIRMAC_OFF     124
#define EE_HASPAIR_OFF     130

void DeviceConfig::begin() {
#if defined(ESP32)
    _prefs.begin("device", true);
    _hand        = _prefs.getUChar("hand", cfg::HAND_UNKNOWN);
    _sensorMode  = _prefs.getUChar("sensor", cfg::SENSOR_MODE_DEFAULT);
    _transport   = _prefs.getUChar("transport", cfg::TRANSPORT_DEFAULT);
    _hasPair     = _prefs.getBool("hasPair", false);
    _pairChannel = _prefs.getUChar("pairCh", cfg::ESPNOW_CHANNEL_DEFAULT);
    if (_hasPair) {
        uint8_t mac[6] = {0};
        if (_prefs.getBytes("pairMac", mac, 6) == 6) memcpy(_pairMac, mac, 6);
        else _hasPair = false;
    }
    _prefs.end();
#else
    EEPROM.begin(144);
    _hand = EEPROM.read(120);
    if (_hand > cfg::HAND_RIGHT) _hand = cfg::HAND_UNKNOWN;
    _sensorMode = EEPROM.read(121);
    if (_sensorMode > cfg::SENSOR_MODE_FDC2214) _sensorMode = cfg::SENSOR_MODE_DEFAULT;
    _transport = EEPROM.read(EE_TRANSPORT_OFF);
    if (_transport > cfg::TRANSPORT_ESPNOW) _transport = cfg::TRANSPORT_DEFAULT;
    _pairChannel = EEPROM.read(EE_PAIRCH_OFF);
    if (_pairChannel < cfg::ESPNOW_CHANNEL_MIN || _pairChannel > cfg::ESPNOW_CHANNEL_MAX)
        _pairChannel = cfg::ESPNOW_CHANNEL_DEFAULT;
    _hasPair = EEPROM.read(EE_HASPAIR_OFF) == 0xA5;
    if (_hasPair) {
        for (uint8_t i = 0; i < 6; i++) _pairMac[i] = EEPROM.read(EE_PAIRMAC_OFF + i);
        bool allZero = true;
        for (uint8_t i = 0; i < 6; i++) if (_pairMac[i]) allZero = false;
        if (allZero) _hasPair = false;
    }
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
    EEPROM.begin(144);
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
    EEPROM.begin(144);
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

void DeviceConfig::setTransport(uint8_t t) {
    if (t > cfg::TRANSPORT_ESPNOW) t = cfg::TRANSPORT_DEFAULT;
    _transport = t;
#if defined(ESP32)
    _prefs.begin("device", false);
    _prefs.putUChar("transport", t);
    _prefs.end();
#else
    EEPROM.begin(144);
    EEPROM.write(EE_TRANSPORT_OFF, t);
    EEPROM.commit();
    EEPROM.end();
#endif
}

const char* DeviceConfig::transportString() const {
    switch (_transport) {
        case cfg::TRANSPORT_ESPNOW: return "espnow";
        default:                    return "wifi";
    }
}

void DeviceConfig::setPair(const uint8_t mac[6], uint8_t channel) {
    memcpy(_pairMac, mac, 6);
    if (channel >= cfg::ESPNOW_CHANNEL_MIN && channel <= cfg::ESPNOW_CHANNEL_MAX)
        _pairChannel = channel;
    _hasPair = true;
    _savePair();
}

void DeviceConfig::clearPair() {
    _hasPair = false;
    memset(_pairMac, 0, 6);
    _savePair();
}

void DeviceConfig::_savePair() {
#if defined(ESP32)
    _prefs.begin("device", false);
    _prefs.putBool("hasPair", _hasPair);
    _prefs.putBytes("pairMac", _pairMac, 6);
    _prefs.putUChar("pairCh", _pairChannel);
    _prefs.end();
#else
    EEPROM.begin(144);
    for (uint8_t i = 0; i < 6; i++) EEPROM.write(EE_PAIRMAC_OFF + i, _pairMac[i]);
    EEPROM.write(EE_PAIRCH_OFF, _pairChannel);
    EEPROM.write(EE_HASPAIR_OFF, _hasPair ? 0xA5 : 0x00);
    EEPROM.commit();
    EEPROM.end();
#endif
}
