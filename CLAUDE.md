# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-based cat litter box sensor that detects entry (reed switch, GPIO 32) and exit (IR sensor, GPIO 33), then reports session data to Home Assistant via webhook. The device uses deep sleep and light sleep to maximize battery life on 4×AA batteries.

## Build Commands

```bash
pio run                  # Build firmware
pio run -t upload        # Build and flash to ESP32
pio device monitor       # Serial monitor (115200 baud)
```

## Configuration

WiFi and HA credentials are in `secrets.ini` (gitignored), injected as build flags with escaped quotes:
```ini
[env:esp32dev]
build_flags =
  -DWIFI_SSID=\"MyNetwork\"
  -DWIFI_PASSWORD=\"MyPassword\"
  -DHA_WEBHOOK_URL=\"http://HA_IP:8123/api/webhook/cat_toilet\"
```

ESP-IDF SDK settings are in `sdkconfig` (referenced via `platformio.ini` cmake args). IDE support via `.clangd` which reads the PlatformIO compilation database and injects placeholder build flags.

## Architecture

**Firmware** (`src/main.cpp`): Single-file Arduino/ESP-IDF application. Flow:
1. Deep sleep → reed switch wakes ESP32 (ext0 on GPIO 32, active LOW) → records `millis()` as entry reference (no WiFi)
2. Light sleeps in ~500ms intervals polling IR exit sensor (GPIO 33, active LOW), with periodic LED blink
3. On exit (or 5-min safety timeout): single WiFi connection → NTP sync → compute `entered_at` by subtracting `millis()` duration from current epoch → POST `CAT_SESSION` to HA → deep sleep

Only two webhook events: `CAT_SESSION` (per visit: `entered_at`, `exited_at`, `duration_sec` as UNIX epochs/seconds) and `DEVICE_STARTED` (on power-on/reset only, with `ts`).

**Home Assistant** (`homeassistant/`):
- `automations.yaml` — two automations:
  1. **Cat Toilet** (webhook `cat_toilet`): processes `CAT_SESSION` → stores timestamps/duration, increments daily counter, tracks min/max duration, appends to rotating weekday session log, sends push notification, fires anomaly alert if visits exceed EMA average + buffer
  2. **Reset Cat Toilet Daily Stats** (midnight trigger): appends day summary to 14-day history, updates EMA rolling averages (30% today / 70% history), resets daily counter/durations, clears the new day's session slot
- `helpers.yaml` — all `input_datetime`, `input_number`, `input_text`, and `counter` entities (prefixed `cat_toilet_`)
- `dashboard.yaml` — Lovelace dashboard with session browser (date picker + per-day table), stats cards, history graphs, 14-day history table

## HA Data Formats

These compact string formats are used in `input_text` entities (255-char max) and parsed by Jinja2 templates in both automations and dashboard:

- **Session log** (7 weekday slots, `input_text.cat_toilet_sessions_{weekday}`): `HHMM:duration,HHMM:duration,...` — each entry is entered time (24h, no colon) and duration in seconds, comma-separated, truncated to last 255 chars
- **14-day history** (`input_text.cat_toilet_history`): `MMDD:visits:shortest:longest,MMDD:...` — newest first, kept to 14 entries

When modifying templates that read/write these formats, keep both the automation (write side) and dashboard (read side) in sync.

## Key Design Decisions

- All WiFi activity is deferred to exit to minimize power (one connection per session)
- Timestamps are computed by NTP-syncing at exit and back-calculating entry from `millis()` duration
- `RTC_DATA_ATTR` variables persist across deep sleep cycles
- TX power reduced to ~13 dBm; CPU runs at 80 MHz for power savings
- Entity naming convention in HA: `input_*.cat_toilet_*`, `counter.cat_toilet_*`
- Anomaly threshold: `avg + max(avg × 0.5, 5)` — adaptive buffer that avoids false positives at low visit counts
- After editing HA YAML files, push changes to the live Home Assistant instance (automations via REST API, dashboards via WebSocket)
