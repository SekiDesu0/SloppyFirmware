#pragma once
#include <Arduino.h>
#include <WiFiUdp.h>
#include "PacketIO.h"

class Discovery {
public:
    void begin(uint16_t port);
    void stop();

    // Broadcast HELLO beacon (call every HELLO_INTERVAL_MS in DISCOVERING).
    void sendHello(uint8_t hand);

    // Server has accepted device; remember its IP / keepalive interval.
    void setServer(const IPAddress& ip, uint16_t dataPort, uint32_t keepaliveMs);
    void clearServer();

    // Pump UDP rx. Call from loop() in DISCOVERING and STREAMING.
    // Returns the received packet type (or 0 if nothing parsed).
    PacketType pump(WelcomePacket& welcomeOut, KeepalivePacket& keepaliveOut);

    // Send DATA packet to the server.
    void sendData(const DataPacket& p);

    IPAddress serverIP() const { return _serverIP; }
    uint16_t  dataPort() const { return _dataPort; }
    uint32_t  keepaliveMs() const { return _keepaliveMs; }
    bool      hasServer() const { return _hasServer; }

private:
    WiFiUDP   _udp;
    uint16_t  _localPort  = 0;
    IPAddress _serverIP;
    uint16_t  _dataPort   = 0;
    uint32_t  _keepaliveMs = 1000;
    bool      _hasServer  = false;
    bool      _udpStarted = false;
};