#pragma once
#include <Arduino.h>

class SerialCLI {
public:
    // Call from loop(). Drives a tiny command line over the USB CDC serial.
    // Callbacks let it poke into WifiManager / Sensor without circular deps.
    using StatusFn = void (*)();
    using WifiSetFn = bool (*)(const String& ssid, const String& pass);
    using WifiClearFn = void (*)();
    using HandSetFn  = void (*)(uint8_t hand);
    void begin(StatusFn statusFn, WifiSetFn wifiSetFn, WifiClearFn wifiClearFn, HandSetFn handSetFn);
    void update();

private:
    String _line;
    StatusFn      _statusFn   = nullptr;
    WifiSetFn     _wifiSetFn  = nullptr;
    WifiClearFn   _wifiClearFn = nullptr;
    HandSetFn     _handSetFn  = nullptr;
    bool _started = false;

    void _help();
    void _exec(const String& line);
};