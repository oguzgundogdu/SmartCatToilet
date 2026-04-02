#include "esp_sleep.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_wifi.h>
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

constexpr unsigned long MAX_AWAKE_MS = 5UL * 60 * 1000; // 5 min safety timeout
constexpr unsigned long BLINK_ON_MS = 100;              // Short flash
constexpr unsigned long BLINK_OFF_MS = 3000;            // Long pause between flashes

// Persist across deep sleep (best effort).
RTC_DATA_ATTR bool hasPendingEntry = false;
RTC_DATA_ATTR uint32_t enteredEpoch = 0;

bool waitingForExit = false;

bool ledOn = false;
unsigned long lastBlinkMs = 0;

unsigned long entryStartMs = 0;

static bool connectWiFiWithTimeoutMs(unsigned long timeoutMs) {
  WiFi.mode(WIFI_STA);
  esp_wifi_set_max_tx_power(52); // ~13 dBm (down from 20 dBm default)
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

static void recordEntryTimestamp() {
  // Just record millis for duration calculation.
  // WiFi + NTP + POST deferred to exit to save a full WiFi cycle.
  entryStartMs = millis();
  hasPendingEntry = true;
  Serial.println("Entry recorded (WiFi deferred to exit).");
}

// Single WiFi connection: sync time, send CAT_ENTERED + CAT_SESSION, disconnect.
static void sendSessionOnExit() {
  if (!connectWiFiWithTimeoutMs(10000)) {
    disconnectWiFi();
    return;
  }

  ensureTimeSynced(8000);

  const uint32_t exitEpoch = epochNowOrZero();
  const uint32_t durationSec =
      static_cast<uint32_t>((millis() - entryStartMs) / 1000UL);

  // Compute entered epoch by subtracting duration from exit epoch.
  const uint32_t entryEpoch =
      (exitEpoch > durationSec) ? (exitEpoch - durationSec) : 0;
  enteredEpoch = entryEpoch;

  // Send CAT_SESSION (includes entered_at, so no separate CAT_ENTERED needed).
  {
    const String payload = String("{\"event\":\"CAT_SESSION\",\"entered_at\":") +
                           String(entryEpoch) + String(",\"exited_at\":") +
                           String(exitEpoch) + String(",\"duration_sec\":") +
                           String(durationSec) + String("}");
    Serial.print("POST payload: ");
    Serial.println(payload);

    HTTPClient http;
    WiFiClient client;
    http.begin(client, HA_WEBHOOK_URL);
    http.addHeader("Content-Type", "application/json");
    http.POST(payload);
    http.end();
  }

  disconnectWiFi();
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
  setCpuFrequencyMhz(80);
  Serial.begin(115200);
  delay(100);
  Serial.println("System booted");

  pinMode((int)ENTRY_PIN, INPUT_PULLUP);
  pinMode((int)EXIT_PIN, INPUT_PULLUP);
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);

  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

  if (cause == ESP_SLEEP_WAKEUP_EXT0) {
    // Entry happened — just record timestamp, defer WiFi to exit.
    Serial.println("EVENT: CAT_ENTERED");
    recordEntryTimestamp();

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

  const unsigned long now = millis();

  // Safety timeout: go back to sleep if exit not detected.
  if (now - entryStartMs >= MAX_AWAKE_MS) {
    Serial.println("TIMEOUT: no exit detected, sleeping.");
    sendSessionOnExit();
    hasPendingEntry = false;
    enteredEpoch = 0;
    waitingForExit = false;
    goToDeepSleep();
  }

  // Exit sensor active LOW.
  if (digitalRead((int)EXIT_PIN) == LOW) {
    Serial.println("EVENT: CAT_EXITED");

    sendSessionOnExit();

    hasPendingEntry = false;
    enteredEpoch = 0;

    delay(300);
    waitingForExit = false;
    goToDeepSleep();
  }

  // Blink LED: short flash every ~3 seconds.
  const unsigned long interval = ledOn ? BLINK_ON_MS : BLINK_OFF_MS;

  if (now - lastBlinkMs >= interval) {
    ledOn = !ledOn;
    digitalWrite(STATUS_LED_PIN, ledOn ? HIGH : LOW);
    lastBlinkMs = now;
  }

  // Light sleep ~500ms between polls instead of busy-looping (~0.8mA vs ~20mA).
  esp_sleep_enable_timer_wakeup(500 * 1000ULL); // 500ms in microseconds
  esp_light_sleep_start();
}