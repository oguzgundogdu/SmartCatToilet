#include "esp_sleep.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <time.h>

// ====== USER CONFIG (defined via secrets.ini build_flags) ======
#ifndef WIFI_SSID
#error "WIFI_SSID not defined. Create secrets.ini with build_flags."
#endif
#ifndef WIFI_PASSWORD
#error "WIFI_PASSWORD not defined. Create secrets.ini with build_flags."
#endif
#ifndef HA_WEBHOOK_URL
#error "HA_WEBHOOK_URL not defined. Create secrets.ini with build_flags."
#endif
// ==============================================================

constexpr gpio_num_t ENTRY_PIN = GPIO_NUM_32; // Reed switch (entry), active LOW
constexpr gpio_num_t EXIT_PIN = GPIO_NUM_33;  // IR sensor (exit), active LOW
constexpr int STATUS_LED_PIN = 2;             // On-board LED

// Persist across deep sleep (best effort).
RTC_DATA_ATTR bool hasPendingEntry = false;
RTC_DATA_ATTR uint32_t enteredEpoch = 0;

bool waitingForExit = false;

bool ledOn = false;
unsigned long lastBlinkMs = 0;

unsigned long entryStartMs = 0;

static bool connectWiFiWithTimeoutMs(unsigned long timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to WiFi");
  const unsigned long startAttemptTime = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - startAttemptTime < timeoutMs) {
    Serial.print(".");
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected");
    return true;
  }

  Serial.println("\nWiFi connection failed");
  return false;
}

static void disconnectWiFi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

static bool ensureTimeSynced(unsigned long timeoutMs) {
  // If time is not set, SNTP will return a very small epoch.
  time_t now = time(nullptr);
  if (now > 1700000000) {
    return true;
  }

  // Start SNTP (UTC). Home Assistant can convert using timestamp_local.
  configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");

  const unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    now = time(nullptr);
    if (now > 1700000000) {
      return true;
    }
    delay(200);
  }

  return false;
}

static uint32_t epochNowOrZero() {
  time_t now = time(nullptr);
  if (now > 1700000000) {
    return static_cast<uint32_t>(now);
  }
  return 0;
}

static bool postJsonToHomeAssistant(const String &jsonPayload,
                                    unsigned long wifiTimeoutMs,
                                    unsigned long timeSyncTimeoutMs) {
  if (!connectWiFiWithTimeoutMs(wifiTimeoutMs)) {
    disconnectWiFi();
    return false;
  }

  // Best effort time sync (needed for epoch payloads).
  if (!ensureTimeSynced(timeSyncTimeoutMs)) {
    Serial.println("Time sync failed (best effort).");
  } else {
    Serial.println("Time synced.");
  }

  HTTPClient http;
  WiFiClient client;

  http.begin(client, HA_WEBHOOK_URL);
  http.addHeader("Content-Type", "application/json");

  const int httpResponseCode = http.POST(jsonPayload);

  Serial.print("HA response code: ");
  Serial.println(httpResponseCode);

  http.end();
  disconnectWiFi();

  return httpResponseCode > 0;
}

static void sendEnteredBestEffort() {
  const uint32_t ts = epochNowOrZero();
  if (ts > 0) {
    enteredEpoch = ts;
    hasPendingEntry = true;
  }
  const String payload =
      String("{\"event\":\"CAT_ENTERED\",\"ts\":") + String(ts) + String("}");

  // Entry event is best-effort: short WiFi timeout and short SNTP wait.
  Serial.print("POST payload: ");
  Serial.println(payload);
  postJsonToHomeAssistant(payload, /*wifiTimeoutMs=*/3500,
                          /*timeSyncTimeoutMs=*/2500);
}

