#include "SerialCLI.h"

void SerialCLI::begin(StatusFn statusFn, WifiSetFn wifiSetFn, WifiClearFn wifiClearFn, HandSetFn handSetFn) {
    _statusFn    = statusFn;
    _wifiSetFn   = wifiSetFn;
    _wifiClearFn = wifiClearFn;
    _handSetFn   = handSetFn;
    _started = true;
    Serial.println();
    Serial.println("SloppyFirmware v3 - serial CLI ready.");
    _help();
    Serial.print("> ");
}

void SerialCLI::update() {
    if (!_started) return;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r' || c == '\n') {
            Serial.println();
            _exec(_line);
            _line = "";
            Serial.print("> ");
        } else if (c == 0x08 || c == 0x7F) {  // backspace / DEL
            if (_line.length()) {
                _line.remove(_line.length() - 1);
                Serial.write("\b \b");
            }
        } else if (c >= 0x20 && c < 0x7F) {  // printable: echo so user sees typing
            if (_line.length() < 128) {
                _line += c;
                Serial.write(c);
            }
        }
    }
}

void SerialCLI::_help() {
    Serial.println("Commands:");
    Serial.println("  wifi set <ssid> <pass>   Store WiFi creds in NVS and reboot");
    Serial.println("  wifi clear               Erase WiFi creds and reboot");
    Serial.println("  hand left                 Mark this device as the LEFT hand");
    Serial.println("  hand right                Mark this device as the RIGHT hand");
    Serial.println("  hand auto                 Clear hand assignment (unknown)");
    Serial.println("  status                   Print device diagnostics");
    Serial.println("  reset                    Soft-reset the device");
    Serial.println("  help                     Show this message");
}

void SerialCLI::_exec(const String& line) {
    String t = line;
    t.trim();
    if (t.length() == 0) return;

    if (t == "help") { _help(); return; }
    if (t == "status") { if (_statusFn) _statusFn(); return; }
    if (t == "reset")  { Serial.println("Rebooting..."); delay(100); ESP.restart(); return; }

    if (t.startsWith("hand ")) {
        String arg = t.substring(5);
        arg.trim();
        uint8_t h;
        if (arg == "left")         h = 1;
        else if (arg == "right")   h = 2;
        else if (arg == "auto" || arg == "unknown" || arg == "none") h = 0;
        else { Serial.println("Usage: hand left | hand right | hand auto"); return; }
        if (_handSetFn) _handSetFn(h);
        Serial.printf("Hand set to %s. Will be included in next HELLO.\r\n",
            h == 1 ? "left" : (h == 2 ? "right" : "unknown"));
        return;
    }

    if (t == "wifi clear") {
        if (_wifiClearFn) _wifiClearFn();
        Serial.println("WiFi creds cleared. Rebooting...");
        delay(200);
        ESP.restart();
        return;
    }

    if (t.startsWith("wifi set ")) {
        String rest = t.substring(9);
        rest.trim();
        int sp = rest.indexOf(' ');
        if (sp < 0) {
            Serial.println("Usage: wifi set <ssid> <pass>");
            return;
        }
        String ssid = rest.substring(0, sp);
        String pass = rest.substring(sp + 1);
        if (_wifiSetFn && _wifiSetFn(ssid, pass)) {
            Serial.println("WiFi creds stored. Rebooting...");
            delay(200);
            ESP.restart();
        } else {
            Serial.println("Failed to store creds (empty ssid?).");
        }
        return;
    }

    Serial.println("Unknown command. Type 'help'.");
}