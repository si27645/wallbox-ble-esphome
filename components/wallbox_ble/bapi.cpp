#include "bapi.h"

namespace esphome {
namespace wallbox_ble {
namespace bapi {

static uint8_t calc_checksum(const uint8_t *data, size_t len) {
  uint32_t sum = 0;
  for (size_t i = 0; i < len; i++) sum += data[i];
  return sum & 0xFF;
}

std::string frame(const std::string &json_payload) {
  size_t plen = json_payload.size();
  std::string out;
  out.reserve(plen + 16);

  out += 'E';
  out += 'a';
  out += 'E';

  if (plen < 256) {
    out += (char) plen;
  } else {
    // Long-form length: 0x00 + ASCII length + 0x00 (mirrors upstream;
    // BAPI commands are always short in practice, this path is untested).
    out += (char) 0;
    out += std::to_string(plen);
    out += (char) 0;
  }

  out += json_payload;

  uint8_t cs = calc_checksum(reinterpret_cast<const uint8_t *>(out.data()), out.size());
  out += (char) cs;

  return out;
}

std::string build_cmd(const char *met, const char *par, int id) {
  std::string json;
  json.reserve(128);
  json += "{\"met\":\"";
  json += met;
  json += "\",\"par\":";
  json += par;
  json += ",\"id\":";
  json += std::to_string(id);
  json += "}";
  return json;
}

void ResponseParser::reset() {
  buf_.clear();
  buf_.reserve(1024);
  brace_depth_ = 0;
  in_string_ = false;
  escape_ = false;
}

bool ResponseParser::feed(const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    char c = (char) data[i];
    buf_ += c;

    if (escape_) {
      escape_ = false;
      continue;
    }
    if (c == '\\' && in_string_) {
      escape_ = true;
      continue;
    }
    if (c == '"') {
      in_string_ = !in_string_;
      continue;
    }
    if (in_string_) continue;

    if (c == '{') {
      brace_depth_++;
    } else if (c == '}') {
      brace_depth_--;
      if (brace_depth_ == 0 && !buf_.empty()) {
        return true;  // complete top-level JSON object
      }
    }
  }
  return false;
}

}  // namespace bapi
}  // namespace wallbox_ble
}  // namespace esphome