static void sendSessionReliable(uint32_t exitedAtEpoch) {
  // If we have a valid enteredEpoch, use it. Otherwise fall back to millis
  // duration.
  uint32_t enteredAt = enteredEpoch;
  uint32_t durationSec = 0;

  if (enteredAt > 0 && exitedAtEpoch > 0 && exitedAtEpoch >= enteredAt) {
    durationSec = exitedAtEpoch - enteredAt;
  } else {
    durationSec = static_cast<uint32_t>((millis() - entryStartMs) / 1000UL);

    // If we managed to capture enteredEpoch (e.g. during the entry POST), use
    // it.
    if (enteredAt > 0 && exitedAtEpoch > 0 && exitedAtEpoch >= enteredAt) {
      durationSec = exitedAtEpoch - enteredAt;
    } else {
      enteredAt = (enteredEpoch > 0) ? enteredEpoch : 0;
    }
  }

  const String payload = String("{\"event\":\"CAT_SESSION\",\"entered_at\":") +
                         String(enteredAt) + String(",\"exited_at\":") +
                         String(exitedAtEpoch) + String(",\"duration_sec\":") +
                         String(durationSec) + String("}");

  // Session event should be reliable: longer WiFi timeout and longer SNTP wait.
  Serial.print("POST payload: ");
  Serial.println(payload);
  postJsonToHomeAssistant(payload, /*wifiTimeoutMs=*/10000,
                          /*timeSyncTimeoutMs=*/8000);
}

static void sendDeviceStarted() {
  const uint32_t ts = epochNowOrZero();
  const String payload =
      String("{\"event\":\"DEVICE_STARTED\",\"ts\":") + String(ts) + String("}");

  Serial.print("POST payload: ");
  Serial.println(payload);
  postJsonToHomeAssistant(payload, /*wifiTimeoutMs=*/5000,
                          /*timeSyncTimeoutMs=*/4000);
}

static void goToDeepSleep() {
  digitalWrite(STATUS_LED_PIN, LOW);
  // Wake up when ENTRY_PIN goes LOW (reed triggered).
  esp_sleep_enable_ext0_wakeup(ENTRY_PIN, 0);
  Serial.println("Going to deep sleep...");
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  delay(1200);
  Serial.println("System booted");

  pinMode((int)ENTRY_PIN, INPUT_PULLUP);
  pinMode((int)EXIT_PIN, INPUT_PULLUP);
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);

  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

  if (cause == ESP_SLEEP_WAKEUP_EXT0) {
    // Entry happened.
    Serial.println("EVENT: CAT_ENTERED");
    entryStartMs = millis();

    // Send entered notification (best-effort).
    sendEnteredBestEffort();

    Serial.println("Waiting for CAT_EXITED...");
    waitingForExit = true;
  } else {
    Serial.println("BOOT: power on / reset");
    sendDeviceStarted();
    goToDeepSleep();
  }
}

void loop() {
  if (!waitingForExit) {
    return;
  }

  // Blink LED while waiting for exit
  const unsigned long now = millis();
  const unsigned long interval = ledOn ? 200 : 800;

  if (now - lastBlinkMs >= interval) {
    ledOn = !ledOn;
    digitalWrite(STATUS_LED_PIN, ledOn ? HIGH : LOW);
    lastBlinkMs = now;
  }

  // Exit sensor active LOW.
  if (digitalRead((int)EXIT_PIN) == LOW) {
    Serial.println("EVENT: CAT_EXITED");

    // On exit, get a reliable epoch time + send session.
    uint32_t exitEpoch = 0;

    if (connectWiFiWithTimeoutMs(10000)) {
      if (ensureTimeSynced(8000)) {
        exitEpoch = epochNowOrZero();
      }
      disconnectWiFi();
    }

    // If we still don't have an epoch, send 0 for exited_at and rely on
    // duration.
    sendSessionReliable(exitEpoch);

    // Clear state for next session.
    hasPendingEntry = false;
    enteredEpoch = 0;

    delay(300);
    waitingForExit = false;
    goToDeepSleep();
  }
}