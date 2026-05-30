# SmartCatToilet

An ESP32-based sensor system that detects when a cat enters and exits a litter box, then reports session data to Home Assistant via webhooks.

## How It Works

The device uses two sensors and deep sleep to minimize power consumption:

1. **Reed switch** (GPIO 32) — detects the cat entering (e.g., mounted on a door flap)
2. **IR sensor** (GPIO 33) — detects the cat exiting

### Event Flow

```
[Deep Sleep] --(reed triggered)--> [Wake Up]
    --> Record entry time (no WiFi yet, to save power)
    --> Wait for exit (LED blinks while waiting)
    --> IR sensor triggered (or 5-min safety timeout)
    --> POST CAT_SESSION to Home Assistant (single WiFi connection)
    --> [Deep Sleep]
```

WiFi is only used once per session, at exit, to minimize battery drain.

Each session payload includes:
- `entered_at` — UNIX epoch timestamp
- `exited_at` — UNIX epoch timestamp
- `duration_sec` — time spent in the litter box

### Re-entry Cancellation

If the reed switch triggers **again** while a session is in progress, the
session is treated as invalid (e.g., the cat backed out and re-entered, or
sensor bounce). The device silently discards it — **no webhook is sent** — and
returns to deep sleep. Since the reed pin is already LOW at wake-up, it must
first release (go HIGH) before another LOW counts as a genuine re-entry. The
5-minute safety timeout still reports a session; only an explicit re-entry
discards it.

Timestamps are synced via NTP (`pool.ntp.org`, `time.nist.gov`, `time.google.com`).

## Hardware

| Component | Pin | Notes |
|-----------|-----|-------|
| Reed switch | GPIO 32 | Entry detection, active LOW, internal pull-up |
| IR sensor | GPIO 33 | Exit detection, active LOW, internal pull-up |
| Status LED | GPIO 2 | On-board LED, blinks while waiting for exit |

Any ESP32 dev board should work. The reed switch and IR break-beam sensor should pull their respective pins LOW when triggered.

## Setup

### Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or VSCode extension)
- An ESP32 dev board
- A Home Assistant instance with a [webhook automation](https://www.home-assistant.io/docs/automation/trigger/#webhook-trigger)

### 1. Clone the repo

```bash
git clone https://github.com/oguzgundogdu/SmartCatToilet.git
cd SmartCatToilet
```

### 2. Create `secrets.ini`

WiFi and Home Assistant credentials are not stored in the repository. Create a `secrets.ini` file in the project root:

```ini
[env:esp32dev]
build_flags =
  -DWIFI_SSID=\"YourWiFiName\"
  -DWIFI_PASSWORD=\"YourWiFiPassword\"
  -DHA_WEBHOOK_URL=\"http://YOUR_HA_IP:8123/api/webhook/your_webhook_id\"
```

This file is gitignored and will not be committed.

### 3. Build and upload

```bash
pio run -t upload
```

### 4. Monitor serial output

```bash
pio device monitor
```

## Home Assistant Integration

Create a webhook automation in Home Assistant to receive the events. The
firmware sends two event types:

**Session event** (one per visit, sent at exit):
```json
{
  "event": "CAT_SESSION",
  "entered_at": 1700000000,
  "exited_at": 1700000120,
  "duration_sec": 120
}
```

**Device started event** (on genuine power-on / reset only):
```json
{"event": "DEVICE_STARTED", "ts": 1700000000}
```

You can use these to track litter box usage frequency, duration, and timing in Home Assistant dashboards.

## Project Structure

```
SmartCatToilet/
├── src/
│   └── main.cpp              # Application code
├── include/                   # Header files (if needed)
├── lib/                       # Project-specific libraries
├── platformio.ini             # PlatformIO configuration
├── secrets.ini                # WiFi/HA credentials (gitignored)
├── sdkconfig                  # ESP-IDF SDK configuration
└── generate_compile_commands.py
```

## License

MIT
