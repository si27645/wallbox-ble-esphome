#pragma once

#ifdef USE_ESP32

#include "esphome/core/component.h"
#include "esphome/components/select/select.h"
#include "../wallbox_ble.h"

namespace esphome {
namespace wallbox_ble {

// Optimistic Eco-Smart mode select: writes `s_ecos` and immediately
// publishes the requested option. The hub also publishes real readback
// from periodic `g_ecos` polls (see WallboxBleHub::update()), which
// corrects this if the write didn't take or someone changed the mode
// via the Wallbox app.
class WallboxEcoModeSelect : public select::Select, public Component {
 public:
  void set_hub(WallboxBleHub *hub) { this->hub_ = hub; }

 protected:
  void control(const std::string &value) override {
    int mode = wallbox_eco_mode_from_string(value);
    if (mode < 0) {
      // Shouldn't happen — HA only offers the options we declared — but
      // guard against a stale/mismatched frontend anyway.
      return;
    }
    this->hub_->set_eco_mode(mode);
    this->publish_state(value);
  }

  WallboxBleHub *hub_;
};

}  // namespace wallbox_ble
}  // namespace esphome

#endif
