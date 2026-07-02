// SloppyFirmware v2
// Reads all 12 MPR121 electrodes and streams them to a server over UDP.
//
// State machine: PROVISIONING -> CONNECTING -> DISCOVERING -> STREAMING
// Provisioning waits for serial `wifi set <ssid> <pass>` if no creds stored.

#include <Arduino.h>
#if defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#include <esp_task_wdt.h>
#endif

// Run CPU at 80 MHz instead of 240 MHz
static constexpr unsigned long CPU_FREQ_MHZ = 80;

#include "config.h"
#include "StatusLED.h"
#include "WifiManager.h"
#include "SerialCLI.h"
#include "SensorMPR121.h"
#include "Discovery.h"
#include "PacketIO.h"
#include "DeviceConfig.h"

// --- Globals (externs for SerialCLI callbacks) ------------------------------
WifiManager   wifi;
SensorMPR121  sensor;
StatusLED     led;
Discovery     discovery;
SerialCLI     cli;
DeviceConfig  deviceCfg;

DeviceState   state = DeviceState::Provisioning;
uint32_t      packetId       = 0;
uint32_t      lastHelloMs     = 0;
uint32_t      lastFrameMs    = 0;
uint32_t      lastKeepaliveMs = 0;
uint32_t      stateEnterMs   = 0;

// --- CLI callbacks ----------------------------------------------------------
static void cb_status() {
    Serial.printf("state=%s fw=%u uptime=%lus pkts=%lu\r\n",
        state == DeviceState::Provisioning ? "provisioning" :
        state == DeviceState::Connecting   ? "connecting"   :
        state == DeviceState::Discovering  ? "discovering"  :
                                             "streaming",
        cfg::FW_VERSION,
        (unsigned long)(millis() / 1000),
        (unsigned long)packetId);
    Serial.println(wifi.statusLine());
    Serial.printf("hand=%s sensor=%s channels=%u\r\n",
        deviceCfg.handString(),
        sensor.present() ? "ok" : "absent",
        cfg::CHANNEL_COUNT);
    if (discovery.hasServer()) {
        Serial.printf("server=%s:%u keepaliveMs=%lu lastKeepaliveAgo=%lums\r\n",
            discovery.serverIP().toString().c_str(),
            discovery.dataPort(),
            (unsigned long)discovery.keepaliveMs(),
            (long)(millis() - lastKeepaliveMs));
    } else {
        Serial.println("server=none");
    }
}

static bool cb_wifiSet(const String& ssid, const String& pass) {
    return wifi.setCredentials(ssid, pass);
}
static void cb_wifiClear() {
    wifi.clearCredentials();
}

static void cb_handSet(uint8_t h) {
    deviceCfg.setHand(h);
}

// --- Helpers ----------------------------------------------------------------
static void enterState(DeviceState s) {
    state       = s;
    stateEnterMs = millis();
    led.setState(s);
}

static void watchdogInit() {
#if defined(ESP32)
#if ESP_IDF_VERSION_MAJOR >= 5
    const esp_task_wdt_config_t cfg = {
        .timeout_ms = 10000,
        .idle_core_mask = 0,
        .trigger_panic = true
    };
    esp_task_wdt_init(&cfg);
#else
    esp_task_wdt_init(10000, true);
#endif
    esp_task_wdt_add(NULL);
#endif
}

// --- Setup ------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(50);
#if defined(ESP32)
    setCpuFrequencyMhz(CPU_FREQ_MHZ);
#elif defined(ESP8266)
    system_update_cpu_freq(CPU_FREQ_MHZ);
#endif
    led.begin(cfg::RGB_LED);
    led.setState(DeviceState::Provisioning);

    watchdogInit();

    bool sensorOk = sensor.begin(cfg::MPR121_ADDR,
                                 cfg::I2C_SDA, cfg::I2C_SCL,
                                 cfg::I2C_CLOCK_HZ);
    if (!sensorOk) {
        Serial.printf("[BOOT] MPR121 not found at 0x%02X. Proceeding without sensor (server will get empty frames).\r\n",
            cfg::MPR121_ADDR);
    } else {
        Serial.println("[BOOT] MPR121 initialized (12 channels).");
    }
    led.setSensorAbsent(!sensorOk);

    deviceCfg.begin();
    Serial.printf("[BOOT] Hand assignment: %s (use serial 'hand left|right|auto' to change)\r\n",
        deviceCfg.handString());

    cli.begin(cb_status, cb_wifiSet, cb_wifiClear, cb_handSet);

    if (wifi.hasCredentials()) {
        String ssid, pass;
        wifi.getCredentials(ssid, pass);
        Serial.printf("[BOOT] Stored creds found for \"%s\". Connecting...\r\n",
            ssid.c_str());
        wifi.beginConnect();
        enterState(DeviceState::Connecting);
        discovery.begin(cfg::UDP_PORT);
    } else {
        Serial.println("[BOOT] No WiFi creds stored. Waiting for serial 'wifi set <ssid> <pass>'.");
        enterState(DeviceState::Provisioning);
    }
}

