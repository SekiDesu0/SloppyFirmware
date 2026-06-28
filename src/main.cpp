#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include "Adafruit_MPR121.h"

const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
const int udpPort = 4242;

// Auto-discovery variables
IPAddress targetIP;
bool clientConnected = false;

#define I2C_SDA 8
#define I2C_SCL 9
#define RGB_LED 48 // Standard WS2812 pin for S3 SuperMini clones

Adafruit_MPR121 cap = Adafruit_MPR121();
WiFiUDP udp;

struct __attribute__((packed)) FingerData {
  uint32_t packetId;
  uint16_t mid_p;  uint16_t mid_d;
  uint16_t ring_p; uint16_t ring_d;
  uint16_t pinky_p; uint16_t pinky_d;
  uint16_t i2cReadTimeMs;
  uint16_t totalLoopTimeMs;
};

uint32_t packetCount = 0;
unsigned long lastFrameTime = 0;
const int TARGET_FPS = 50;
const int FRAME_TIME_MS = 1000 / TARGET_FPS;

void setup() {
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000); 

  if (!cap.begin(0x5A)) {
    while (1) { delay(10); } 
  }

  cap.writeRegister(MPR121_ECR, 0x00);
  cap.writeRegister(MPR121_AUTOCONFIG0, 0x00);
  cap.writeRegister(MPR121_AUTOCONFIG1, 0x00);
  cap.writeRegister(MPR121_CONFIG1, 0x10); 
  cap.writeRegister(MPR121_CONFIG2, 0x20); 
  cap.writeRegister(MPR121_ECR, 0x8F);

  // Turn LED Red while connecting
  neopixelWrite(RGB_LED, 10, 0, 0);

  WiFi.begin(ssid, password);
  WiFi.setSleep(WIFI_PS_NONE); 
  while (WiFi.status() != WL_CONNECTED) { delay(500); }

  // Turn LED Yellow while waiting for Python discovery ping
  neopixelWrite(RGB_LED, 10, 10, 0);

  // Start listening for the Python ping
  udp.begin(udpPort);
}

void loop() {
  // Phase 1: Wait for Python to reveal its IP
  if (!clientConnected) {
    if (udp.parsePacket()) {
      targetIP = udp.remoteIP();
      clientConnected = true;
      // Turn LED Green when target is locked
      neopixelWrite(RGB_LED, 0, 10, 0);
      
      // Flush the buffer
      while(udp.available()) udp.read();
    }
    yield();
    return; // Do not run the sensor loop yet
  }

  // Phase 2: Blast data to the locked IP
  unsigned long currentMillis = millis();
  if (currentMillis - lastFrameTime >= FRAME_TIME_MS) {
    unsigned long loopStart = millis();

    FingerData payload;
    payload.packetId = packetCount++;

    unsigned long readStart = millis();
    payload.mid_p = cap.filteredData(0);
    payload.mid_d = cap.filteredData(1);
    payload.ring_p = cap.filteredData(2);
    payload.ring_d = cap.filteredData(3);
    payload.pinky_p = cap.filteredData(4);
    payload.pinky_d = cap.filteredData(5);
    payload.i2cReadTimeMs = millis() - readStart;

    payload.totalLoopTimeMs = millis() - loopStart;

    udp.beginPacket(targetIP, udpPort);
    udp.write((uint8_t*)&payload, sizeof(payload));
    udp.endPacket();

    lastFrameTime = currentMillis;
  }
  
  yield(); 
}