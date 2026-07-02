#include "WifiManager.h"
#include "config.h"

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif

namespace {
    WifiManager* g_self = nullptr;
#if defined(ESP32)
    void wifiEventHandler(WiFiEvent_t event) {
        if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED && g_self) {
            g_self->markDisconnect();
        }
    }
#endif
}

bool WifiManager::hasCredentials() {
#if defined(ESP32)
    _prefs.begin("wifi", true);
    bool ok = _prefs.isKey("ssid");
    _prefs.end();
    return ok;
#else
    EEPROM.begin(128);
    bool ok = EEPROM.read(EE_MAGIC_OFF) == EE_MAGIC;
    EEPROM.end();
    return ok;
#endif
}

bool WifiManager::setCredentials(const String& ssid, const String& pass) {
    if (ssid.length() == 0) return false;
#if defined(ESP32)
    _prefs.begin("wifi", false);
    _prefs.putString("ssid", ssid);
    _prefs.putString("pass", pass);
    _prefs.end();
#else
    EEPROM.begin(128);
    for (unsigned i = 0; i < 48; i++) {
        EEPROM.write(EE_SSID_OFF + i, i < ssid.length() ? ssid[i] : 0);
    }
    for (unsigned i = 0; i < 64; i++) {
        EEPROM.write(EE_PASS_OFF + i, i < pass.length() ? pass[i] : 0);
    }
    EEPROM.write(EE_MAGIC_OFF, EE_MAGIC);
    EEPROM.commit();
    EEPROM.end();
#endif
    _ssid = ssid;
    _pass = pass;
    return true;
}

void WifiManager::clearCredentials() {
#if defined(ESP32)
    _prefs.begin("wifi", false);
    _prefs.remove("ssid");
    _prefs.remove("pass");
    _prefs.end();
#else
    EEPROM.begin(128);
    for (unsigned i = 0; i < 48; i++) EEPROM.write(EE_SSID_OFF + i, 0);
    for (unsigned i = 0; i < 64; i++) EEPROM.write(EE_PASS_OFF + i, 0);
    EEPROM.write(EE_MAGIC_OFF, 0);
    EEPROM.commit();
    EEPROM.end();
#endif
    _ssid = "";
    _pass = "";
}

void WifiManager::getCredentials(String& ssid, String& pass) {
#if defined(ESP32)
    _prefs.begin("wifi", true);
    ssid = _prefs.getString("ssid", "");
    pass = _prefs.getString("pass", "");
    _prefs.end();
#else
    EEPROM.begin(128);
    char ssidBuf[49];
    for (unsigned i = 0; i < 48; i++) {
        char c = EEPROM.read(EE_SSID_OFF + i);
        if (!c) { ssidBuf[i] = 0; break; }
        ssidBuf[i] = c;
    }
    ssidBuf[48] = 0;
    ssid = String(ssidBuf);
    char passBuf[65];
    for (unsigned i = 0; i < 64; i++) {
        char c = EEPROM.read(EE_PASS_OFF + i);
        if (!c) { passBuf[i] = 0; break; }
        passBuf[i] = c;
    }
    passBuf[64] = 0;
    pass = String(passBuf);
    EEPROM.end();
#endif
    _ssid = ssid;
    _pass = pass;
}

void WifiManager::beginConnect() {
    if (_ssid.length() == 0) getCredentials(_ssid, _pass);
    if (g_self == nullptr) {
        g_self = this;
#if defined(ESP32)
        WiFi.onEvent(wifiEventHandler);
#else
        WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected& e) {
            if (g_self) g_self->markDisconnect();
        });
#endif
    }
#if defined(ESP32)
    WiFi.setSleep(WIFI_PS_MIN_MODEM);
    WiFi.setHostname("sloppyhands");
#else
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    WiFi.hostname("sloppyhands");
#endif
    WiFi.begin(_ssid.c_str(), _pass.c_str());
    _lastAttempt = millis();
    _disconnectFlag = false;
}

bool WifiManager::isConnected() {
    return WiFi.status() == WL_CONNECTED;
}

bool WifiManager::update() {
    if (isConnected()) return true;
#if defined(ESP8266)
    // ESP8266 needs more time to connect; don't kill the radio mid-attempt.
    // Just re-call begin() if the timeout has truly elapsed.
    if (millis() - _lastAttempt > cfg::WIFI_CONNECT_TIMEOUT_MS) {
        WiFi.begin(_ssid.c_str(), _pass.c_str());
        _lastAttempt = millis();
    }
#else
    if (millis() - _lastAttempt > cfg::WIFI_RETRY_INTERVAL_MS) {
        WiFi.disconnect(true, false);
        WiFi.begin(_ssid.c_str(), _pass.c_str());
        _lastAttempt = millis();
    }
#endif
    return false;
}

bool WifiManager::hadDisconnect() {
    bool f = _disconnectFlag;
    _disconnectFlag = false;
    return f;
}

void WifiManager::markDisconnect() {
    _disconnectFlag = true;
}

String WifiManager::statusLine() {
    String s = "wifi=";
    s += (isConnected() ? "connected(" + WiFi.localIP().toString() + ")" : String("disconnected"));
    s += " ssid=" + _ssid;
    s += " rssi=" + String(rssi());
    s += " mac=" + mac();
    return s;
}

IPAddress WifiManager::ip()    { return WiFi.localIP(); }
int8_t   WifiManager::rssi()   { return (int8_t)WiFi.RSSI(); }
String   WifiManager::mac()    { return WiFi.macAddress(); }
