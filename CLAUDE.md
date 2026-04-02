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

WiFi and HA credentials are in `secrets.ini` (gitignored), injected as build flags:
- `WIFI_SSID`, `WIFI_PASSWORD`, `HA_WEBHOOK_URL`

ESP-IDF SDK settings are in `sdkconfig` (referenced via `platformio.ini` cmake args).

## Architecture

**Firmware** (`src/main.cpp`): Single-file Arduino/ESP-IDF application. Flow:
1. Deep sleep → reed switch wakes ESP32 → records entry timestamp locally (no WiFi)
2. Light sleeps in ~500ms intervals polling IR exit sensor, with periodic LED flash
3. On exit (or 5-min safety timeout): single WiFi connection → NTP sync → POST `CAT_SESSION` to HA webhook → deep sleep

Only two webhook events exist: `CAT_SESSION` (per visit, with `entered_at`/`exited_at`/`duration_sec`) and `DEVICE_STARTED` (on power-on/reset only).

**Home Assistant** (`homeassistant/`):
- `automations.yaml` — webhook-triggered automation processing `CAT_SESSION` and `DEVICE_STARTED` events; daily midnight reset automation computing rolling averages (EMA 30/70)
- `helpers.yaml` — all `input_datetime`, `input_number`, `input_text`, and `counter` entities (prefixed `cat_toilet_`)
- `dashboard.yaml` — Lovelace dashboard with session history, 14-day trends, anomaly alerts

## Key Design Decisions

- All WiFi activity is deferred to exit to minimize power (one connection per session)
- Timestamps are computed by NTP-syncing at exit and back-calculating entry from `millis()` duration
- `RTC_DATA_ATTR` variables persist across deep sleep cycles
- TX power reduced to ~13 dBm; CPU runs at 80 MHz for power savings
- Entity naming convention in HA: `input_*.cat_toilet_*`, `counter.cat_toilet_*`
