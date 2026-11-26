#include "tracker.h"
#include <ArduinoJson.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <algorithm>

Point projectPosition(const Region &region, double lat, double lon) {
    return {float((lon - region.lon) * 60 * cos(region.lat * DEG_TO_RAD)),
            float((lat - region.lat) * 60)};
}

namespace {
TaskHandle_t worker;
QueueHandle_t completed;
Feed report;
bool pending;
char requestKey[sizeof(FeedSettings::key)]{};
constexpr size_t BodyLimit = 768 * 1024;

struct Body {
    char *data;
    size_t used = 0;
    bool overflow = false;
};

esp_err_t onHttp(esp_http_client_event_t *event) {
    auto &body = *static_cast<Body *>(event->user_data);
    if (event->event_id == HTTP_EVENT_ON_HEADER &&
        strcasecmp(event->header_key, "Retry-After") == 0) {
        report.retryAfter = std::min(86400UL, strtoul(event->header_value, nullptr, 10));
    }
    if (event->event_id == HTTP_EVENT_ON_DATA) {
        if (body.used + event->data_len > BodyLimit) {
            body.overflow = true;
            return ESP_FAIL;
        }
        memcpy(body.data + body.used, event->data, event->data_len);
        body.used += event->data_len;
    }
    return ESP_OK;
}

float number(JsonVariantConst value) {
    return value.is<float>() ? value.as<float>() : NAN;
}

float distance2(const Aircraft &a) {
    return a.pos.east * a.pos.east + a.pos.north * a.pos.north;
}

const char *parseBody(Body &body) {
    JsonDocument filter, doc;
    filter["now"] = true;
    for (auto key : {"hex","flight","t","category","lat","lon","alt_baro","gs","track","seen_pos"})
        filter["ac"][0][key] = true;
    auto err = deserializeJson(doc, body.data, body.used, DeserializationOption::Filter(filter));
    if (err || !doc["ac"].is<JsonArray>() || !doc["now"].is<double>()) return "Invalid feed";
    double stamp = doc["now"];
    report.observed = time_t(stamp > 1.e11 ? stamp / 1000 : stamp);
    if (abs(difftime(time(nullptr), report.observed)) > 600) return "Feed timestamp expired";
    for (JsonObject a : doc["ac"].as<JsonArray>()) {
        if (!a["lat"].is<double>() || !a["lon"].is<double>() || !a["hex"].is<const char *>() ||
            (a["seen_pos"] | 0.0) > 90) continue;
        const char *category = a["category"] | "";
        // C1～C5は地上車両・障害物。駐機中の航空機は表示する。
        if (category[0] == 'C' && category[1] >= '1' && category[1] <= '5' && !category[2])
            continue;
        Aircraft p;
        p.pos = projectPosition(regions[report.region], a["lat"], a["lon"]);
        p.sample = p.pos;
        p.positionAge = std::max(0.f, a["seen_pos"] | 0.f);
        if (distance2(p) > 60.5f * 60.5f) continue;
        strlcpy(p.hex, a["hex"], sizeof(p.hex));
        String call = a["flight"] | "";
        call.trim();
        strlcpy(p.call, call.length() ? call.c_str() : p.hex, sizeof(p.call));
        strlcpy(p.type, a["t"] | "", sizeof(p.type));
        p.altitude = number(a["alt_baro"]);
        p.ground = strcmp(a["alt_baro"] | "", "ground") == 0;
        p.speed = number(a["gs"]);
        p.heading = number(a["track"]);
        auto *end = report.aircraft + report.count;
        if (report.count < MaxAircraft) report.aircraft[report.count++] = p;
        else {
            auto farthest = std::max_element(report.aircraft, end,
                [](const Aircraft &x, const Aircraft &y) { return distance2(x) < distance2(y); });
            if (distance2(p) < distance2(*farthest)) *farthest = p;
        }
    }
    std::sort(report.aircraft, report.aircraft + report.count,
        [](const Aircraft &a, const Aircraft &b) { return distance2(a) < distance2(b); });
    return "";
}

const char *download() {
    Body body{static_cast<char *>(ps_malloc(BodyLimit))};
    if (!body.data) return "Feed memory unavailable";
    esp_http_client_config_t cfg{};
    const auto &region = regions[report.region];
    char url[96];
    snprintf(url, sizeof(url), "https://api.adsb.lol/v2/point/%.4f/%.4f/60", region.lat, region.lon);
    cfg.url = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 12000;
    cfg.disable_auto_redirect = true;
    cfg.event_handler = onHttp;
    cfg.user_data = &body;
    cfg.user_agent = "Tab5FlightTracker/1.0";
    auto client = esp_http_client_init(&cfg);
    const char *error = "HTTPS connection failed";
    if (client) {
        if (*requestKey) esp_http_client_set_header(client, "x-api-key", requestKey);
        const auto rc = esp_http_client_perform(client);
        report.http = esp_http_client_get_status_code(client);
        if (rc != ESP_OK) {
            int tlsCode = 0, tlsFlags = 0;
            esp_http_client_get_and_clear_last_tls_error(client, &tlsCode, &tlsFlags);
            Serial.printf("ADS-B HTTPS: %s, errno=%d, TLS=%d, flags=%d\n",
                          esp_err_to_name(rc), esp_http_client_get_errno(client), tlsCode, tlsFlags);
        }
        if (body.overflow) error = "Feed exceeds memory limit";
        else if (rc == ESP_OK && report.http == 200) error = parseBody(body);
        else if (report.http == 429) error = "API rate limit";
        else if (report.http > 0) error = "API unavailable";
        esp_http_client_cleanup(client);
    }
    free(body.data);
    return error;
}

void fetchLoop(void *) {
    for (;;) {
        uint32_t region;
        xTaskNotifyWait(0, UINT32_MAX, &region, portMAX_DELAY);
        report.region = region;
        report.count = 0;
        report.http = 0;
        report.observed = 0;
        report.retryAfter = 0;
        strlcpy(report.error, download(), sizeof(report.error));
        memset(requestKey, 0, sizeof(requestKey));
        const uint8_t done = 1;
        xQueueSend(completed, &done, portMAX_DELAY);
    }
}
}

void startFeed() {
    completed = xQueueCreate(1, sizeof(uint8_t));
    if (completed) xTaskCreate(fetchLoop, "flight-feed", 16384, nullptr, 1, &worker);
}

bool requestFeed(uint8_t region, const char *key) {
    if (!worker || pending) return false;
    pending = true;
    strlcpy(requestKey, key, sizeof(requestKey));
    xTaskNotify(worker, region, eSetValueWithOverwrite);
    return true;
}

bool takeFeed(const Feed *&result) {
    uint8_t done;
    if (!completed || !xQueueReceive(completed, &done, 0)) return false;
    pending = false;
    result = &report;
    return true;
}
