#include "EspNowLink.h"
#if defined(ESP8266)
extern "C" {
#include "espnow.h"
#include "user_interface.h"
}
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#endif

// IDF 5.3+ (arduino-esp32 3.1+) moved MAC+len into esp_now_recv_info_t and
// passes the payload separately. Older stacks use (mac, data, len).
#if defined(ESP32)
#if ESP_IDF_VERSION_MAJOR > 5 || \
    (ESP_IDF_VERSION_MAJOR == 5 && ESP_IDF_VERSION_MINOR >= 3)
#define ESPNOW_RX_INFO_SIGNATURE 1
#endif
#endif

// --- RX callbacks (run in WiFi stack context; keep them tiny) ---------------
#if defined(ESP8266)
static void _rxCb(uint8_t* mac, uint8_t* data, uint8_t len) {
    if (EspNowLink::_self) EspNowLink::_self->pushRx(mac, 0, data, len);
}
#else
#ifdef ESPNOW_RX_INFO_SIGNATURE
static void _rxCb(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if (EspNowLink::_self) {
        int8_t rssi = 0;
        if (info->rx_ctrl) rssi = info->rx_ctrl->rssi;
        EspNowLink::_self->pushRx(info->src_addr, rssi, data, len);
    }
}
#else
static void _rxCb(const uint8_t* mac, const uint8_t* data, int len) {
    if (EspNowLink::_self) EspNowLink::_self->pushRx(mac, 0, data, len);
}
#endif
#endif

EspNowLink* EspNowLink::_self = nullptr;
constexpr uint8_t EspNowLink::BROADCAST_MAC[6];

#if defined(ESP8266)
// The ESP8266 SDK predates const-correctness.
static inline u8* sdkMac(const uint8_t* m) { return const_cast<uint8_t*>(m); }
#endif

bool EspNowLink::begin(uint8_t channel) {
    if (_self != this) {
        _self = this;
    }
    _channel = channel;

#if defined(ESP8266)
    WiFi.mode(WIFI_STA);
    wifi_set_sleep_type(NONE_SLEEP_T);          // modem sleep drops frames
    wifi_set_channel(_channel);
    if (!_started) {
        if (esp_now_init() != 0) return false;
        esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
        esp_now_register_recv_cb(_rxCb);
        _started = true;
    }
    _readdPeers();      // broadcast + unicast peers on the current channel
#else
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    // Channel must be set with promiscuous mode per ESP-NOW docs
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(_channel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);

    if (!_started) {
        if (esp_now_init() != ESP_OK) return false;
        esp_now_register_recv_cb(_rxCb);    // signature picked by #defines above
        _started = true;
    }
    _readdPeers();      // broadcast + unicast peers on the new channel
#endif
    return true;
}

bool EspNowLink::setChannel(uint8_t channel) {
    if (channel == _channel && _started) return true;
    return begin(channel);
}

void EspNowLink::_readdPeers() {
    // Broadcast peer first: it must exist on the current channel too.
#if defined(ESP8266)
    esp_now_del_peer(sdkMac(BROADCAST_MAC));
    esp_now_add_peer(sdkMac(BROADCAST_MAC), ESP_NOW_ROLE_COMBO, _channel, NULL, 0);
    for (uint8_t i = 0; i < _peerCount; i++) {
        esp_now_del_peer(sdkMac(_peers[i]));
        esp_now_add_peer(sdkMac(_peers[i]), ESP_NOW_ROLE_COMBO, _channel, NULL, 0);
    }
#else
    esp_now_del_peer(BROADCAST_MAC);
    esp_now_peer_info_t b{};
    memcpy(b.peer_addr, BROADCAST_MAC, 6);
    b.channel = _channel;
    b.ifidx   = WIFI_IF_STA;
    b.encrypt = false;
    esp_now_add_peer(&b);
    for (uint8_t i = 0; i < _peerCount; i++) {
        esp_now_del_peer(_peers[i]);
        esp_now_peer_info_t p{};
        memcpy(p.peer_addr, _peers[i], 6);
        p.channel = _channel;
        p.ifidx   = WIFI_IF_STA;
        p.encrypt = false;
        esp_now_add_peer(&p);
    }
#endif
}

bool EspNowLink::addPeer(const uint8_t mac[6]) {
    for (uint8_t i = 0; i < _peerCount; i++) {
        if (memcmp(_peers[i], mac, 6) == 0) return true;    // already known
    }
    if (_peerCount >= cfg::ESPNOW_MAX_PEERS) return false;
    memcpy(_peers[_peerCount++], mac, 6);

#if defined(ESP8266)
    return esp_now_add_peer(sdkMac(mac), ESP_NOW_ROLE_COMBO, _channel, NULL, 0) == 0;
#else
    esp_now_peer_info_t p{};
    memcpy(p.peer_addr, mac, 6);
    p.channel = _channel;
    p.ifidx   = WIFI_IF_STA;
    p.encrypt = false;
    return esp_now_add_peer(&p) == ESP_OK;
#endif
}

void EspNowLink::removePeer(const uint8_t mac[6]) {
    for (uint8_t i = 0; i < _peerCount; i++) {
        if (memcmp(_peers[i], mac, 6) == 0) {
#if defined(ESP8266)
            esp_now_del_peer(sdkMac(mac));
#else
            esp_now_del_peer(mac);
#endif
            memmove(_peers[i], _peers[i + 1], (_peerCount - i - 1) * 6);
            _peerCount--;
            return;
        }
    }
}

bool EspNowLink::send(const uint8_t* mac, const uint8_t* buf, size_t len) {
    if (!_started || len > 250 || len == 0) return false;
    const uint8_t* dst = mac ? mac : BROADCAST_MAC;
#if defined(ESP8266)
    // SDK API predates const-correctness
    return esp_now_send(sdkMac(dst), sdkMac(buf), (int)len) == 0;
#else
    return esp_now_send(dst, buf, len) == ESP_OK;
#endif
}

void EspNowLink::pushRx(const uint8_t* mac, int8_t rssi, const uint8_t* data, int len) {
    if (!data || len <= 0 || len > 250) return;
    uint8_t next = (_head + 1) & (RING_SLOTS - 1);
    if (next == _tail) {                        // ring full: drop newest
        _drops++;
        return;
    }
    RxFrame& f = _ring[_head];
    memcpy(f.mac, mac, 6);
    f.rssi = rssi;
    f.len  = (uint16_t)len;
    memcpy(f.data, data, len);
    _head = next;
}

bool EspNowLink::poll(RxFrame& out) {
    if (_tail == _head) return false;
    out = _ring[_tail];
    _tail = (_tail + 1) & (RING_SLOTS - 1);
    return true;
}
