# wallbox_ble — ESPHome external component

An ESPHome custom `external_component` that talks BAPI (Wallbox's BLE
protocol) directly to a **Wallbox Pulsar MAX**, exposing charging power,
session energy, status, and start/stop/max-current control as native
ESPHome/Home Assistant entities — no MQTT broker, no cloud.

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
| `sensor` | `power` | Charging power (kW) |
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
2. Copy `example/wallbox.yaml`, set `wallbox_mac`, add your WiFi
   credentials to `secrets.yaml`.
3. If you've set a Bluetooth PIN on the charger (Wallbox app → Settings),
   add it as `pin:` under `wallbox_ble:` (via `!secret`, not committed in
   plain text).
4. `esphome run wallbox.yaml`.

## Validation status — read this first

I don't have a physical Pulsar MAX to test against, so be clear-eyed about
what's actually been checked here vs. what hasn't:

- ✅ **ESPHome config validation passes** (`esphome config wallbox.yaml` →
  `Configuration is valid!`) — the YAML schema, Python codegen, and every
  class/method name referenced between the `.py` files and the C++ headers
  are internally consistent.
- ✅ **C++ source generation succeeds** — ESPHome's codegen renders valid
  C++ from the config with no errors.
- ✅ **BLE API usage is grounded in real, current ESPHome source** — the
  `gattc_event_handler`/`register_for_notify`/`write_char_descr` sequence
  in `wallbox_ble.cpp` mirrors ESPHome's own `bedjet` component
  line-for-line in structure, not guessed from documentation.
- ✅ **Protocol constants are sourced from the upstream repo's own docs**
  (`docs/CHARGER_QUIRKS.md`, `wb_mqtt.cpp`'s discovery table) — the service/
  characteristic UUIDs, the `st`/`cp`/`en`/`cur` field names, the status
  enum, and the `w_cha` start/stop `par` values are not guesses.
- ❌ **Not yet compiled** (`esphome compile`) — I installed ESPHome and got
  as far as ESP-IDF toolchain download in this sandbox, but the PlatformIO
  package post-install step failed on an unrelated sandbox issue
  (`package-postinstall.py: command not found` — looks like a PATH/exec
  quirk in this environment, not a code problem). **Run `esphome compile
  wallbox.yaml` yourself as the next step** — that's the first real
  compiler check this code has had.
- ❌ **Not tested against real hardware** — the PIN handshake, notify
  framing, and field scaling are all correct *on paper* against the
  upstream project's own documentation, but nothing beats plugging it in.
  Expect a debugging pass on first boot.

## Known limitations / roadmap

- **Pulsar MAX only.** Plus/Copper/Quasar/Quasar2 use a different
  dual-characteristic BLE service and a different `w_cha` stop value
  (`par=0` pause, not `par=2` hard-stop) — see upstream's
  `docs/CHARGER_QUIRKS.md` for the full model matrix. Not implemented here.
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
- No dual-phase / three-phase current sensors (L1/L2/L3) yet, though
  `cur` (configured max) is exposed.

## Credits

Protocol reverse-engineering: [jagheterfredrik](https://github.com/jagheterfredrik)
(`wallbox-ble`, `wallbox-mqtt-bridge`). Firmware this rewrite is based on:
[botts7/esp32-wallbox](https://github.com/botts7/esp32-wallbox). Not
affiliated with, endorsed by, or connected to Wallbox Chargers SL.
