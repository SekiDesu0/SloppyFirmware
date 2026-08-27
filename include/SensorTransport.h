#pragma once
#include <Arduino.h>
#include "PacketIO.h"

// Transport abstraction so the tracker state machine can run unchanged over
// UDP/WiFi (Discovery) or ESP-NOW to the HID dongle (EspNowTransport).
class SensorTransport {
public:
    virtual ~SensorTransport() {}

    // Bring the link up. Returns false on hard failure.
    virtual bool begin() = 0;

    // Broadcast HELLO beacon (DISCOVERING).
    virtual void sendHello(uint8_t hand) = 0;

    // Cadence at which the caller should call sendHello() while discovering.
    virtual uint32_t helloIntervalMs() const = 0;

    // Pump inbound packets. Call from loop() in DISCOVERING and STREAMING.
    virtual PacketType pump(WelcomePacket& welcomeOut, KeepalivePacket& keepaliveOut) = 0;

    // Latch a received WELCOME (remember server/peer). Returns false if the
    // welcome is unusable.
    virtual bool acceptWelcome(const WelcomePacket& w) = 0;

    virtual void clearServer() = 0;
    virtual bool hasServer() const = 0;

    // Stream one sensor frame.
    virtual void sendData(const DataPacket& p) = 0;

    // One-line diagnostics for `status`.
    virtual String statusLine() const = 0;
};
