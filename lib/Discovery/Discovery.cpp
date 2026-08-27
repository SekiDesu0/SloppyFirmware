#include "Discovery.h"
#include "config.h"
#if defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif

void Discovery::begin(uint16_t port) {
    if (_udpStarted && _localPort == port) return;  // idempotent
    if (_udpStarted) _udp.stop();
    _localPort = port;
    _udp.begin(port);
    _udpStarted = true;
}

bool Discovery::begin() {
    begin(cfg::UDP_PORT);
    return _udpStarted;
}

void Discovery::stop() {
    if (_udpStarted) {
        _udp.stop();
        _udpStarted = false;
    }
}

void Discovery::sendHello(uint8_t hand) {
    if (!_udpStarted) return;

    HelloPacket p;
    uint8_t mac[6];
    WiFi.macAddress(mac);
    PacketIO::buildHello(p, mac, hand);

    // Broadcast on the local subnet
    IPAddress bcast = WiFi.localIP();
    bcast[3] = 255;
    _udp.beginPacket(bcast, _localPort);
    _udp.write((uint8_t*)&p, sizeof(p));
    _udp.endPacket();
}

void Discovery::setServer(const IPAddress& ip, uint16_t dataPort, uint32_t keepaliveMs) {
    _serverIP    = ip;
    _dataPort    = dataPort ? dataPort : _localPort;
    _keepaliveMs = keepaliveMs ? keepaliveMs : 1000;
    _hasServer   = true;
}

bool Discovery::acceptWelcome(const WelcomePacket& w) {
    // pump() captured the sender IP in _serverIP; adopt it with the offered
    // data port / keepalive cadence.
    setServer(_serverIP, w.dataPort, w.keepaliveMs);
    return _hasServer;
}

void Discovery::clearServer() {
    _hasServer = false;
    _serverIP  = IPAddress(0, 0, 0, 0);
    _dataPort  = 0;
}

PacketType Discovery::pump(WelcomePacket& welcomeOut, KeepalivePacket& keepaliveOut) {
    if (!_udpStarted) return static_cast<PacketType>(0);

    while (_udp.parsePacket()) {
        uint8_t buf[256];
        int len = _udp.read(buf, sizeof(buf));
        if (len <= 0) continue;

        if (len >= (int)sizeof(Header)) {
            Header h;
            memcpy(&h, buf, sizeof(h));
            if (h.magic != cfg::MAGIC) continue;

            switch (h.type) {
                case static_cast<uint8_t>(PacketType::Welcome):
                    if (PacketIO::parseWelcome(buf, len, welcomeOut)) {
                        // Capture the actual sender IP so the caller has it
                        // available via serverIP() when accepting the welcome.
                        _serverIP = _udp.remoteIP();
                        return PacketType::Welcome;
                    }
                    break;
                case static_cast<uint8_t>(PacketType::Keepalive):
                    if (PacketIO::parseKeepalive(buf, len, keepaliveOut)) {
                        return PacketType::Keepalive;
                    }
                    break;
                default:
                    break;
            }
        }
    }
    return static_cast<PacketType>(0);
}

void Discovery::sendData(const DataPacket& p) {
    if (!_udpStarted || !_hasServer) return;
    _udp.beginPacket(_serverIP, _dataPort);
    _udp.write((uint8_t*)&p, sizeof(p));
    _udp.endPacket();
}

String Discovery::statusLine() const {
    if (!_hasServer) {
        return String("link=wifi-udp server=none");
    }
    char buf[96];
    snprintf(buf, sizeof(buf), "link=wifi-udp server=%s:%u keepaliveMs=%lu",
             _serverIP.toString().c_str(), _dataPort, (unsigned long)_keepaliveMs);
    return String(buf);
}