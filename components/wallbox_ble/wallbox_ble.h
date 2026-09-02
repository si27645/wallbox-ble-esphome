#pragma once

#ifdef USE_ESP32

#include "esphome/core/component.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/select/select.h"

#include "bapi.h"

#include <esp_gattc_api.h>
#include <string>

namespace esphome {
namespace wallbox_ble {

namespace espbt = esphome::esp32_ble_tracker;

// Pulsar MAX default single-characteristic BAPI service (u-blox BLE
// module). Write and notify happen on the same characteristic.
// Source: si27645/esp32-wallbox `wb_config.cpp` DEFAULT_BLE_SVC/DEFAULT_BLE_CHR.
// Plus/Copper/Quasar use a *different* dual-characteristic service and are
// NOT supported by this component yet — see README.
static const espbt::ESPBTUUID WALLBOX_SERVICE_UUID =
    espbt::ESPBTUUID::from_raw("2456e1b9-26e2-8f83-e744-f34f01e9d701");
static const espbt::ESPBTUUID WALLBOX_CHAR_UUID =
    espbt::ESPBTUUID::from_raw("2456e1b9-26e2-8f83-e744-f34f01e9d703");

// Original (pre-BGX, no-WiFi) Pulsar — a Zentri AMS module running the
// TruConnect serial-over-BLE profile, not u-blox (MAX) or BGX13P (Plus).
// Separate write (RX), notify (TX), and mode characteristics, and the mode
// char must be switched to STREAM_MODE (0x01) before BAPI bytes pass
// through. Source: si27645/esp32-wallbox `wb_ble.cpp` (Zentri detection/
// handling around line 556) and `docs/CHARGER_QUIRKS.md`.
static const espbt::ESPBTUUID ZENTRI_SERVICE_UUID =
    espbt::ESPBTUUID::from_raw("175f8f23-a570-49bd-9627-815a6a27de2a");
static const espbt::ESPBTUUID ZENTRI_WRITE_CHAR_UUID =
    espbt::ESPBTUUID::from_raw("1cce1ea8-bd34-4813-a00a-c76e028fadcb");
static const espbt::ESPBTUUID ZENTRI_NOTIFY_CHAR_UUID =
    espbt::ESPBTUUID::from_raw("cacc07ff-ffff-4c48-8fae-a9ef71b75e26");
static const espbt::ESPBTUUID ZENTRI_MODE_CHAR_UUID =
    espbt::ESPBTUUID::from_raw("20b9794f-da1a-4d14-8014-a0fb9cefb2f7");

// Charger status codes (r_dat.st), Pulsar MAX / Plus table.
// Source: si27645/esp32-wallbox `wb_mqtt.cpp` discovery table (entry 8).
enum WallboxStatus : int {
  STATUS_READY = 0,
  STATUS_CHARGING = 1,
  STATUS_WAITING_FOR_CAR = 2,
  STATUS_WAITING_FOR_SCHEDULE = 3,
  STATUS_PAUSED = 4,
  STATUS_CHARGE_COMPLETE = 5,
  STATUS_LOCKED = 6,
  STATUS_ERROR = 7,
  STATUS_WAITING_FOR_CURRENT_ALLOCATION = 8,
  STATUS_POWER_SHARING_NOT_CONFIGURED = 9,
  STATUS_QUEUED_POWER_BOOST = 10,
  STATUS_DISCHARGING = 11,
  STATUS_WAITING_FOR_MID_AUTH = 12,
  STATUS_MID_SAFETY_MARGIN_EXCEEDED = 13,
  STATUS_OCPP_UNAVAILABLE = 14,
  STATUS_OCPP_FINISHING = 15,
  STATUS_OCPP_RESERVED = 16,
  STATUS_UPDATING = 17,
  STATUS_QUEUED_ECO_SMART = 18,
};

const char *wallbox_status_to_string(int st);
// A car is physically plugged in for these status codes (MAX/Plus table).
// Status 6 (Locked) is ambiguous in the local protocol on its own — the
// upstream project cross-checks r_sta.charger_status to disambiguate;
// this component doesn't poll r_sta yet, so a locked idle charger may
// under-report car-connected. See README roadmap.
bool wallbox_status_car_connected(int st);

// Eco-Smart mode (g_ecos/s_ecos `esm`, gated by `ese`). Source:
// si27645/esp32-wallbox `wb_mqtt.cpp` (kEcoOptions) and `wb_web.cpp`
// (g_ecos display table). The displayed mode is 0 whenever `ese` is
// false, regardless of the last `esm` value — see wallbox_eco_mode_of().
enum WallboxEcoMode : int {
  ECO_MODE_DISABLED = 0,
  ECO_MODE_FULL_GREEN = 1,   // solar only
  ECO_MODE_SOLAR_GRID = 2,   // solar + grid top-up
};

const char *wallbox_eco_mode_to_string(int mode);
// -1 if `option` isn't one of wallbox_eco_mode_to_string()'s strings.
int wallbox_eco_mode_from_string(const std::string &option);
// g_ecos reports {ese, esm, esp} separately; the *displayed* mode is 0
// (Disabled) whenever ese is false, whatever esm last was — mirrors
// esp32-wallbox `wb_ble.cpp`: merged["eco_mode"] = ese ? esm : 0.
inline int wallbox_eco_mode_of(bool ese, int esm) { return ese ? esm : 0; }

class WallboxBleHub : public ble_client::BLEClientNode, public PollingComponent {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::BLUETOOTH; }

  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                            esp_ble_gattc_cb_param_t *param) override;