// --- State-specific loops ---------------------------------------------------
static void loopProvisioning() {
    // Re-print instructions every few seconds so a serial monitor opened
    // after boot still learns what to do. Non-blocking.
    static uint32_t lastHintMs = 0;
    if (millis() - lastHintMs > 3000) {
        lastHintMs = millis();
        Serial.println();
        Serial.println("==============================================");
        Serial.println(" No WiFi credentials stored.");
        Serial.println(" Type:  wifi set <ssid> <pass>");
        Serial.println(" Then press Enter. Device will reboot & connect.");
        Serial.println(" Other commands: status | wifi clear | reset | help");
        Serial.println("==============================================");
        Serial.print("> ");
    }
}

static void loopConnecting() {
    if (wifi.update()) {
        Serial.printf("[NET] WiFi connected. IP=%s RSSI=%d. Entering discovery.\r\n",
            wifi.ip().toString().c_str(),
            (int)wifi.rssi());
        if (!discovery.hasServer()) {
            // (re)start UDP listening on the local port if not already
            discovery.begin(cfg::UDP_PORT);
        }
        enterState(DeviceState::Discovering);
    }
}

static void loopDiscovering() {
    if (millis() - lastHelloMs >= cfg::HELLO_INTERVAL_MS) {
        lastHelloMs = millis();
        discovery.sendHello(deviceCfg.getHand());
    }

    WelcomePacket w;
    KeepalivePacket k;
    PacketType t = discovery.pump(w, k);
    if (t == PacketType::Welcome) {
        // Discovery::pump captured the sender IP internally; use w to set
        // the data port and keepalive cadence.
        discovery.setServer(discovery.serverIP(), w.dataPort, w.keepaliveMs);
        Serial.printf("[DISC] Server welcomed us: %s port=%u keepalive=%lums\r\n",
            discovery.serverIP().toString().c_str(),
            w.dataPort, (unsigned long)w.keepaliveMs);
        lastKeepaliveMs = millis();
        enterState(DeviceState::Streaming);
    }
}

static void loopStreaming() {
    // 1) Drain inbound (keepalive / welcome / bye)
    WelcomePacket w;
    KeepalivePacket k;
    PacketType t = discovery.pump(w, k);
    if (t == PacketType::Keepalive) {
        lastKeepaliveMs = millis();
    } else if (t == PacketType::Welcome) {
        // server restarted; re-arm
        discovery.setServer(discovery.serverIP(), w.dataPort, w.keepaliveMs);
        lastKeepaliveMs = millis();
    }

    // 2) Check keepalive timeout -> back to discovery
    if (millis() - lastKeepaliveMs > cfg::KEEPALIVE_TIMEOUT_MS) {
        Serial.println("[DISC] Keepalive timeout. Returning to discovery.");
        discovery.clearServer();
        enterState(DeviceState::Discovering);
        return;
    }

    // 3) Check WiFi drop
    if (wifi.hadDisconnect() || !wifi.isConnected()) {
        Serial.println("[NET] WiFi dropped. Returning to CONNECTING.");
        discovery.clearServer();
        enterState(DeviceState::Connecting);
        return;
    }

    // 4) Stream sensor frame at ~50 FPS
    uint32_t now = millis();
    if (now - lastFrameMs >= cfg::FRAME_TIME_MS) {
        unsigned long loopStart = millis();
        uint16_t filtered[12] = {0};
        uint16_t touch = 0;
        uint16_t i2cMs = 0;
        if (sensor.present()) {
            unsigned long i2cStart = millis();
            touch = sensor.readAll(filtered);
            i2cMs = (uint16_t)(millis() - i2cStart);
        }
        uint16_t loopMs = (uint16_t)(millis() - loopStart);
        DataPacket p;
        PacketIO::buildData(p, packetId++, filtered, touch,
                            i2cMs, loopMs, wifi.rssi());
        discovery.sendData(p);
        lastFrameMs = now;
    } else {
        delay(1);
    }
}

// --- Loop -------------------------------------------------------------------
void loop() {
#if defined(ESP32)
    esp_task_wdt_reset();
#endif
    cli.update();
    led.tick();

    switch (state) {
        case DeviceState::Provisioning: loopProvisioning(); break;
        case DeviceState::Connecting:    loopConnecting();  break;
        case DeviceState::Discovering:   loopDiscovering(); break;
        case DeviceState::Streaming:     loopStreaming();   break;
    }

    // Universal WiFi drop detection (catches both CONNECTING and after STREAMING)
    if (state != DeviceState::Provisioning && wifi.hadDisconnect()) {
        Serial.println("[NET] WiFi disconnected by event.");
        discovery.clearServer();
        enterState(DeviceState::Connecting);
    }

    yield();
}