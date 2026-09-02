#pragma once

#ifdef USE_ESP32

#include "esphome/core/component.h"
#include "esphome/components/switch/switch.h"
#include "../wallbox_ble.h"

namespace esphome {
namespace wallbox_ble {

// Optimistic start/stop switch: writes `w_cha` and immediately publishes
// the requested state, since BAPI writes are fire-and-forget (no ack in
// this component yet). The next status poll corrects it if the write
// didn't actually take.
class WallboxChargingSwitch : public switch_::Switch, public Component {
 public:
  void set_hub(WallboxBleHub *hub) { this->hub_ = hub; }
  void loop() override {}

 protected:
  void write_state(bool state) override {
    if (state) {
      this->hub_->start_charging();
    } else {
      this->hub_->stop_charging();
    }
    this->publish_state(state);
  }

  WallboxBleHub *hub_;
};

}  // namespace wallbox_ble
}  // namespace esphome

#endif
