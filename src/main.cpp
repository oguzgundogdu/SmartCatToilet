#include "esp_sleep.h"
#include <Arduino.h>

constexpr gpio_num_t ENTRY_PIN = GPIO_NUM_32; // Reed switch (entry), active LOW
constexpr gpio_num_t EXIT_PIN = GPIO_NUM_33;  // IR sensor (exit), active LOW
constexpr int STATUS_LED_PIN = 2;             // On-board LED (usually GPIO2)

// State flags
bool waitingForExit = false;

// LED blink state
bool ledOn = false;
unsigned long lastBlinkMs = 0;

void goToDeepSleep() {
  // Ensure LED is off before sleeping
  digitalWrite(STATUS_LED_PIN, LOW);

  // Wake up only when ENTRY (reed) goes LOW again
  esp_sleep_enable_ext0_wakeup(ENTRY_PIN, 0);

  Serial.println("Going to deep sleep...");
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("System booted");

  pinMode((int)ENTRY_PIN, INPUT_PULLUP);
  pinMode((int)EXIT_PIN, INPUT_PULLUP);
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);

  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

  if (cause == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("EVENT: CAT_ENTERED");
    Serial.println("Waiting for CAT_EXITED...");
    waitingForExit = true;
  } else {
    Serial.println("BOOT: power on / reset");
    goToDeepSleep();
  }
}

void loop() {
  if (!waitingForExit) {
    return;
  }

  // Blink LED while waiting for exit
  const unsigned long now = millis();
  const unsigned long interval = ledOn ? 200 : 800; // ON 200ms, OFF 800ms

  if (now - lastBlinkMs >= interval) {
    ledOn = !ledOn;
    digitalWrite(STATUS_LED_PIN, ledOn ? HIGH : LOW);
    lastBlinkMs = now;
  }

  // Check for exit event (IR sensor active LOW)
  if (digitalRead((int)EXIT_PIN) == LOW) {
    Serial.println("EVENT: CAT_EXITED");
    delay(300); // Simple debounce
    waitingForExit = false;
    goToDeepSleep();
  }
}