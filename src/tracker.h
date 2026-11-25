#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

constexpr double CenterLat = 35.5494, CenterLon = 139.7798;
constexpr int MaxAircraft = 96;
constexpr unsigned PollSeconds = 180;

struct Point { float east, north; }; // 羽田を原点とした海里
Point projectPosition(double lat, double lon);

struct Aircraft {
    char hex[10]{}, call[12]{}, type[9]{};
    Point pos{};
    float altitude = NAN, speed = NAN, heading = NAN;
    bool ground = false;
    Point trail[5]{};
    uint8_t trailCount = 0;
};

struct Feed {
    Aircraft aircraft[MaxAircraft]{};
    int count = 0;
    time_t observed = 0;
    int http = 0;
    unsigned retryAfter = 0;
    char error[64]{};
};

struct Settings {
    uint8_t brightness = 65, range = 40, cycle = 12, sweep = 8;
};

extern Settings settings;
extern bool demoMode;
void configureDevice();
void serviceDevice();
bool wifiConfigured();
void startFeed();
bool requestFeed();
bool takeFeed(const Feed *&result);
void initializeDisplay();
void renderTracker();
void updateTouch();
void acceptFeed(const Feed &result);
void setFeedStatus(const char *message);
void setDemo(bool enabled);
void describeRadar(JsonObject out);
