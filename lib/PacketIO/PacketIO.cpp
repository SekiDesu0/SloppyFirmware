#include "PacketIO.h"
#if defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif

void PacketIO::initHeader(Header& h, PacketType t) {
    h.magic      = cfg::MAGIC;
    h.type       = static_cast<uint8_t>(t);
    h.fwVersion  = cfg::FW_VERSION;
    h.reserved   = 0;
}

void PacketIO::buildHello(HelloPacket& p, const uint8_t mac[6], uint8_t hand) {
    initHeader(p.h, PacketType::Hello);
    memcpy(p.mac, mac, 6);
    p.deviceType   = cfg::DEVICE_TYPE;
    p.channelCount = cfg::CHANNEL_COUNT;
    p.hand         = hand;
    p.reserved     = 0;
}

void PacketIO::buildData(DataPacket& p, uint32_t packetId,
                         const uint16_t filtered[12], uint16_t touch,
                         const uint32_t fdcRaw[4], uint8_t sensorType,
                         uint16_t i2cMs, uint16_t loopMs, int8_t rssi,
                         uint16_t battMv, uint8_t battPct) {
    initHeader(p.h, PacketType::Data);
    p.packetId        = packetId;
    p.uptimeMs        = millis();
    memcpy(p.filtered, filtered, sizeof(p.filtered));
    p.touchStatus     = touch;
    memcpy(p.fdcRaw, fdcRaw, sizeof(p.fdcRaw));
    p.sensorType      = sensorType;
    p.reserved2       = 0;
    p.i2cReadTimeMs   = i2cMs;
    p.totalLoopTimeMs = loopMs;
    p.wifiRssi        = rssi;
    p.reserved3       = 0;
    p.battMv          = battMv;
    p.battPercent     = battPct;
}

bool PacketIO::isHeaderValid(const Header& h, PacketType expected) {
    return h.magic == cfg::MAGIC && h.type == static_cast<uint8_t>(expected);
}

void PacketIO::buildTunnel(TunnelPacket& t, const uint8_t mac[6], int8_t rssi, const DataPacket& d) {
    initHeader(t.h, PacketType::Tunnel);
    memcpy(t.mac, mac, 6);
    t.rssi     = rssi;
    t.reserved = 0;
    t.data     = d;
}

bool PacketIO::parseWelcome(const uint8_t* buf, size_t len, WelcomePacket& out) {
    if (len < sizeof(WelcomePacket)) return false;
    memcpy(&out, buf, sizeof(WelcomePacket));
    return isHeaderValid(out.h, PacketType::Welcome);
}

bool PacketIO::parseKeepalive(const uint8_t* buf, size_t len, KeepalivePacket& out) {
    if (len < sizeof(KeepalivePacket)) return false;
    memcpy(&out, buf, sizeof(KeepalivePacket));
    return isHeaderValid(out.h, PacketType::Keepalive);
}

bool PacketIO::parseTunnel(const uint8_t* buf, size_t len, TunnelPacket& out) {
    if (len < sizeof(TunnelPacket)) return false;
    memcpy(&out, buf, sizeof(TunnelPacket));
    return isHeaderValid(out.h, PacketType::Tunnel);
}