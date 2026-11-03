#include "sim_elm.h"

#include <stdio.h>
#include <string.h>

#include "sim.h"

namespace sim {
namespace {

constexpr uint32_t kConnectMs = 900;
constexpr uint32_t kLatencyMs = 45;  // typical Bluetooth round trip

// PIDs the emulated ECU supports (no 0x5E: the link estimates the fuel rate from MAF).
const uint8_t kSupported[] = {0x01, 0x04, 0x05, 0x06, 0x07, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
                              0x10, 0x11, 0x1F, 0x20, 0x2F, 0x40, 0x46, 0x5C};

uint32_t support_mask(uint8_t base) {
  uint32_t mask = 0;
  for (uint8_t pid : kSupported) {
    if (pid > base && pid <= base + 32) mask |= 1u << (31 - (pid - base - 1));
  }
  return mask;
}

int clamp_byte(float v) { return v < 0 ? 0 : (v > 255 ? 255 : static_cast<int>(v + 0.5f)); }
int clamp_word(float v) { return v < 0 ? 0 : (v > 65535 ? 65535 : static_cast<int>(v + 0.5f)); }

std::string hex_bytes(const int* bytes, int n) {
  std::string s;
  char b[4];
  for (int i = 0; i < n; ++i) {
    snprintf(b, sizeof(b), i == 0 ? "%02X" : " %02X", bytes[i]);
    s += b;
  }
  return s;
}

std::string upper_no_spaces(const std::string& in) {
  std::string out;
  for (char c : in) {
    if (c == ' ') continue;
    out += (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
  }
  return out;
}

}  // namespace

void FakeElm327::open() {
  state_ = State::Connecting;
  connected_at_ = now() + kConnectMs;
  echo_ = spaces_ = true;
  searched_ = false;
  line_.clear();
  out_.clear();
  car_.begin(model_, now());
}

void FakeElm327::close() { state_ = State::Closed; }

core::ObdTransport::State FakeElm327::state() {
  if (state_ == State::Connecting && static_cast<int32_t>(now() - connected_at_) >= 0) state_ = State::Connected;
  return state_;
}

size_t FakeElm327::read(uint8_t* buf, size_t len) {
  if (state_ != State::Connected || out_.empty() || static_cast<int32_t>(now() - out_ready_at_) < 0) return 0;
  const size_t n = out_.size() < len ? out_.size() : len;
  memcpy(buf, out_.data(), n);
  out_.erase(0, n);
  return n;
}

void FakeElm327::write(const char* text) {
  for (const char* p = text; *p != '\0'; ++p) {
    if (*p == '\r') {
      execute(line_);
      line_.clear();
    } else {
      line_ += *p;
    }
  }
}

void FakeElm327::reply(const std::string& command, const std::string& body, uint32_t delay_ms) {
  std::string text = body;
  if (!spaces_) text = upper_no_spaces(text);
  out_ = (echo_ ? command + "\r" : std::string()) + text + "\r\r>";
  out_ready_at_ = now() + delay_ms;
}

std::string FakeElm327::pid_reply(uint8_t pid) {
  using core::SignalId;
  const uint32_t t = now();
  auto v = [&](SignalId id) { return model_.value_or(id, t, 0.0f); };
  int d[4];
  int n = 1;
  switch (pid) {
    case 0x00: case 0x20: case 0x40: {
      const uint32_t m = support_mask(pid);
      d[0] = m >> 24 & 0xFF; d[1] = m >> 16 & 0xFF; d[2] = m >> 8 & 0xFF; d[3] = m & 0xFF;
      n = 4;
      break;
    }
    case 0x01:  // MIL + DTC count, monitors complete
      d[0] = codes_cleared_ ? 0x00 : 0x83; d[1] = 0x07; d[2] = 0xE5; d[3] = 0x00;
      n = 4;
      break;
    case 0x04: d[0] = clamp_byte(v(SignalId::EngineLoad) * 255 / 100); break;
    case 0x05: d[0] = clamp_byte(v(SignalId::CoolantTemp) + 40); break;
    case 0x06: d[0] = clamp_byte(v(SignalId::ShortFuelTrim) * 128 / 100 + 128); break;
    case 0x07: d[0] = clamp_byte(v(SignalId::LongFuelTrim) * 128 / 100 + 128); break;
    case 0x0B: d[0] = clamp_byte(v(SignalId::IntakePressure)); break;
    case 0x0D: d[0] = clamp_byte(v(SignalId::Speed)); break;
    case 0x0E: d[0] = clamp_byte((v(SignalId::TimingAdvance) + 64) * 2); break;
    case 0x0F: d[0] = clamp_byte(v(SignalId::IntakeTemp) + 40); break;
    case 0x11: d[0] = clamp_byte(v(SignalId::ThrottlePos) * 255 / 100); break;
    case 0x2F: d[0] = clamp_byte(v(SignalId::FuelLevel) * 255 / 100); break;
    case 0x46: d[0] = clamp_byte(v(SignalId::AmbientTemp) + 40); break;
    case 0x5C: d[0] = clamp_byte(v(SignalId::OilTemp) + 40); break;
    case 0x0C: case 0x10: case 0x1F: {
      const float raw = pid == 0x0C ? v(SignalId::Rpm) * 4 : pid == 0x10 ? v(SignalId::Maf) * 100 : v(SignalId::RunTime);
      const int w = clamp_word(raw);
      d[0] = w >> 8; d[1] = w & 0xFF;
      n = 2;
      break;
    }
    default:
      return "NO DATA";
  }
  char head[8];
  snprintf(head, sizeof(head), "41 %02X ", pid);
  return head + hex_bytes(d, n);
}

void FakeElm327::execute(const std::string& raw) {
  car_.poll(model_, now());
  const std::string cmd = upper_no_spaces(raw);
  if (cmd.empty()) return;

  if (cmd.rfind("AT", 0) == 0) {
    if (cmd == "ATZ") {
      echo_ = spaces_ = true;
      searched_ = false;
      reply(raw, "\rELM327 v1.5", 600);
    } else if (cmd == "ATE0") {
      reply(raw, "OK", kLatencyMs);
      echo_ = false;  // the reply to ATE0 itself is still echoed
    } else if (cmd == "ATS0") {
      spaces_ = false;
      reply(raw, "OK", kLatencyMs);
    } else if (cmd == "ATDPN") {
      reply(raw, "A6", kLatencyMs);
    } else if (cmd == "ATRV") {
      char volts[16];
      snprintf(volts, sizeof(volts), "%.1fV", static_cast<double>(model_.value_or(core::SignalId::BatteryVoltage, now(), 12.4f)));
      reply(raw, volts, kLatencyMs);
    } else {
      reply(raw, "OK", kLatencyMs);
    }
    return;
  }

  if (cmd.size() == 4 && cmd.rfind("01", 0) == 0) {
    const uint8_t pid = static_cast<uint8_t>(strtoul(cmd.c_str() + 2, nullptr, 16));
    std::string body = pid_reply(pid);
    uint32_t delay = kLatencyMs;
    if (pid == 0x00 && !searched_) {
      body = "SEARCHING...\r" + body;
      searched_ = true;
      delay = 1500;
    }
    reply(raw, body, delay);
    return;
  }

  if (cmd == "03") {
    // P0301, P0420, P0128 stored: 8 data bytes -> ISO-TP multi-frame reply.
    reply(raw, codes_cleared_ ? "43 00" : "008\r0: 43 03 03 01 04 20\r1: 01 28 00 00 00 00 00", kLatencyMs * 2);
  } else if (cmd == "07") {
    reply(raw, codes_cleared_ ? "NO DATA" : "47 01 01 71", kLatencyMs * 2);  // P0171 pending
  } else if (cmd == "0A") {
    reply(raw, "NO DATA", kLatencyMs * 2);
  } else if (cmd == "04") {
    codes_cleared_ = true;
    reply(raw, "44", 400);
  } else {
    reply(raw, "?", kLatencyMs);
  }
}

}  // namespace sim
