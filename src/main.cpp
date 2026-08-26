// SloppyFirmware v6
// Reads all 12 MPR121 electrodes (or 4 FDC2214 channels) and streams them to
// a server over UDP or ESP-NOW, with battery gauge in the DATA frames.
//
// State machine: PROVISIONING -> CONNECTING -> DISCOVERING -> STREAMING
// Provisioning waits for serial `wifi set <ssid> <pass>` if no creds stored.

#include <Arduino.h>
#include <Wire.h>
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
#include "SensorFDC2214.h"
#include "BatteryMonitor.h"
#include "Discovery.h"
#include "EspNowTransport.h"
#include "PacketIO.h"
#include "DeviceConfig.h"
#include "SensorTransport.h"

// --- Globals (externs for SerialCLI callbacks) ------------------------------
WifiManager   wifi;
SensorMPR121  sensor;
SensorFDC2214 fdcSensor;
BatteryMonitor battery;
StatusLED     led;
Discovery     discovery;
EspNowTransport espnowLink;
SerialCLI     cli;
DeviceConfig  deviceCfg;

SensorTransport* transportPtr   = nullptr;   // active link (UDP or ESP-NOW)
uint8_t          activeTransport = cfg::TRANSPORT_DEFAULT;

DeviceState   state = DeviceState::Provisioning;
uint32_t      packetId       = 0;
uint32_t      lastHelloMs     = 0;
uint32_t      lastFrameMs    = 0;
uint32_t      lastKeepaliveMs = 0;
uint32_t      stateEnterMs   = 0;
uint8_t       sensorType     = cfg::SENSOR_TYPE_NONE;

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
    Serial.printf("transport=%s hand=%s sensorMode=%s sensorType=%s channels=%u\r\n",
        activeTransport == cfg::TRANSPORT_ESPNOW ? "espnow" : "wifi",
        deviceCfg.handString(),
        deviceCfg.sensorModeString(),
        sensorType == cfg::SENSOR_TYPE_MPR121  ? "mpr121" :
        sensorType == cfg::SENSOR_TYPE_FDC2214 ? "fdc2214" : "none",
        sensorType == cfg::SENSOR_TYPE_FDC2214 ? cfg::FDC_CHANNEL_COUNT : cfg::CHANNEL_COUNT);
    if (transportPtr) {
        Serial.println(transportPtr->statusLine());
    } else {
        Serial.println("link=none");
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

static void cb_sensorSet(uint8_t mode) {
    deviceCfg.setSensorMode(mode);
}

static void cb_transportSet(uint8_t tr) {
    deviceCfg.setTransport(tr);
}

static void cb_pairClear() {
    deviceCfg.clearPair();
    espnowLink.clearPair();
}

static void cb_i2cScan() {
    Wire.begin(cfg::I2C_SDA, cfg::I2C_SCL);
    Wire.setClock(cfg::I2C_CLOCK_HZ);
    Serial.printf("Scanning I2C bus (SDA=%d SCL=%d)...\r\n", cfg::I2C_SDA, cfg::I2C_SCL);
    uint8_t found = 0;
    for (uint8_t a = 0x03; a <= 0x77; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            Serial.printf("  0x%02X%s\r\n", a,
                a == cfg::MPR121_ADDR ? "  <- MPR121" :
                (a == 0x2A || a == 0x2B) ? "  <- FDC2214" : "");
            found++;
        }
        yield();
    }
    if (!found) Serial.println("  (bus empty - check wiring/power/ADDR pin)");
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

    deviceCfg.begin();
    activeTransport = deviceCfg.getTransport();
    Serial.printf("[BOOT] Transport: %s (use serial 'transport wifi|espnow' to change)\r\n",
        activeTransport == cfg::TRANSPORT_ESPNOW ? "espnow" : "wifi");
    Serial.printf("[BOOT] Hand assignment: %s (use serial 'hand left|right|auto' to change)\r\n",
        deviceCfg.handString());
    Serial.printf("[BOOT] Sensor mode: %s (use serial 'sensor auto|mpr121|fdc2214' to change)\r\n",
        deviceCfg.sensorModeString());

    uint8_t mode = deviceCfg.getSensorMode();
    bool mprOk = false;
    bool fdcOk = false;

    if (mode == cfg::SENSOR_MODE_FDC2214) {
        fdcOk = fdcSensor.begin(cfg::I2C_SDA, cfg::I2C_SCL, cfg::I2C_CLOCK_HZ);
        sensorType = fdcOk ? cfg::SENSOR_TYPE_FDC2214 : cfg::SENSOR_TYPE_NONE;
    } else if (mode == cfg::SENSOR_MODE_MPR121) {
        mprOk = sensor.begin(cfg::MPR121_ADDR, cfg::I2C_SDA, cfg::I2C_SCL, cfg::I2C_CLOCK_HZ);
        sensorType = mprOk ? cfg::SENSOR_TYPE_MPR121 : cfg::SENSOR_TYPE_NONE;
    } else {
        // auto: probe FDC2214 first (higher resolution), fall back to MPR121
        fdcOk = fdcSensor.begin(cfg::I2C_SDA, cfg::I2C_SCL, cfg::I2C_CLOCK_HZ);
        if (fdcOk) {
            sensorType = cfg::SENSOR_TYPE_FDC2214;
        } else {
            mprOk = sensor.begin(cfg::MPR121_ADDR, cfg::I2C_SDA, cfg::I2C_SCL, cfg::I2C_CLOCK_HZ);
            sensorType = mprOk ? cfg::SENSOR_TYPE_MPR121 : cfg::SENSOR_TYPE_NONE;
        }
    }

    if (sensorType == cfg::SENSOR_TYPE_NONE) {
        Serial.println("[BOOT] No sensor found. Proceeding without sensor (server will get empty frames).");
    } else if (sensorType == cfg::SENSOR_TYPE_FDC2214) {
        Serial.println("[BOOT] FDC2214 initialized (4 channels).");
    } else {
        Serial.println("[BOOT] MPR121 initialized (12 channels).");
    }
    led.setSensorAbsent(sensorType == cfg::SENSOR_TYPE_NONE);

    battery.begin();
    Serial.printf("[BOOT] Battery monitor: %s (pin %d)\r\n",
        battery.enabled() ? "enabled" : "disabled",
        cfg::BATT_ADC_PIN);

    if (activeTransport == cfg::TRANSPORT_ESPNOW) {
        // ESP-NOW mode: no WiFi credentials needed; pair with the dongle.
        transportPtr = &espnowLink;
        uint8_t pairMac[6];
        if (deviceCfg.hasPair()) {
            deviceCfg.getPairMac(pairMac);
            espnowLink.configure(deviceCfg.getPairChannel(), pairMac);
            Serial.printf("[BOOT] Remembered dongle %02X:%02X:%02X:%02X:%02X:%02X on ch%u. Hello!\r\n",
                pairMac[0], pairMac[1], pairMac[2], pairMac[3], pairMac[4], pairMac[5],
                deviceCfg.getPairChannel());
        } else {
            espnowLink.configure(cfg::ESPNOW_CHANNEL_DEFAULT, nullptr);
            Serial.println("[BOOT] No dongle paired yet. Channel-hopping discovery.");
        }
    } else {
        transportPtr = &discovery;
    }

    cli.begin(cb_status, cb_wifiSet, cb_wifiClear, cb_handSet, cb_sensorSet,
              cb_transportSet, cb_pairClear, cb_i2cScan);

    if (activeTransport == cfg::TRANSPORT_ESPNOW) {
        transportPtr->begin();
        enterState(DeviceState::Discovering);
    } else if (wifi.hasCredentials()) {
        String ssid, pass;
        wifi.getCredentials(ssid, pass);
        Serial.printf("[BOOT] Stored creds found for \"%s\". Connecting...\r\n",
            ssid.c_str());
        wifi.beginConnect();
        enterState(DeviceState::Connecting);
        transportPtr->begin();
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
    if (millis() - lastHelloMs >= transportPtr->helloIntervalMs()) {
        lastHelloMs = millis();
        transportPtr->sendHello(deviceCfg.getHand());
    }

    WelcomePacket w;
    KeepalivePacket k;
    PacketType t = transportPtr->pump(w, k);
    if (t == PacketType::Welcome && transportPtr->acceptWelcome(w)) {
        Serial.printf("[DISC] Server accepted us (%s). Entering streaming.\r\n",
            activeTransport == cfg::TRANSPORT_ESPNOW ? "dongle" : "udp");
        if (activeTransport == cfg::TRANSPORT_ESPNOW && !deviceCfg.hasPair()
                && espnowLink.peerMac()) {
            // First successful pairing: remember MAC + channel for instant
            // reconnects on later boots.
            deviceCfg.setPair(espnowLink.peerMac(), espnowLink.channel());
            Serial.println("[DISC] Dongle paired & remembered ('pair clear' to forget).");
        }
        lastKeepaliveMs = millis();
        enterState(DeviceState::Streaming);
    }
}

static void loopStreaming() {
    // 1) Drain inbound (keepalive / welcome / bye)
    WelcomePacket w;
    KeepalivePacket k;
    PacketType t = transportPtr->pump(w, k);
    if (t == PacketType::Keepalive) {
        lastKeepaliveMs = millis();
    } else if (t == PacketType::Welcome) {
        // server restarted; re-arm
        transportPtr->acceptWelcome(w);
        lastKeepaliveMs = millis();
    }

    // 2) Check keepalive timeout -> back to discovery
    if (millis() - lastKeepaliveMs > cfg::KEEPALIVE_TIMEOUT_MS) {
        Serial.println("[DISC] Keepalive timeout. Returning to discovery.");
        transportPtr->clearServer();
        enterState(DeviceState::Discovering);
        return;
    }

    // 3) Check WiFi drop (UDP mode only)
    if (activeTransport == cfg::TRANSPORT_WIFI &&
            (wifi.hadDisconnect() || !wifi.isConnected())) {
        Serial.println("[NET] WiFi dropped. Returning to CONNECTING.");
        transportPtr->clearServer();
        enterState(DeviceState::Connecting);
        return;
    }

    // 4) Stream sensor frame at ~50 FPS
    uint32_t now = millis();
    if (now - lastFrameMs >= cfg::FRAME_TIME_MS) {
        unsigned long loopStart = millis();
        uint16_t filtered[12] = {0};
        uint16_t touch = 0;
        uint32_t fdcRaw[4] = {0};
        uint16_t i2cMs = 0;
        unsigned long i2cStart = millis();
        if (sensorType == cfg::SENSOR_TYPE_MPR121 && sensor.present()) {
            touch = sensor.readAll(filtered);
        } else if (sensorType == cfg::SENSOR_TYPE_FDC2214 && fdcSensor.present()) {
            fdcSensor.readAll(fdcRaw);
        }
        i2cMs = (uint16_t)(millis() - i2cStart);
        uint16_t loopMs = (uint16_t)(millis() - loopStart);
        uint8_t battPct  = battery.read();
        uint16_t battMv  = battery.milliVolts();
        DataPacket p;
        PacketIO::buildData(p, packetId++, filtered, touch, fdcRaw, sensorType,
                            i2cMs, loopMs, wifi.rssi(), battMv, battPct);
        transportPtr->sendData(p);
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

    // Universal WiFi drop detection (UDP transport only)
    if (activeTransport == cfg::TRANSPORT_WIFI && state != DeviceState::Provisioning &&
            wifi.hadDisconnect()) {
        Serial.println("[NET] WiFi disconnected by event.");
        transportPtr->clearServer();
        enterState(DeviceState::Connecting);
    }

    yield();
}