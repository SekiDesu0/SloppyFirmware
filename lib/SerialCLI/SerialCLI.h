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
    using SensorSetFn = void (*)(uint8_t mode);
    using TransportSetFn = void (*)(uint8_t transport);
    using PairClearFn = void (*)();
    using I2cScanFn   = void (*)();
    void begin(StatusFn statusFn, WifiSetFn wifiSetFn, WifiClearFn wifiClearFn,
               HandSetFn handSetFn, SensorSetFn sensorSetFn,
               TransportSetFn transportSetFn = nullptr,
               PairClearFn pairClearFn = nullptr,
               I2cScanFn i2cScanFn = nullptr);
    void update();

private:
    String _line;
    StatusFn      _statusFn   = nullptr;
    WifiSetFn     _wifiSetFn  = nullptr;
    WifiClearFn   _wifiClearFn = nullptr;
    HandSetFn     _handSetFn  = nullptr;
    SensorSetFn   _sensorSetFn = nullptr;
    TransportSetFn _transportSetFn = nullptr;
    PairClearFn    _pairClearFn    = nullptr;
    I2cScanFn      _i2cScanFn      = nullptr;
    bool _started = false;

    void _help();
    void _exec(const String& line);
};