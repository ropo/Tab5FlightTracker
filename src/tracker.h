#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

constexpr unsigned RegionCount = 3;
struct Region {
    const char *airport, *title;
    double lat, lon;
    const uint16_t *rows, *spans;
};
extern const Region regions[RegionCount];
constexpr int MaxAircraft = 96;
constexpr unsigned PollSeconds = 180;

struct Point { float east, north; }; // 選択中の空港を原点とした海里
Point projectPosition(const Region &region, double lat, double lon);

struct Aircraft {
    char hex[10]{}, call[12]{}, type[9]{};
    Point pos{};
    float altitude = NAN, speed = NAN, heading = NAN;
    bool ground = false;
    Point trail[5]{};
    uint8_t trailCount = 0;
};

struct Feed {
    uint8_t region = 0;
    Aircraft aircraft[MaxAircraft]{};
    int count = 0;
    time_t observed = 0;
    int http = 0;
    unsigned retryAfter = 0;
    char error[64]{};
};

struct Settings {
    uint8_t brightness = 65, range = 40, cycle = 12, sweep = 8, region = 0;
};

extern Settings settings;
extern bool demoMode;
void configureDevice();
void serviceDevice();
bool wifiConfigured();
void startFeed();
bool requestFeed(uint8_t region);
bool takeFeed(const Feed *&result);
void initializeDisplay();
void renderTracker();
void updateTouch();
void acceptFeed(const Feed &result);
void clearRegionTraffic();
void onRegionChanged();
void setFeedStatus(const char *message);
void setDemo(bool enabled);
void describeRadar(JsonObject out);