  void set_pin(const std::string &pin) { this->pin_ = pin; }

  // --- Entity registration (direct-setter pattern) ---
  void set_car_connected_binary_sensor(binary_sensor::BinarySensor *s) { this->car_connected_sensor_ = s; }
  void set_charging_binary_sensor(binary_sensor::BinarySensor *s) { this->charging_sensor_ = s; }
  void set_power_sensor(sensor::Sensor *s) { this->power_sensor_ = s; }
  void set_energy_sensor(sensor::Sensor *s) { this->energy_sensor_ = s; }
  void set_max_current_sensor(sensor::Sensor *s) { this->max_current_sensor_ = s; }
  void set_status_text_sensor(text_sensor::TextSensor *s) { this->status_text_sensor_ = s; }
  void set_last_error_text_sensor(text_sensor::TextSensor *s) { this->last_error_text_sensor_ = s; }
  // Setting this also enables periodic g_ecos polling in update() — see
  // there. Left null (the default), Eco-Smart isn't polled at all, so
  // configs that don't use it don't pay the extra BLE round-trip.
  void set_eco_mode_select(select::Select *s) { this->eco_mode_select_ = s; }

  // --- Controls, called by the switch/number/select platforms ---
  void start_charging();
  void stop_charging();
  void set_max_current(uint8_t amps);
  void set_eco_mode(int mode);

  // Last known values — read by switch/number for optimistic state after
  // a write, ahead of the next poll confirming it.
  bool last_charging() const { return this->last_status_ == STATUS_CHARGING; }
  uint8_t last_max_current() const { return this->last_cur_; }
  bool is_connected() const { return this->node_state == espbt::ClientState::ESTABLISHED; }

 protected:
  bool discover_characteristic_();
  bool register_for_notify_();
  bool write_notify_config_descriptor_(bool enable);
  void write_bapi_(const char *met, const std::string &par = "null");
  // Kicks off the BAPI PIN handshake once the connection is established.
  // MAX firmware: `read_pin` returns the configured PIN's metadata (or
  // nothing, on firmware without BAPI PIN support at all); if a PIN is
  // set, `set_pin` must succeed before other commands are honoured.
  void begin_authenticate_();
  void handle_response_(const std::string &json);
  void report_error_(const std::string &message);

  // Write handle for BAPI commands. Single-char (MAX) mode: also the notify
  // handle. Zentri (dual-char) mode: the RX char; notify_handle_ is the
  // separate TX char.
  uint16_t char_handle_{0};
  uint16_t notify_handle_{0};
  uint16_t config_descr_handle_{0};
  // Zentri TruConnect only: mode characteristic, switched to STREAM_MODE
  // once notifications are subscribed so BAPI bytes pass through.
  uint16_t mode_handle_{0};
  bool zentri_{false};
  // Current ATT MTU, tracked from ESP_GATTC_CFG_MTU_EVT. Zentri's AMS
  // module caps this at the 23-byte default and never negotiates up, so
  // BAPI frames (~41 B) must be fragmented into (mtu-3) chunks on write.
  uint16_t mtu_{23};
  // Set by update() when an eco-mode poll is wanted; consumed by
  // handle_response_() once r_dat's response comes back, so g_ecos is
  // never in flight at the same time as another request — see update().
  bool eco_poll_due_{false};

  bapi::ResponseParser parser_;
  std::string pin_;
  bool pin_required_{false};
  bool authenticated_{false};
  int next_id_{1};
  // Two-step PIN handshake tracked by request id, since responses arrive
  // async via notify rather than as a return value.
  int pending_read_pin_id_{-1};
  int pending_set_pin_id_{-1};
  int pending_eco_poll_id_{-1};

  int last_status_{-1};
  uint8_t last_cur_{0};

  binary_sensor::BinarySensor *car_connected_sensor_{nullptr};
  binary_sensor::BinarySensor *charging_sensor_{nullptr};
  sensor::Sensor *power_sensor_{nullptr};
  sensor::Sensor *energy_sensor_{nullptr};
  sensor::Sensor *max_current_sensor_{nullptr};
  text_sensor::TextSensor *status_text_sensor_{nullptr};
  text_sensor::TextSensor *last_error_text_sensor_{nullptr};
  select::Select *eco_mode_select_{nullptr};
};

}  // namespace wallbox_ble
}  // namespace esphome

#endif
