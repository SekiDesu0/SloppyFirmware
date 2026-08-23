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

    // Transport selection (wifi UDP vs ESP-NOW dongle)
    uint8_t getTransport() const { return _transport; }
    void setTransport(uint8_t t);
    const char* transportString() const;

    // Paired ESP-NOW dongle (MAC + Wi-Fi channel)
    bool hasPair() const { return _hasPair; }
    void getPairMac(uint8_t macOut[6]) const { memcpy(macOut, _pairMac, 6); }
    uint8_t getPairChannel() const { return _pairChannel; }
    void setPair(const uint8_t mac[6], uint8_t channel);
    void clearPair();

private:
#if defined(ESP32)
    Preferences _prefs;
#endif
    void _savePair();
    uint8_t     _hand = cfg::HAND_UNKNOWN;
    uint8_t     _sensorMode = cfg::SENSOR_MODE_DEFAULT;
    uint8_t     _transport  = cfg::TRANSPORT_DEFAULT;
    bool        _hasPair    = false;
    uint8_t     _pairMac[6] = {0};
    uint8_t     _pairChannel = cfg::ESPNOW_CHANNEL_DEFAULT;
};
