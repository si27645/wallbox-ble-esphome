#ifdef USE_ESP32

#include "wallbox_ble.h"
#include "esphome/core/log.h"
#include "esphome/components/json/json_util.h"

namespace esphome {
namespace wallbox_ble {

static const char *const TAG = "wallbox_ble";

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

void WallboxBleHub::setup() {
  this->parser_.reset();
}

void WallboxBleHub::dump_config() {
  ESP_LOGCONFIG(TAG, "Wallbox BLE Hub:");
  ESP_LOGCONFIG(TAG, "  MAC address: %s", this->parent()->address_str().c_str());
  ESP_LOGCONFIG(TAG, "  PIN configured: %s", this->pin_.empty() ? "no" : "yes");
}

void WallboxBleHub::update() {
  if (!this->is_connected() || !this->authenticated_) return;
  this->write_bapi_(bapi::MET_GET_STATUS);
}

// ---- GATT plumbing -------------------------------------------------------

bool WallboxBleHub::discover_characteristic_() {
  auto *chr = this->parent()->get_characteristic(WALLBOX_SERVICE_UUID, WALLBOX_CHAR_UUID);
  if (chr == nullptr) {
    ESP_LOGW(TAG, "BAPI characteristic not found — is this a Pulsar MAX? Plus/Copper/Quasar use a "
                  "different (dual-characteristic) service, not yet supported by this component.");
    return false;
  }
  this->char_handle_ = chr->handle;

  auto *descr = this->parent()->get_config_descriptor(this->char_handle_);
  if (descr == nullptr) {
    ESP_LOGW(TAG, "No CCCD found for the BAPI characteristic — can't subscribe to notifications.");
    return false;
  }
  this->config_descr_handle_ = descr->handle;
  return true;
}

bool WallboxBleHub::register_for_notify_() {
  auto status = esp_ble_gattc_register_for_notify(this->parent()->get_gattc_if(), this->parent()->get_remote_bda(),
                                                   this->char_handle_);
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

  auto status = esp_ble_gattc_write_char(
      this->parent()->get_gattc_if(), this->parent()->get_conn_id(), this->char_handle_, frame.size(),
      (uint8_t *) frame.data(), ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
  if (status) {
    ESP_LOGW(TAG, "Write '%s' failed, status=%d", met, status);
  }
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
  // par=2 is the Pulsar MAX "hard stop". Plus/Copper/Quasar use par=0
  // (pause) instead — not distinguished here since this component only
  // targets MAX for now. See README roadmap.
  this->write_bapi_(bapi::MET_START_STOP, "2");
}

void WallboxBleHub::set_max_current(uint8_t amps) {
  if (!this->authenticated_) {
    ESP_LOGW(TAG, "Not authenticated yet, ignoring set_max_current()");
    return;
  }
  this->write_bapi_(bapi::MET_SET_CURRENT, std::to_string(amps));
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

    if (root["error"].is<JsonObject>()) {
      std::string msg = root["error"]["message"] | std::string("unknown BAPI error");
      this->report_error_(msg);
      return true;
    }

    // ---- PIN handshake ----
    if (id == this->pending_read_pin_id_) {
      this->pending_read_pin_id_ = -1;
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
      ESP_LOGI(TAG, "PIN authenticated.");
      this->authenticated_ = true;
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
      if (this->power_sensor_ != nullptr && r["cp"].is<float>()) {
        this->power_sensor_->publish_state(r["cp"].as<float>());
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
      this->parser_.reset();
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
      if (param->reg_for_notify.handle != this->char_handle_) break;
      // Mirrors ESPHome's `bedjet` component: once node_state is
      // ESTABLISHED the parent BLEClient purges its cached handle/
      // descriptor table, so the CCCD write has to happen here rather
      // than relying on BLEClient's own (now-gone) bookkeeping.
      this->write_notify_config_descriptor_(true);
      ESP_LOGD(TAG, "Subscribed to BAPI notifications, starting PIN handshake.");
      this->parser_.reset();
      this->begin_authenticate_();
      break;
    }
    case ESP_GATTC_NOTIFY_EVT: {
      if (param->notify.handle != this->char_handle_) break;
      if (this->parser_.feed(param->notify.value, param->notify.value_len)) {
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
