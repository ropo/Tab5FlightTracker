#include "tracker.h"
#include <ArduinoJson.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

extern const char settingsPage[] asm("_binary_web_settings_html_start");
extern const char flightImage[] asm("_binary_web_ropo_flight_jpg_start");
extern const char flightImageEnd[] asm("_binary_web_ropo_flight_jpg_end");
Settings settings;

namespace {
Preferences prefs;
WebServer server(80);
struct Credentials { char ssid[33], pass[65]; };
Credentials station{};
bool serving, reconnect, storedWifi;
uint32_t lastConnect;
char line[768];
size_t used;
bool discard;

bool allowed(const Settings &s) {
    return s.brightness >= 5 && s.brightness <= 100 &&
           (s.range == 20 || s.range == 40 || s.range == 60) &&
           (s.cycle == 0 || (s.cycle >= 5 && s.cycle <= 60)) &&
           s.sweep >= 3 && s.sweep <= 20 && s.region < RegionCount;
}

void describe(JsonDocument &reply) {
    reply["ok"] = true;
    auto s = reply["settings"].to<JsonObject>();
    s["brightness"] = settings.brightness;
    s["range"] = settings.range;
    s["cycle"] = settings.cycle;
    s["sweep"] = settings.sweep;
    s["region"] = settings.region;
    reply["connected"] = WiFi.isConnected();
    if (WiFi.isConnected()) reply["ip"] = WiFi.localIP().toString();
    reply["configured"] = storedWifi;
    describeRadar(reply["radar"].to<JsonObject>());
}

const char *apply(JsonDocument &req, bool usb) {
    const char *op = req["op"] | "";
    if (!strcmp(op, "get")) return nullptr;
    if (!strcmp(op, "wifi")) {
        if (!usb) return "Wi-Fi registration requires USB";
        const auto ssid = req["ssid"].as<JsonString>(), pass = req["password"].as<JsonString>();
        if (ssid.isNull() || pass.isNull() || !ssid.size() || ssid.size() > 32 ||
            pass.size() > 63 || (pass.size() && pass.size() < 8) ||
            strlen(ssid.c_str()) != ssid.size() || strlen(pass.c_str()) != pass.size())
            return "SSID 1-32 bytes / password 8-63 bytes or empty";
        Credentials next{};
        strlcpy(next.ssid, ssid.c_str(), sizeof(next.ssid));
        strlcpy(next.pass, pass.c_str(), sizeof(next.pass));
        if (prefs.putBytes("station", &next, sizeof(next)) != sizeof(next)) return "Save failed";
        station = next;
        storedWifi = reconnect = true;
        setDemo(false);
        return nullptr;
    }
    if (strcmp(op, "save")) return "Unknown operation";
    auto s = req["settings"].as<JsonObjectConst>();
    const bool hasRegion = s["region"].is<uint8_t>();
    if (!s || s.size() != (hasRegion ? 5 : 4)) return "Invalid display settings";
    for (auto key : {"brightness","range","cycle","sweep"})
        if (!s[key].is<uint8_t>()) return "Invalid setting";
    Settings next{s["brightness"], s["range"], s["cycle"], s["sweep"], s["region"] | settings.region};
    if (!allowed(next)) return "Setting outside range";
    if (memcmp(&next, &settings, sizeof(next))) {
        if (prefs.putBytes("display", &next, sizeof(next)) != sizeof(next)) return "Save failed";
        const bool regionChanged = next.region != settings.region;
        settings = next;
        if (regionChanged) onRegionChanged();
        M5.Display.setBrightness(settings.brightness * 255 / 100);
    }
    return nullptr;
}

String command(const char *data, bool usb, int &status) {
    JsonDocument req, reply;
    const char *error = deserializeJson(req, data) ? "Invalid JSON" : apply(req, usb);
    status = error ? 400 : 200;
    if (error) { reply["ok"] = false; reply["error"] = error; }
    else {
        describe(reply);
        if (usb && storedWifi) reply["ssid"] = station.ssid;
    }
    String output;
    serializeJson(reply, output);
    return output;
}

void connectStation() {
    if (storedWifi) WiFi.begin(station.ssid, station.pass);
    else WiFi.begin(); // 既存のSTA登録があれば再利用
    lastConnect = millis();
}
}

bool wifiConfigured() { return storedWifi || WiFi.isConnected(); }

void configureDevice() {
    prefs.begin("flight-radar", false);
    Settings saved;
    // 旧版は地域を含まない先頭4バイト。地域は初期値の羽田を使う。
    size_t length = prefs.getBytesLength("display");
    if ((length == sizeof(saved) || length == offsetof(Settings, region)) &&
        prefs.getBytes("display", &saved, length) == length && allowed(saved))
        settings = saved;
    storedWifi = prefs.getBytesLength("station") == sizeof(station) &&
        prefs.getBytes("station", &station, sizeof(station)) == sizeof(station) && station.ssid[0];
    M5.Display.setBrightness(settings.brightness * 255 / 100);
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    connectStation();
    server.on("/", HTTP_GET, [] { server.send_P(200, "text/html; charset=utf-8", settingsPage); });
    server.on("/ropo-flight.jpg", HTTP_GET, [] {
        server.send_P(200, "image/jpeg", flightImage, flightImageEnd - flightImage);
    });
    server.on("/settings", HTTP_GET, [] {
        JsonDocument doc;
        describe(doc);
        String body;
        serializeJson(doc, body);
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "application/json", body);
    });
    const char *headers[] = {"Content-Type", "Origin", "Host"};
    server.collectHeaders(headers, 3);
    server.on("/settings", HTTP_POST, [] {
        const String origin = server.header("Origin");
        if (origin.length() && origin != String("http://") + server.hostHeader()) {
            server.send(403); return;
        }
        if (!server.header("Content-Type").startsWith("application/json")) { server.send(415); return; }
        const String body = server.arg("plain");
        if (body.length() > sizeof(line)) { server.send(413); return; }
        int status;
        String response = command(body.c_str(), false, status);
        server.send(status, "application/json", response);
    });
}

void serviceDevice() {
    for (int n = 0; n < 768 && Serial.available(); ++n) {
        char c = Serial.read();
        if (c == '\n') {
            if (discard) Serial.println("{\"ok\":false,\"error\":\"Request too long\"}");
            else if (used) {
                line[used] = 0;
                int status;
                Serial.println(command(line, true, status));
            }
            memset(line, 0, used);
            used = 0;
            discard = false;
        } else if (c != '\r') {
            if (used < sizeof(line)-1) line[used++] = c;
            else discard = true;
        }
    }
    if (reconnect) {
        reconnect = false;
        WiFi.disconnect();
        connectStation();
    } else if (!WiFi.isConnected() && storedWifi && millis()-lastConnect >= 30000) {
        connectStation();
    }
    if (WiFi.isConnected()) {
        if (!serving) { server.begin(); serving = true; }
        server.handleClient();
    } else if (serving) {
        server.stop();
        serving = false;
    }
}
