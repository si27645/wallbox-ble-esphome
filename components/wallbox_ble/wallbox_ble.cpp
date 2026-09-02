#ifdef USE_ESP32

#include "wallbox_ble.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/json/json_util.h"

#include <algorithm>

namespace esphome {
namespace wallbox_ble {

static const char *const TAG = "wallbox_ble";

// Some firmware (confirmed on the original/Zentri Pulsar) omits r_dat's "cp"
// (charging power) field entirely and reports per-phase current instead
// (L1/L2/L3, in deciamps — e.g. 58 = 5.8 A). There's no voltage reading in
// this response, so approximate power as V * sum(phase currents), assuming
// 230 V single/split-phase mains. This is a rough estimate, not a metered
// reading — good enough for automations/dashboards, not billing.
static constexpr float ASSUMED_MAINS_VOLTAGE = 230.0f;

const char *wallbox_status_to_string(int st) {
  switch (st) {
    case STATUS_READY: return "Ready";
    case STATUS_CHARGING: return "Charging";
    case STATUS_WAITING_FOR_CAR: return "Waiting for Car";
    case STATUS_WAITING_FOR_SCHEDULE: return "Waiting for Schedule";
    case STATUS_PAUSED: return "Paused";
    case STATUS_CHARGE_COMPLETE: return "Charge Complete";
    case STATUS_LOCKED: return "Locked";
    case STATUS_ERROR: return "Error";
    case STATUS_WAITING_FOR_CURRENT_ALLOCATION: return "Waiting for Current Allocation";
    case STATUS_POWER_SHARING_NOT_CONFIGURED: return "Power Sharing Not Configured";
    case STATUS_QUEUED_POWER_BOOST: return "Queued (Power Boost)";
    case STATUS_DISCHARGING: return "Discharging";
    case STATUS_WAITING_FOR_MID_AUTH: return "Waiting for MID Auth";
    case STATUS_MID_SAFETY_MARGIN_EXCEEDED: return "MID Safety Margin Exceeded";
    case STATUS_OCPP_UNAVAILABLE: return "OCPP Unavailable";
    case STATUS_OCPP_FINISHING: return "OCPP Finishing";
    case STATUS_OCPP_RESERVED: return "OCPP Reserved";
    case STATUS_UPDATING: return "Updating";
    case STATUS_QUEUED_ECO_SMART: return "Queued (Eco-Smart)";
    default: return "Unknown";
  }
}

bool wallbox_status_car_connected(int st) {
  switch (st) {
    case STATUS_CHARGING:
    case STATUS_WAITING_FOR_CAR:
    case STATUS_WAITING_FOR_SCHEDULE:
    case STATUS_PAUSED:
    case STATUS_CHARGE_COMPLETE:
    case STATUS_WAITING_FOR_CURRENT_ALLOCATION:
    case STATUS_QUEUED_POWER_BOOST:
    case STATUS_DISCHARGING:
    case STATUS_WAITING_FOR_MID_AUTH:
    case STATUS_MID_SAFETY_MARGIN_EXCEEDED:
    case STATUS_QUEUED_ECO_SMART:
      return true;
    default:
      return false;
  }
}

const char *wallbox_eco_mode_to_string(int mode) {
  switch (mode) {
    case ECO_MODE_DISABLED: return "Disabled";
    case ECO_MODE_FULL_GREEN: return "Full Green";
    case ECO_MODE_SOLAR_GRID: return "Solar + Grid";
    default: return "Unknown";
  }
}

int wallbox_eco_mode_from_string(const std::string &option) {
  if (option == "Disabled") return ECO_MODE_DISABLED;
  if (option == "Full Green") return ECO_MODE_FULL_GREEN;
  if (option == "Solar + Grid") return ECO_MODE_SOLAR_GRID;
  return -1;
}

void WallboxBleHub::setup() {
  this->parser_.reset();
}

void WallboxBleHub::dump_config() {
  ESP_LOGCONFIG(TAG, "Wallbox BLE Hub:");
  ESP_LOGCONFIG(TAG, "  MAC address: %s", this->parent()->address_str().c_str());
  ESP_LOGCONFIG(TAG, "  PIN configured: %s", this->pin_.empty() ? "no" : "yes");
  // BLE service variant (u-blox single-char vs Zentri dual-char) isn't known
  // until the first connection completes discovery — see the "Zentri
  // TruConnect peripheral detected" / characteristic-not-found log lines.
}

void WallboxBleHub::update() {
  if (!this->is_connected() || !this->authenticated_) return;
  this->write_bapi_(bapi::MET_GET_STATUS);
  // g_ecos is fired as a follow-up from handle_response_() once r_dat's
  // response arrives — not here. Firing both writes back-to-back let their
  // responses land in the same BLE notify burst; ResponseParser::feed()
  // returns as soon as one top-level JSON object completes and drops
  // whatever's left in that chunk (the start of the next response),
  // silently losing it. Never have two BAPI requests in flight at once.
  if (this->eco_mode_select_ != nullptr) {
    this->eco_poll_due_ = true;
  }
}

// ---- GATT plumbing -------------------------------------------------------

bool WallboxBleHub::discover_characteristic_() {
  // Try the Pulsar MAX single-characteristic (u-blox) service first.
  auto *chr = this->parent()->get_characteristic(WALLBOX_SERVICE_UUID, WALLBOX_CHAR_UUID);
  if (chr != nullptr) {
    this->char_handle_ = chr->handle;
    this->notify_handle_ = chr->handle;
    this->zentri_ = false;
  } else {
    // Fall back to the Zentri TruConnect (original, no-WiFi Pulsar)
    // dual-characteristic service.
    auto *write_chr = this->parent()->get_characteristic(ZENTRI_SERVICE_UUID, ZENTRI_WRITE_CHAR_UUID);
    auto *notify_chr = this->parent()->get_characteristic(ZENTRI_SERVICE_UUID, ZENTRI_NOTIFY_CHAR_UUID);
    if (write_chr == nullptr || notify_chr == nullptr) {
      ESP_LOGW(TAG, "BAPI characteristic not found — neither the Pulsar MAX (u-blox) nor the original "
                    "Pulsar (Zentri TruConnect) service is present. Plus/Copper/Quasar use a different "
                    "BGX-based service, not yet supported by this component.");
      return false;
    }
    this->char_handle_ = write_chr->handle;
    this->notify_handle_ = notify_chr->handle;
    this->zentri_ = true;
    ESP_LOGI(TAG, "Zentri TruConnect peripheral detected (dual-characteristic mode).");

    auto *mode_chr = this->parent()->get_characteristic(ZENTRI_SERVICE_UUID, ZENTRI_MODE_CHAR_UUID);
    this->mode_handle_ = mode_chr != nullptr ? mode_chr->handle : 0;
  }

  auto *descr = this->parent()->get_config_descriptor(this->notify_handle_);
  if (descr == nullptr) {
    ESP_LOGW(TAG, "No CCCD found for the BAPI notify characteristic — can't subscribe to notifications.");
    return false;
  }
  this->config_descr_handle_ = descr->handle;
  return true;
}

bool WallboxBleHub::register_for_notify_() {
  auto status = esp_ble_gattc_register_for_notify(this->parent()->get_gattc_if(), this->parent()->get_remote_bda(),
                                                   this->notify_handle_);
  if (status) {
    ESP_LOGW(TAG, "esp_ble_gattc_register_for_notify failed, status=%d", status);
  }
  return status == 0;
}

bool WallboxBleHub::write_notify_config_descriptor_(bool enable) {
  uint16_t value = enable ? 0x0001 : 0x0000;
  auto status = esp_ble_gattc_write_char_descr(
      this->parent()->get_gattc_if(), this->parent()->get_conn_id(), this->config_descr_handle_, sizeof(value),
      (uint8_t *) &value, ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
  if (status) {
    ESP_LOGW(TAG, "esp_ble_gattc_write_char_descr failed, status=%d", status);
  }
  return status == 0;
}

void WallboxBleHub::write_bapi_(const char *met, const std::string &par) {
  if (this->char_handle_ == 0) return;
  int id = this->next_id_++;
  std::string cmd = bapi::build_cmd(met, par.c_str(), id);
  std::string frame = bapi::frame(cmd);

  // On a small-MTU link (Zentri: capped at the 23-byte ATT default) a BAPI
  // frame (~41 B) can exceed the single-write payload (mtu-3). Fragment into
  // (mtu-3) chunks with write-no-response — the TruConnect module reassembles
  // the byte stream, same as a plain serial link. MAX (MTU 247) never hits
  // this path. Source: si27645/esp32-wallbox `wb_ble.cpp` _sendCommandDirect.
  size_t max_chunk = this->mtu_ > 3 ? (size_t) (this->mtu_ - 3) : 20;
  const uint8_t *data = (const uint8_t *) frame.data();
  size_t len = frame.size();

  if (len <= max_chunk) {
    auto status = esp_ble_gattc_write_char(
        this->parent()->get_gattc_if(), this->parent()->get_conn_id(), this->char_handle_, len, (uint8_t *) data,
        ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
    if (status) {
      ESP_LOGW(TAG, "Write '%s' failed, status=%d", met, status);
    }
    return;
  }

  size_t off = 0;
  while (off < len) {
    size_t n = std::min(max_chunk, len - off);
    auto status = esp_ble_gattc_write_char(
        this->parent()->get_gattc_if(), this->parent()->get_conn_id(), this->char_handle_, n,
        (uint8_t *) (data + off), ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
    if (status) {
      ESP_LOGW(TAG, "Fragmented write '%s' failed at offset %zu, status=%d", met, off, status);
      return;
    }
    off += n;
    if (off < len) delay(8);  // let the controller TX buffer drain between chunks
  }
  ESP_LOGD(TAG, "TX %s fragmented: %zu bytes in %zu-byte chunks (mtu=%u)", met, len, max_chunk, this->mtu_);
}

void WallboxBleHub::begin_authenticate_() {
  this->pending_read_pin_id_ = this->next_id_;
  this->write_bapi_(bapi::MET_READ_PIN);
}

// ---- Controls (called from switch/number platforms) ----------------------

void WallboxBleHub::start_charging() {
  if (!this->authenticated_) {
    ESP_LOGW(TAG, "Not authenticated yet, ignoring start_charging()");
    return;
  }
  this->write_bapi_(bapi::MET_START_STOP, "1");
}

void WallboxBleHub::stop_charging() {
  if (!this->authenticated_) {
    ESP_LOGW(TAG, "Not authenticated yet, ignoring stop_charging()");
    return;
  }
  // par=2 is the Pulsar MAX (u-blox) "hard stop". The Zentri path uses
  // par=0 (pause) instead, like Plus/Copper/Quasar — confirmed live: par=2
  // on a Zentri charger comes back {"error":{"code":5}}. This mirrors a
  // known bug in esp32-wallbox itself (docs/CHARGER_QUIRKS.md #10: "Zentri
  // stop-par chosen by config family, not runtime _isZentri"); we have the
  // runtime detection already, so just branch on it directly.
  this->write_bapi_(bapi::MET_START_STOP, this->zentri_ ? "0" : "2");
}

void WallboxBleHub::set_max_current(uint8_t amps) {
  if (!this->authenticated_) {
    ESP_LOGW(TAG, "Not authenticated yet, ignoring set_max_current()");
    return;
  }
  this->write_bapi_(bapi::MET_SET_CURRENT, std::to_string(amps));
}

void WallboxBleHub::set_eco_mode(int mode) {
  if (!this->authenticated_) {
    ESP_LOGW(TAG, "Not authenticated yet, ignoring set_eco_mode()");
    return;
  }
  if (mode <= ECO_MODE_DISABLED) {
    // The charger ignores an `esm` change that arrives together with
    // `ese=0` in the same write, leaving `esm` stuck at its previous value
    // (reads back as a phantom mode). Two-step per esp32-wallbox
    // `wb_mqtt.cpp`: send the mode-clear with the master flag still on
    // (accepted), then drop the flag in a second write.
    this->write_bapi_(bapi::MET_SET_ECO_SMART, "{\"esm\":0,\"ese\":1,\"esp\":100}");
    this->write_bapi_(bapi::MET_SET_ECO_SMART, "{\"esm\":0,\"ese\":0,\"esp\":100}");
  } else {
    // esp (solar power target %) isn't exposed as its own entity yet —
    // matches esp32-wallbox's own eco_mode write, which always sends 100.
    std::string par = "{\"esm\":" + std::to_string(mode) + ",\"ese\":1,\"esp\":100}";
    this->write_bapi_(bapi::MET_SET_ECO_SMART, par);
  }
}

// ---- Response handling -----------------------------------------------

void WallboxBleHub::report_error_(const std::string &message) {
  ESP_LOGW(TAG, "%s", message.c_str());
  if (this->last_error_text_sensor_ != nullptr) {
    this->last_error_text_sensor_->publish_state(message);
  }
}

void WallboxBleHub::handle_response_(const std::string &json) {
  bool parsed_ok = json::parse_json(json, [this](JsonObject root) -> bool {
    int id = root["id"] | -1;

    // ---- PIN handshake ----
    // Checked before the generic error handler below: on firmware that
    // doesn't implement `read_pin` at all (e.g. the original/Zentri Pulsar),
    // the charger answers with a BAPI error (commonly code 4, "feature not
    // supported") rather than an empty response. Upstream esp32-wallbox
    // treats that identically to "no PIN set" — doc["r"]["pin"] comes back
    // null either way (missing "r" key) — so mirror that here instead of
    // reporting it as a persistent error.
    if (id == this->pending_read_pin_id_) {
      this->pending_read_pin_id_ = -1;
      if (root["error"].is<JsonObject>()) {
        int code = root["error"]["code"] | -1;
        ESP_LOGI(TAG, "read_pin not supported by this firmware (error code %d) — proceeding unauthenticated.",
                 code);
        this->authenticated_ = true;
        return true;
      }
      const char *pin = root["r"]["pin"] | (const char *) nullptr;
      if (pin == nullptr || pin[0] == '\0') {
        ESP_LOGI(TAG, "Charger has no BAPI PIN set — proceeding unauthenticated.");
        this->authenticated_ = true;
        return true;
      }
      this->pin_required_ = true;
      if (this->pin_.empty()) {
        this->report_error_("Charger requires a BAPI PIN but none was configured (set `pin:` in YAML)");
        return true;
      }
      int version = root["r"]["version"] | 0;
      std::string par = "{\"pin\":\"" + this->pin_ + "\",\"version\":" + std::to_string(version) + "}";
      this->pending_set_pin_id_ = this->next_id_;
      this->write_bapi_(bapi::MET_SET_PIN, par);
      return true;
    }
    if (id == this->pending_set_pin_id_) {
      this->pending_set_pin_id_ = -1;
      if (root["error"].is<JsonObject>()) {
        std::string msg = root["error"]["message"] | std::string("unknown BAPI error");
        this->report_error_("PIN authentication failed: " + msg);
        return true;
      }
      ESP_LOGI(TAG, "PIN authenticated.");
      this->authenticated_ = true;
      return true;
    }
    if (id == this->pending_eco_poll_id_) {
      this->pending_eco_poll_id_ = -1;
      if (root["error"].is<JsonObject>()) {
        // Not fatal, and not necessarily worth alarming over — e.g. this
        // firmware may simply not support Eco-Smart over BLE at all
        // (confirmed on a Zentri-path charger: error code 4, "feature not
        // supported", matching esp32-wallbox's own CHARGER_QUIRKS.md note
        // that Zentri "typically" lacks it). Log once at debug rather than
        // spamming the Last BAPI Error sensor every poll cycle.
        ESP_LOGD(TAG, "g_ecos error (code %d) — Eco Mode select won't update this cycle.",
                 (int) (root["error"]["code"] | -1));
        return true;
      }
      if (this->eco_mode_select_ != nullptr) {
        JsonObject r = root["r"];
        bool ese = r["ese"] | false;
        int esm = r["esm"] | 0;
        this->eco_mode_select_->publish_state(wallbox_eco_mode_to_string(wallbox_eco_mode_of(ese, esm)));
      }
      return true;
    }

    if (root["error"].is<JsonObject>()) {
      std::string msg = root["error"]["message"] | std::string("unknown BAPI error");
      this->report_error_(msg);
      return true;
    }

    // ---- r_dat status ----
    if (root["r"]["st"].is<int>()) {
      JsonObject r = root["r"];
      int st = r["st"] | -1;
      this->last_status_ = st;

      if (this->status_text_sensor_ != nullptr) {
        this->status_text_sensor_->publish_state(wallbox_status_to_string(st));
      }
      if (this->charging_sensor_ != nullptr) {
        this->charging_sensor_->publish_state(st == STATUS_CHARGING);
      }
      if (this->car_connected_sensor_ != nullptr) {
        this->car_connected_sensor_->publish_state(wallbox_status_car_connected(st));
      }
      if (this->power_sensor_ != nullptr) {
        if (r["cp"].is<float>()) {
          this->power_sensor_->publish_state(r["cp"].as<float>());
        } else if (r["L1"].is<int>() || r["L2"].is<int>() || r["L3"].is<int>()) {
          float deciamps = (r["L1"] | 0) + (r["L2"] | 0) + (r["L3"] | 0);
          // Sensor unit is kW (matches "cp" above) — watts / 1000.
          this->power_sensor_->publish_state((deciamps / 10.0f) * ASSUMED_MAINS_VOLTAGE / 1000.0f);
        }
      }
      if (this->energy_sensor_ != nullptr && r["en"].is<float>()) {
        // r_dat.en is centi-kWh (see docs/CHARGER_QUIRKS.md in the
        // upstream firmware repo) — divide by 100 for kWh.
        this->energy_sensor_->publish_state(r["en"].as<float>() / 100.0f);
      }
      if (r["cur"].is<int>()) {
        this->last_cur_ = r["cur"].as<int>();
        if (this->max_current_sensor_ != nullptr) {
          this->max_current_sensor_->publish_state(this->last_cur_);
        }
      }

      // Fire the eco-mode poll now that r_dat's response is fully consumed
      // — see update()/eco_poll_due_ for why this can't just fire alongside
      // r_dat itself.
      if (this->eco_poll_due_) {
        this->eco_poll_due_ = false;
        this->pending_eco_poll_id_ = this->next_id_;
        this->write_bapi_(bapi::MET_GET_ECO_SMART);
      }
    }
    return true;
  });

  if (!parsed_ok) {
    ESP_LOGV(TAG, "Non-JSON or partial BAPI response ignored (%zu bytes)", json.size());
  }
}

// ---- GATT event handler ---------------------------------------------------

void WallboxBleHub::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                        esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_DISCONNECT_EVT: {
      this->authenticated_ = false;
      this->pin_required_ = false;
      this->char_handle_ = 0;
      this->notify_handle_ = 0;
      this->mode_handle_ = 0;
      this->zentri_ = false;
      this->mtu_ = 23;
      this->eco_poll_due_ = false;
      this->parser_.reset();
      break;
    }
    case ESP_GATTC_CFG_MTU_EVT: {
      this->mtu_ = param->cfg_mtu.mtu;
      break;
    }
    case ESP_GATTC_SEARCH_CMPL_EVT: {
      if (!this->discover_characteristic_()) {
        this->node_state = espbt::ClientState::IDLE;
        break;
      }
      this->node_state = espbt::ClientState::ESTABLISHED;
      this->register_for_notify_();
      break;
    }
    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      if (param->reg_for_notify.handle != this->notify_handle_) break;
      // Mirrors ESPHome's `bedjet` component: once node_state is
      // ESTABLISHED the parent BLEClient purges its cached handle/
      // descriptor table, so the CCCD write has to happen here rather
      // than relying on BLEClient's own (now-gone) bookkeeping.
      this->write_notify_config_descriptor_(true);
      ESP_LOGD(TAG, "Subscribed to BAPI notifications.");

      if (this->zentri_ && this->mode_handle_ != 0) {
        // Switch the Zentri TruConnect module to STREAM_MODE (0x01) so it
        // passes BAPI bytes through transparently instead of swallowing
        // them as module commands. Source: esp32-wallbox `wb_ble.cpp`.
        uint8_t stream_mode = 0x01;
        esp_ble_gattc_write_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(), this->mode_handle_,
                                  1, &stream_mode, ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
        ESP_LOGD(TAG, "Zentri: sent STREAM_MODE switch.");
        delay(200);  // let the module honour the mode change before first write
      }

      ESP_LOGD(TAG, "Starting PIN handshake.");
      this->parser_.reset();
      this->begin_authenticate_();
      break;
    }
    case ESP_GATTC_NOTIFY_EVT: {
      if (param->notify.handle != this->notify_handle_) break;
      ESP_LOGVV(TAG, "RX notify chunk (%u bytes): %.*s", param->notify.value_len, param->notify.value_len,
                (const char *) param->notify.value);
      if (this->parser_.feed(param->notify.value, param->notify.value_len)) {
        ESP_LOGD(TAG, "RX: %s", this->parser_.json_string().c_str());
        this->handle_response_(this->parser_.json_string());
        this->parser_.reset();
      }
      break;
    }
    default:
      break;
  }
}

}  // namespace wallbox_ble
}  // namespace esphome

#endif
