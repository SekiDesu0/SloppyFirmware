#pragma once
#include <Arduino.h>
#include "SensorTransport.h"
#include "EspNowLink.h"

// SensorTransport over ESP-NOW, talking to an ESP32-S3 HID dongle.
//
// DISCOVERING: hops channels broadcasting HELLO until a WELCOME arrives
// (or unicasts HELLO straight to a previously-paired dongle). STREAMING:
// DATA unicasts + KEEPALIVEs, same wire packets as the UDP path.
class EspNowTransport : public SensorTransport {
public:
    // Optional pre-pairing from persisted config. Call before begin().
    void configure(uint8_t channel, const uint8_t peerMac[6]);
    void clearPair();

    bool begin() override;
    void sendHello(uint8_t hand) override;
    PacketType pump(WelcomePacket& welcomeOut, KeepalivePacket& keepaliveOut) override;
    bool acceptWelcome(const WelcomePacket& w) override;
    void clearServer() override;
    bool hasServer() const override { return _hasServer; }
    void sendData(const DataPacket& p) override;
    String statusLine() const override;

    uint32_t helloIntervalMs() const;           // hop dwell while discovering
    const uint8_t* peerMac() const { return _paired ? _pairMac : nullptr; }
    uint8_t channel() const { return _link.channel(); }
    bool isPaired() const { return _paired; }

private:
    EspNowLink _link;
    uint8_t  _pairMac[6]   = {0};
    bool     _paired       = false;             // remembered dongle (config)
    bool     _hasServer    = false;             // live session
    uint8_t  _sessionMac[6] = {0};              // dongle for current session
    uint32_t _keepaliveMs  = 1000;
    int8_t   _lastRssi     = 0;
    uint8_t  _rxMac[6]     = {0};               // sender of last parsed packet
};
