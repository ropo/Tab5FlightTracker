#include "tracker.h"
#include <M5Unified.h>
#include <WiFi.h>

namespace {
bool initialDemo, fetching, attempted, rateLimited;
enum class KeyState { Unchecked, Valid, Rejected };
KeyState keyState = KeyState::Unchecked;
uint32_t configRevision, requestRevision;
bool keyedRequest;
unsigned long lastRequest, lastRegionChange, waitMs = PollSeconds * 1000;
}

unsigned pollSeconds() {
    return keyState == KeyState::Valid ? feedSettings.seconds : PollSeconds;
}

const char *feedKeyState() {
    if (!*feedSettings.key) return "empty";
    if (keyState == KeyState::Valid) return "valid";
    return keyState == KeyState::Rejected ? "invalid" : "pending";
}

void onFeedSettingsChanged() {
    ++configRevision;
    keyState = KeyState::Unchecked;
    if (!rateLimited) attempted = false;
}

void onRegionChanged() {
    clearRegionTraffic();
    lastRegionChange = millis();
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
        if (requestRevision == configRevision && keyedRequest) {
            if (result->http == 401 || result->http == 403) {
                keyState = KeyState::Rejected;
                if (!rateLimited) attempted = false; // 公開枠で一度取り直す
            } else if (!result->error[0]) keyState = KeyState::Valid;
        }
        if (result->region == settings.region) {
            if (!result->error[0]) acceptFeed(*result);
            else setFeedStatus(result->error);
        } else if (rateLimited) setFeedStatus(result->error);
        waitMs = 1000UL * std::max(rateLimited ? std::max(pollSeconds(), PollSeconds) : pollSeconds(), result->retryAfter);
    }
    if (!demoMode && !fetching) {
        if (!WiFi.isConnected()) setFeedStatus(wifiConfigured() ? "Connecting to Wi-Fi" : "Set Wi-Fi via USB");
        else if (time(nullptr) < 1700000000) setFeedStatus("Synchronizing clock");
        // 地域名を続けてタップしたときは、最後に選んだ地域だけを取得する。
        else if (millis() - lastRegionChange >= 600 && (!attempted || millis() - lastRequest >= waitMs)) {
            attempted = true;
            lastRequest = millis();
            requestRevision = configRevision;
            keyedRequest = *feedSettings.key && keyState != KeyState::Rejected;
            fetching = requestFeed(settings.region, keyedRequest ? feedSettings.key : "");
            setFeedStatus(fetching ? "Receiving ADS-B" : "Feed worker unavailable");
        }
    }
    renderTracker();
    delay(2);
}
