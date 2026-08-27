#pragma once
#include <Arduino.h>
#include <WiFiUdp.h>
#include "PacketIO.h"
#include "SensorTransport.h"

class Discovery : public SensorTransport {
public:
    // SensorTransport
    bool begin() override;                     // bind UDP_PORT
    void sendHello(uint8_t hand) override;
    uint32_t helloIntervalMs() const override { return cfg::HELLO_INTERVAL_MS; }
    PacketType pump(WelcomePacket& welcomeOut, KeepalivePacket& keepaliveOut) override;
    bool acceptWelcome(const WelcomePacket& w) override;
    void clearServer() override;
    bool hasServer() const override { return _hasServer; }
    void sendData(const DataPacket& p) override;
    String statusLine() const override;

    // UDP specifics
    void stop();
    void begin(uint16_t port);                 // legacy explicit-port form
    void setServer(const IPAddress& ip, uint16_t dataPort, uint32_t keepaliveMs);

    IPAddress serverIP() const { return _serverIP; }
    uint16_t  dataPort() const { return _dataPort; }
    uint32_t  keepaliveMs() const { return _keepaliveMs; }

private:
    WiFiUDP   _udp;
    uint16_t  _localPort  = 0;
    IPAddress _serverIP;
    uint16_t  _dataPort   = 0;
    uint32_t  _keepaliveMs = 1000;
    bool      _hasServer  = false;
    bool      _udpStarted = false;
};
