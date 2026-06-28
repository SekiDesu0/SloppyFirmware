#include "WifiManager.h"
#include "config.h"
#include <WiFi.h>

namespace {
    WifiManager* g_self = nullptr;

    void wifiEventHandler(WiFiEvent_t event) {
        if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED && g_self) {
            g_self->markDisconnect();
        }
    }
}

bool WifiManager::hasCredentials() {
    _prefs.begin("wifi", true);
    bool ok = _prefs.isKey("ssid");
    _prefs.end();
    return ok;
}

bool WifiManager::setCredentials(const String& ssid, const String& pass) {
    if (ssid.length() == 0) return false;
    _prefs.begin("wifi", false);
    _prefs.putString("ssid", ssid);
    _prefs.putString("pass", pass);
    _prefs.end();
    _ssid = ssid;
    _pass = pass;
    return true;
}

void WifiManager::clearCredentials() {
    _prefs.begin("wifi", false);
    _prefs.remove("ssid");
    _prefs.remove("pass");
    _prefs.end();
    _ssid = "";
    _pass = "";
}

void WifiManager::getCredentials(String& ssid, String& pass) {
    _prefs.begin("wifi", true);
    ssid = _prefs.getString("ssid", "");
    pass = _prefs.getString("pass", "");
    _prefs.end();
    _ssid = ssid;
    _pass = pass;
}

void WifiManager::beginConnect() {
    if (_ssid.length() == 0) getCredentials(_ssid, _pass);
    if (g_self == nullptr) {
        g_self = this;
        WiFi.onEvent(wifiEventHandler);
    }
    WiFi.setSleep(WIFI_PS_NONE);
    WiFi.setHostname("sloppyhands");
    WiFi.begin(_ssid.c_str(), _pass.c_str());
    _lastAttempt = millis();
    _disconnectFlag = false;
}

bool WifiManager::isConnected() {
    return WiFi.status() == WL_CONNECTED;
}

bool WifiManager::update() {
    if (isConnected()) return true;
    // Retry if we've been stalled for a while
    if (millis() - _lastAttempt > cfg::WIFI_RETRY_INTERVAL_MS) {
        WiFi.disconnect(true, false);
        WiFi.begin(_ssid.c_str(), _pass.c_str());
        _lastAttempt = millis();
    }
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
int8_t   WifiManager::rssi()   { return WiFi.RSSI(); }
String   WifiManager::mac()    { return WiFi.macAddress(); }