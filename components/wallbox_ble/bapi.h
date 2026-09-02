#pragma once

#include <string>
#include <cstdint>
#include <cstddef>

// BAPI framing: the wire protocol Wallbox's BLE module speaks.
//   -> device: "EaE" + 1-byte length + JSON payload + 1-byte checksum
//   <- device: raw JSON (no framing), may arrive split across several
//              20-byte BLE notifications.
//
// Ported from si27645/esp32-wallbox (fork of botts7/esp32-wallbox, MIT
// licensed), which credits jagheterfredrik/wallbox-ble and
// jagheterfredrik/wallbox-mqtt-bridge for the original protocol
// reverse-engineering. Only the framing/parsing logic is ported here;
// Arduino `String` was swapped for `std::string` since this component
// targets ESPHome's esp32_ble_client stack, not NimBLE-Arduino directly.

namespace esphome {
namespace wallbox_ble {
namespace bapi {

// Frame a JSON payload into BAPI wire format, ready to write to the
// charger's command characteristic.
std::string frame(const std::string &json_payload);

// Build a minimal BAPI command JSON string: {"met":"...","par":...,"id":n}
// `par` must already be valid JSON (e.g. "null", "1", "{\"pin\":\"1234\"}").
std::string build_cmd(const char *met, const char *par = "null", int id = 0);

// Accumulates raw BLE notification bytes until a complete top-level JSON
// object has been received (the response has no BAPI framing, just JSON).
class ResponseParser {
 public:
  void reset();
  // Feed raw notification bytes. Returns true once a complete JSON object
  // is available via json_string().
  bool feed(const uint8_t *data, size_t len);
  const std::string &json_string() const { return buf_; }

 private:
  std::string buf_;
  int brace_depth_{0};
  bool in_string_{false};
  bool escape_{false};
};

// ---- BAPI method names -------------------------------------------------
// Discovered/documented by jagheterfredrik/wallbox-ble and confirmed by
// botts7/esp32-wallbox against the Pulsar MAX (the reference model this
// component targets — see README for other models).

// Read
constexpr const char *MET_GET_STATUS = "r_dat";  // main status: st, cp, en, cur, L1-L3...
constexpr const char *MET_READ_PIN = "read_pin";

// Write
constexpr const char *MET_START_STOP = "w_cha";   // par=1 start, par=2 stop (MAX)
constexpr const char *MET_SET_CURRENT = "w_mxI";  // par=<amps>
constexpr const char *MET_SET_PIN = "set_pin";    // par={"pin":"...","version":n}

}  // namespace bapi
}  // namespace wallbox_ble
}  // namespace esphome
