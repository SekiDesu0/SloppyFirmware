#include "EspNowTransport.h"
#include "config.h"
#if defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif

void EspNowTransport::configure(uint8_t channel, const uint8_t peerMac[6]) {
    if (peerMac) {
        memcpy(_pairMac, peerMac, 6);
        _paired = true;
    }
    _link.begin(channel);
}

void EspNowTransport::clearPair() {
    _paired = false;
    memset(_pairMac, 0, 6);
}

bool EspNowTransport::begin() {
    return _link.begin(_link.channel());
}

void EspNowTransport::sendHello(uint8_t hand) {
    HelloPacket p;
    uint8_t mac[6];
    WiFi.macAddress(mac);
    PacketIO::buildHello(p, mac, hand);

    if (_paired && !_hasServer) {
        // Fast path: unicast HELLO straight to the remembered dongle.
        // Peer must be registered before sending (ESP32 requirement).
        if (_link.addPeer(_pairMac)) {
            _link.send(_pairMac, (uint8_t*)&p, sizeof(p));
        }
    } else if (!_hasServer) {
        // Slow path: broadcast on the current channel (caller hops via
        // helloIntervalMs() cadence).
        _link.send(nullptr, (uint8_t*)&p, sizeof(p));
    }
}

PacketType EspNowTransport::pump(WelcomePacket& welcomeOut, KeepalivePacket& keepaliveOut) {
    RxFrame f;
    while (_link.poll(f)) {
        if (f.len < (int)sizeof(Header)) continue;
        Header h;
        memcpy(&h, f.data, sizeof(h));
        if (h.magic != cfg::MAGIC) continue;

        switch (h.type) {
            case static_cast<uint8_t>(PacketType::Welcome):
                if (PacketIO::parseWelcome(f.data, f.len, welcomeOut)) {
                    memcpy(_rxMac, f.mac, 6);
                    _lastRssi = f.rssi;
                    return PacketType::Welcome;
                }
                break;
            case static_cast<uint8_t>(PacketType::Keepalive):
                if (PacketIO::parseKeepalive(f.data, f.len, keepaliveOut)) {
                    _lastRssi = f.rssi;
                    return PacketType::Keepalive;
                }
                break;
            default:
                break;      // HELLO/DATA/TUNNEL not for gloves
        }
    }
    return static_cast<PacketType>(0);
}

bool EspNowTransport::acceptWelcome(const WelcomePacket& w) {
    memcpy(_sessionMac, _rxMac, 6);
    _link.addPeer(_sessionMac);
    _keepaliveMs = w.keepaliveMs ? w.keepaliveMs : 1000;
    _hasServer   = true;
    return true;
}

void EspNowTransport::clearServer() {
    _hasServer = false;
    memset(_sessionMac, 0, 6);
}

void EspNowTransport::sendData(const DataPacket& p) {
    if (!_hasServer) return;
    _link.send(_sessionMac, (const uint8_t*)&p, sizeof(p));
}

String EspNowTransport::statusLine() const {
    char macStr[18] = "none";
    const uint8_t* m = _hasServer ? _sessionMac : (_paired ? _pairMac : nullptr);
    if (m) snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                    m[0], m[1], m[2], m[3], m[4], m[5]);
    char buf[96];
    snprintf(buf, sizeof(buf), "link=espnow chan=%u dongle=%s rssi=%d drops=%lu",
             _link.channel(), macStr, (int)_lastRssi, (unsigned long)_link.drops());
    return String(buf);
}

uint32_t EspNowTransport::helloIntervalMs() const {
    // Hop cadence in discovery; plain keepalive-paced hello once paired.
    return (!_hasServer && !_paired) ? cfg::ESPNOW_HOP_INTERVAL_MS : cfg::HELLO_INTERVAL_MS;
}
