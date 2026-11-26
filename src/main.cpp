#include "tracker.h"
#include <M5Unified.h>
#include <WiFi.h>

namespace {
bool initialDemo, fetching, attempted, rateLimited;
unsigned long lastRequest, waitMs = PollSeconds * 1000;
}

void onRegionChanged() {
    clearRegionTraffic();
    if (!rateLimited) attempted = false;
}

void setup() {
    Serial.begin(115200);
    M5.begin();
    configureDevice();
    configTzTime("JST-9", "ntp.nict.jp", "time.cloudflare.com", "pool.ntp.org");
    startFeed();
    initializeDisplay();
    initialDemo = !wifiConfigured();
    setDemo(initialDemo);
}

void loop() {
    M5.update();
    serviceDevice();
    if (initialDemo && WiFi.isConnected()) {
        initialDemo = false;
        setDemo(false);
    }
    updateTouch();
    const Feed *result;
    if (takeFeed(result)) {
        fetching = false;
        rateLimited = result->http == 429 || result->retryAfter > 0;
        if (rateLimited) { attempted = true; lastRequest = millis(); }
        if (result->region == settings.region) {
            if (!result->error[0]) acceptFeed(*result);
            else setFeedStatus(result->error);
        } else if (rateLimited) setFeedStatus(result->error);
        waitMs = 1000UL * std::max(PollSeconds, result->retryAfter);
    }
    if (!demoMode && !fetching) {
        if (!WiFi.isConnected()) setFeedStatus(wifiConfigured() ? "Connecting to Wi-Fi" : "Set Wi-Fi via USB");
        else if (time(nullptr) < 1700000000) setFeedStatus("Synchronizing clock");
        else if (!attempted || millis() - lastRequest >= waitMs) {
            attempted = true;
            lastRequest = millis();
            fetching = requestFeed(settings.region);
            setFeedStatus(fetching ? "Receiving ADS-B" : "Feed worker unavailable");
        }
    }
    renderTracker();
    delay(2);
}
