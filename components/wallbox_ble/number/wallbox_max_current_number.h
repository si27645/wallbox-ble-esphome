#pragma once

#ifdef USE_ESP32

#include "esphome/core/component.h"
#include "esphome/components/number/number.h"
#include "../wallbox_ble.h"

namespace esphome {
namespace wallbox_ble {

// Optimistic max-current control: writes `w_mxI` and publishes the
// requested value immediately; the next status poll's `cur` field
// corrects it if the charger clamped or rejected the value.
class WallboxMaxCurrentNumber : public number::Number, public Component {
 public:
  void set_hub(WallboxBleHub *hub) { this->hub_ = hub; }

 protected:
  void control(float value) override {
    this->hub_->set_max_current((uint8_t) value);
    this->publish_state(value);
  }

  WallboxBleHub *hub_;
};

}  // namespace wallbox_ble
}  // namespace esphome

#endif
