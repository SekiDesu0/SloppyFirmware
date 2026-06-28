#pragma once
#include <Arduino.h>
#include <Preferences.h>

class WifiManager {
public:
    // Returns true if credentials are present in NVS.
    bool hasCredentials();

    // Store creds and persist. Returns false if ssid empty.
    bool setCredentials(const String& ssid, const String& pass);
    void clearCredentials();

    // Load credentials into the provided buffers.
    void getCredentials(String& ssid, String& pass);

    // Kick off a connection attempt (async). Use isConnectBlocking=false to return immediately.
    void beginConnect();
    bool isConnected();

    // Polling helper for the FSM. Returns true once connected.
    bool update();

    // WiFi disconnect event flag for FSM. Set internally; FSM clears.
    bool hadDisconnect();

    // Called from the static WiFi event handler.
    void markDisconnect();

    // Diagnostics for `status` command.
    String statusLine();

    IPAddress ip();
    int8_t   rssi();
    String   mac();

private:
    Preferences _prefs;
    String _ssid;
    String _pass;
    uint32_t _lastAttempt = 0;
    bool _disconnectFlag = false;

    void _onDisconnect();
};