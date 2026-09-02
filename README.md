# wallbox_ble — ESPHome external component

An ESPHome custom `external_component` that talks BAPI (Wallbox's BLE
protocol) directly to a **Wallbox Pulsar MAX**, exposing charging power,
session energy, status, and start/stop/max-current control as native
ESPHome/Home Assistant entities — no MQTT broker, no cloud.

Two BLE service variants are supported, auto-detected at connect time:
the u-blox single-characteristic service most Pulsar MAX units use, and
the Zentri TruConnect dual-characteristic service used by the original
(pre-BGX, no-WiFi) Pulsar. Plus/Copper/Quasar's BGX-based service is not
supported yet.

This is a **from-scratch rewrite** of the BLE core of
[si27645/esp32-wallbox](https://github.com/si27645/esp32-wallbox) (a fork of
[botts7/esp32-wallbox](https://github.com/botts7/esp32-wallbox)) against
ESPHome's own `ble_client`/`esp32_ble_client` stack, not a port — that
firmware uses NimBLE-Arduino directly, which can't safely share the radio
with ESPHome's own BLE stack. The BAPI wire-framing logic (`bapi.h/.cpp`) is
ported near-verbatim (MIT); everything BLE-connection-related is new code
written against ESPHome's `ble_client::BLEClientNode` API, modeled closely
on ESPHome's own `bedjet` component (another single-service BLE
write+notify device).

See [`docs/VALIDATION.md`](#validation-status-read-this-first) below before
flashing anything to a real charger.

## What this replaces vs. what it doesn't

The upstream firmware is a full standalone appliance: BLE gateway *and*
self-hosted web dashboard *and* MQTT+HA-discovery *and* OTA/WiFi/config
web UI — about 17,600 lines of C++. Moving to ESPHome trades the standalone
dashboard for native HA integration:

| Upstream does it itself | This component | Notes |
|---|---|---|
| WiFi reconnect, captive portal, OTA, web config UI | **Not needed** | ESPHome's `wifi:`/`captive_portal:`/`ota:` do this for free |
| MQTT + HA auto-discovery (~1,600 lines) | **Not needed** | ESPHome's native HA API integration replaces it entirely |
| Self-hosted dashboard, weekly heatmap, CSV export (~6,400 lines) | **Gone, no equivalent** | Lives in Home Assistant (Lovelace + history) instead |
| BLE connect/auth/notify (`wb_ble.*`, ~2,500 lines) | **Rewritten** (`wallbox_ble.h/.cpp`) | Against `esp32_ble_client`, not NimBLE-Arduino |
| BAPI framing/parsing (`bapi.*`, ~300 lines) | **Ported near-verbatim** | Pure protocol logic, framework-independent |

## Entities

| Platform | Key | Description |
|---|---|---|
| `sensor` | `power` | Charging power (kW) — from `r_dat.cp` where the firmware reports it; on firmware that instead reports per-phase current (`L1/L2/L3`, no `cp`), estimated as `sum(phases) × 230 V`. See "Known limitations" below. |
| `sensor` | `energy` | Session energy (kWh) |
| `sensor` | `max_current` | Charger-reported max current (A) |
| `binary_sensor` | `car_connected` | A car is plugged in |
| `binary_sensor` | `charging` | Actively charging |
| `text_sensor` | `status` | Human-readable charger status (Ready/Charging/Paused/Error/...) |
| `text_sensor` | `last_error` | Last BAPI error message, for diagnostics |
| `switch` | `charging` | Start/stop charging |
| `number` | `max_current` | Set max charging current, 6–32 A |

See [`example/wallbox.yaml`](example/wallbox.yaml) for a full working config.

## Setup

1. Find your charger's BLE MAC address (nRF Connect app, or
   `bluetoothctl scan on` near the charger).
2. Copy `example/wallbox.yaml`, set `wallbox_mac`, `esp32.board` (defaults
   to a generic `esp32dev`; change it to match your actual dev board —
   e.g. `esp32-s3-devkitc-1` for an S3), and add your WiFi credentials to
   `secrets.yaml`. If your WiFi network is hidden (doesn't show up in a
   scan), keep `fast_connect: true` under `wifi:`; ESPHome's default
   scan-and-match otherwise never finds it.
3. If you've set a Bluetooth PIN on the charger (Wallbox app → Settings),
   add it as `pin:` under `wallbox_ble:` (via `!secret`, not committed in
   plain text).
4. `esphome run wallbox.yaml`.

## Validation status — read this first

- ✅ **Compiles cleanly** (`esphome compile wallbox.yaml`, esp32dev + esp-idf)
  — RAM 15.5%, Flash 74.3%. If you hit `AttributeError: '_SpecialForm'
  object has no attribute 'replace'` during the ESP-IDF component-manager
  step, it's a Python 3.9 / too-new-`pydantic` incompatibility, not a code
  problem — fix with
  `~/.platformio/penv/.espidf-5.1.5/bin/python -m pip install "pydantic<2.11"`
  and re-run.
- ✅ **Tested live against a real charger** (2026-09-02/03, generic ESP32
  DevKitC + an original/no-WiFi Pulsar on the Zentri TruConnect service).
  Confirmed working end-to-end: BLE connect, service auto-detection,
  STREAM_MODE switch, PIN-handshake fallback (this firmware doesn't
  implement `read_pin` at all — see below), and live polling — Charger
  Status, Charging/Car Connected binary sensors, Session Energy, Max
  Current, and the power-estimate fallback all updated correctly in real
  time while the car was actually charging.
- ✅ **Start/stop charging switch tested live** (2026-09-03, over OTA) —
  round-tripped stop → `st=4` (Paused, `L1` current dropped to 0, real)
  → start → resumed charging, all reflected correctly end-to-end.
- ⚠️ **Not yet tested**: the u-blox single-characteristic path (only the
  Zentri path has been hardware-verified so far), and the set-max-current
  number. Treat those as correct-on-paper until confirmed.
- **Fix found during hardware testing**: this charger's firmware answers
  `read_pin` with a BAPI error (`{"error":{"code":4}}`, "feature not
  supported") rather than an empty response. Upstream `esp32-wallbox`
  treats that the same as "no PIN set" (`doc["r"]["pin"]` comes back null
  either way); `handle_response_()` here now does too — see
  `wallbox_ble.cpp`'s PIN-handshake block for the reasoning.
- **Second fix found during hardware testing**: stopping charging on the
  Zentri path with `par=2` ("hard stop", correct for MAX/u-blox) came back
  `{"error":{"code":5}}`. Zentri needs `par=0` ("pause") instead, like
  Plus/Copper/Quasar — this component now branches on the already-detected
  service variant. (Mirrors a known bug in `esp32-wallbox` itself:
  `docs/CHARGER_QUIRKS.md` #10, "Zentri stop-par chosen by config family,
  not runtime `_isZentri`".)

## Known limitations / roadmap

- **Pulsar (MAX and original/Zentri) only.** Plus/Copper/Quasar/Quasar2 use
  a different, BGX-based dual-characteristic BLE service and a different
  `w_cha` stop value (`par=0` pause, not `par=2` hard-stop) — see
  upstream's `docs/CHARGER_QUIRKS.md` for the full model matrix. Not
  implemented here.
- **Charging Power is estimated, not metered, on firmware without `cp`.**
  Confirmed on the Zentri-path charger tested: `r_dat` reports per-phase
  current (`L1/L2/L3`, deciamps) instead of a power field. The fallback
  computes `sum(phases)/10 × 230 V` — a reasonable approximation for a
  dashboard, not accurate enough for billing, and wrong if your mains
  voltage isn't ~230 V. No dedicated L1/L2/L3 current sensors yet either,
  though `cur` (configured max) is exposed.
- **set-max-current (`w_mxI`) is untested** — the charging switch has been
  verified live (see Validation status above); the max-current number
  hasn't been exercised on real hardware yet.
- **No lock/unlock, reboot, schedules, Eco-Smart/solar mode, or Halo LED
  control.** The BAPI method names for all of these are already in
  `bapi.h` as a starting point (`MET_LOCK`, `MET_SET_ECO_SMART`, etc. — see
  the fuller list in the upstream repo's `include/bapi.h`), they're just
  not wired to entities yet.
- **`car_connected` under-reports when status is `Locked` (code 6).** The
  upstream project disambiguates this by cross-checking `r_sta.charger_status`,
  which this component doesn't poll yet.
- **No async command-response matching for writes** — writes are
  fire-and-forget; state is optimistic until the next poll. Fine for a
  personal charger, would need work for anything more demanding.

## Credits

Protocol reverse-engineering: [jagheterfredrik](https://github.com/jagheterfredrik)
(`wallbox-ble`, `wallbox-mqtt-bridge`). Firmware this rewrite is based on:
[botts7/esp32-wallbox](https://github.com/botts7/esp32-wallbox). Not
affiliated with, endorsed by, or connected to Wallbox Chargers SL.
