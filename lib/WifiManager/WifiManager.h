#pragma once
#include <Arduino.h>
#if defined(ESP8266)
#include <ESP8266WiFi.h>
#include <EEPROM.h>
#else
#include <Preferences.h>
#endif

class WifiManager {
public:
    bool hasCredentials();
    bool setCredentials(const String& ssid, const String& pass);
    void clearCredentials();
    void getCredentials(String& ssid, String& pass);

    void beginConnect();
    bool isConnected();
    bool update();
    bool hadDisconnect();
    void markDisconnect();

    String statusLine();
    IPAddress ip();
    int8_t   rssi();
    String   mac();

private:
#if defined(ESP32)
    Preferences _prefs;
#else
    static constexpr uint8_t EE_SSID_OFF = 0;
    static constexpr uint8_t EE_PASS_OFF = 48;
    static constexpr uint8_t EE_MAGIC_OFF = 112;
    static constexpr uint8_t EE_MAGIC = 0xA5;
#endif
    String _ssid;
    String _pass;
    uint32_t _lastAttempt = 0;
    bool _disconnectFlag = false;

    void _onDisconnect();
};
