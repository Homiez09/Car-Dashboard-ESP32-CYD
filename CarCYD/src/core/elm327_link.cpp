#include "elm327_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "util.h"

namespace core {

// ---- Response parsing -------------------------------------------------------------------

namespace elm327 {
namespace {

constexpr uint8_t kMaxMessages = 8;
constexpr uint8_t kMaxMessageBytes = 64;

struct Message {
  uint8_t bytes[kMaxMessageBytes];
  uint8_t len;
};

bool is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'); }

uint8_t hex_value(char c) { return static_cast<uint8_t>(c <= '9' ? c - '0' : c - 'A' + 10); }

char upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

bool all_hex(const char* s) {
  for (; *s != '\0'; ++s) {
    if (!is_hex(*s)) return false;
  }
  return true;
}

/** True if `needle` (upper case, no spaces) occurs in `hay`, ignoring spaces and case. */
bool contains(const char* hay, const char* needle) {
  for (const char* start = hay; *start != '\0'; ++start) {
    const char* h = start;
    const char* n = needle;
    while (*n != '\0' && *h != '\0') {
      if (*h == ' ') {
        ++h;
        continue;
      }
      if (upper(*h) != *n) break;
      ++h;
      ++n;
    }
    if (*n == '\0') return true;
  }
  return false;
}

/**
 * Splits a response into messages of data bytes. Each line is one message,
 * except ISO-TP multi-frame lines ("1:", "2:", ...) that continue the
 * message started by the preceding "0:" line. Non-hex lines (SEARCHING...,
 * BUS INIT, OK, echo) are skipped.
 */
uint8_t split_messages(const char* response, Message* out, uint8_t max) {
  uint8_t count = 0;
  Message* current = nullptr;
  char line[128];
  const char* p = response;
  while (*p != '\0') {
    uint8_t n = 0;
    while (*p != '\0' && *p != '\r' && *p != '\n') {
      const char c = upper(*p++);
      if (c == ' ' || c == '\t') continue;
      if (n < sizeof(line) - 1) line[n++] = c;
    }
    while (*p == '\r' || *p == '\n') ++p;
    line[n] = '\0';
    if (n == 0) continue;

    const char* data = line;
    bool continuation = false;
    if (n >= 2 && is_hex(line[0]) && line[1] == ':') {
      continuation = line[0] != '0';
      data = line + 2;
    }
    const size_t len = strlen(data);
    if (len == 0 || len % 2 != 0 || !all_hex(data)) {
      // Text line or ISO-TP length line ("00A"): ends the current message.
      current = nullptr;
      continue;
    }
    if (!continuation || current == nullptr) {
      if (count >= max) break;
      current = &out[count++];
      current->len = 0;
    }
    for (size_t i = 0; i + 1 < len && current->len < kMaxMessageBytes; i += 2) {
      current->bytes[current->len++] = static_cast<uint8_t>(hex_value(data[i]) << 4 | hex_value(data[i + 1]));
    }
  }
  return count;
}

}  // namespace

Reply classify(const char* response) {
  if (contains(response, "STOPPED")) return Reply::Stopped;
  if (contains(response, "NODATA")) return Reply::NoData;
  if (contains(response, "UNABLETOCONNECT") || contains(response, "ERROR") || contains(response, "BUSBUSY")) {
    return Reply::BusError;  // CAN ERROR, BUS ERROR, FB ERROR, DATA ERROR, BUS INIT: ...ERROR
  }
  Message messages[kMaxMessages];
  return split_messages(response, messages, kMaxMessages) > 0 ? Reply::Data : Reply::Unknown;
}

bool parse_pid(const char* response, uint8_t pid, uint8_t* data, uint8_t max, uint8_t& len) {
  Message messages[kMaxMessages];
  const uint8_t count = split_messages(response, messages, kMaxMessages);
  for (uint8_t i = 0; i < count; ++i) {
    const Message& m = messages[i];
    if (m.len < 3 || m.bytes[0] != 0x41 || m.bytes[1] != pid) continue;
    len = 0;
    for (uint8_t b = 2; b < m.len && len < max; ++b) data[len++] = m.bytes[b];
    return true;
  }
  return false;
}

uint8_t parse_dtcs(const char* response, uint8_t reply_mode, bool can, uint16_t* codes, uint8_t max) {
  Message messages[kMaxMessages];
  const uint8_t count = split_messages(response, messages, kMaxMessages);
  uint8_t found = 0;
  for (uint8_t i = 0; i < count; ++i) {
    const Message& m = messages[i];
    if (m.len < 1 || m.bytes[0] != reply_mode) continue;
    uint8_t pos = 1;
    uint8_t pairs = static_cast<uint8_t>((m.len - 1) / 2);
    if (can) {
      if (m.len < 2) continue;
      pos = 2;
      pairs = static_cast<uint8_t>(m.bytes[1] < (m.len - 2) / 2 ? m.bytes[1] : (m.len - 2) / 2);
    }
    for (uint8_t p = 0; p < pairs && found < max; ++p, pos += 2) {
      const uint16_t code = static_cast<uint16_t>(m.bytes[pos] << 8 | m.bytes[pos + 1]);
      if (code == 0) continue;  // padding
      bool duplicate = false;
      for (uint8_t k = 0; k < found; ++k) duplicate |= codes[k] == code;
      if (!duplicate) codes[found++] = code;
    }
  }
  return found;
}

bool decode_pid(uint8_t pid, const uint8_t* d, uint8_t len, SignalId& id, float& value) {
  if (len < 1) return false;
  const float a = d[0];
  const float ab = len >= 2 ? static_cast<float>(d[0] * 256 + d[1]) : NAN;
  switch (pid) {
    case 0x04: id = SignalId::EngineLoad; value = a * 100.0f / 255.0f; break;
    case 0x05: id = SignalId::CoolantTemp; value = a - 40.0f; break;
    case 0x06: id = SignalId::ShortFuelTrim; value = (a - 128.0f) * 100.0f / 128.0f; break;
    case 0x07: id = SignalId::LongFuelTrim; value = (a - 128.0f) * 100.0f / 128.0f; break;
    case 0x0B: id = SignalId::IntakePressure; value = a; break;
    case 0x0C: id = SignalId::Rpm; value = ab / 4.0f; break;
    case 0x0D: id = SignalId::Speed; value = a; break;
    case 0x0E: id = SignalId::TimingAdvance; value = a / 2.0f - 64.0f; break;
    case 0x0F: id = SignalId::IntakeTemp; value = a - 40.0f; break;
    case 0x10: id = SignalId::Maf; value = ab / 100.0f; break;
    case 0x11: id = SignalId::ThrottlePos; value = a * 100.0f / 255.0f; break;
    case 0x1F: id = SignalId::RunTime; value = ab; break;
    case 0x2F: id = SignalId::FuelLevel; value = a * 100.0f / 255.0f; break;
    case 0x46: id = SignalId::AmbientTemp; value = a - 40.0f; break;
    case 0x5C: id = SignalId::OilTemp; value = a - 40.0f; break;
    case 0x5E: id = SignalId::FuelRate; value = ab / 20.0f; break;
    default: return false;
  }
  return !isnan(value);
}

}  // namespace elm327

// ---- Link ----------------------------------------------------------------------------

namespace {

constexpr uint32_t kStaleMs = 8000;            // OBD polling is slower than the demo model
constexpr uint32_t kCommandTimeoutMs = 1500;
constexpr uint32_t kResetTimeoutMs = 3000;     // ATZ
constexpr uint32_t kSearchTimeoutMs = 15000;   // first 0100: automatic protocol search
constexpr uint32_t kAdapterRetryMs = 4000;
constexpr uint32_t kProbeRetryMs = 4000;       // ignition off: ask the ECU again
constexpr uint8_t kMaxTimeouts = 3;
constexpr uint8_t kMaxInitAttempts = 3;
constexpr uint8_t kMaxNoData = 3;
constexpr uint8_t kMaxNoDataRun = 12;

constexpr uint8_t kPidStatus = 0x01;    // MIL and number of DTCs
constexpr uint8_t kPidFuelRate = 0x5E;
constexpr uint8_t kPidVoltage = 0xFF;   // pseudo PID: ATRV (adapter supply = battery voltage)

// Stoichiometric petrol: fuel [L/h] = MAF [g/s] * 3600 / (14.7 * 745 g/L)
constexpr float kMafToLitresPerHour = 3600.0f / (14.7f * 745.0f);

struct PollDef {
  uint8_t pid;
  uint16_t period_ms;
};

// Driving values first; the scheduler always sends the most overdue entry.
const PollDef kPoll[Elm327Link::kPollCount] = {
    {0x0C, 250},  {0x0D, 250},  {0x11, 1000}, {0x04, 1000}, {0x0B, 1000},       {0x10, 1000},
    {0x0E, 1500}, {0x5E, 1500}, {0x05, 3000}, {0x0F, 3000}, {0x5C, 3000},       {0x06, 3000},
    {0x07, 5000}, {0x2F, 5000}, {0x46, 5000}, {0x1F, 5000}, {kPidVoltage, 3000}, {kPidStatus, 10000},
};

const char* const kInitCommands[] = {"ATZ", "ATE0", "ATL0", "ATS0", "ATH0", "ATAT1", "ATSP0"};
constexpr uint8_t kInitCount = sizeof(kInitCommands) / sizeof(kInitCommands[0]);

// DTC sequences: requests and the reply mode / status they produce.
struct DtcStep {
  const char* command;
  uint8_t reply;
  DtcStatus status;
};
const DtcStep kDtcRead[] = {
    {"0101", 0x41, DtcStatus::Stored},
    {"03", 0x43, DtcStatus::Stored},
    {"07", 0x47, DtcStatus::Pending},
    {"0A", 0x4A, DtcStatus::Permanent},
};
constexpr uint8_t kDtcReadSteps = sizeof(kDtcRead) / sizeof(kDtcRead[0]);

}  // namespace

void Elm327Link::begin(VehicleState& state, uint32_t now_ms) {
  state.simulated = false;
  state.clock_valid = false;
  state.adapter_used = true;
  state.adapter = transport_ != nullptr ? transport_->kind() : AdapterTransport::Bluetooth;
  state.stale_after_ms = kStaleMs;
  state.invalidate_all();
  busy_ = false;
  dtc_op_ = DtcOperation::Idle;
  enter(transport_ != nullptr && transport_->configured() ? Phase::AdapterRetry : Phase::Unconfigured, state,
        now_ms);
  retry_at_ms_ = now_ms;  // connect right away
}

void Elm327Link::end(VehicleState& state) {
  if (transport_ != nullptr) transport_->close();
  busy_ = false;
  state.adapter_state = LinkState::Disconnected;
  state.link_state = LinkState::Disconnected;
}

void Elm327Link::enter(Phase phase, VehicleState& state, uint32_t now_ms) {
  phase_ = phase;
  busy_ = false;
  rx_len_ = 0;
  timeouts_ = 0;
  switch (phase) {
    case Phase::Unconfigured:
      state.adapter_state = LinkState::Disconnected;
      state.link_state = LinkState::Disconnected;
      break;
    case Phase::Adapter:
      state.adapter_state = LinkState::Connecting;
      state.link_state = LinkState::Disconnected;
      break;
    case Phase::AdapterRetry:
      // Error is shown until the next attempt starts.
      if (state.adapter_state != LinkState::Disconnected) state.adapter_state = LinkState::Error;
      state.link_state = LinkState::Disconnected;
      retry_at_ms_ = now_ms + kAdapterRetryMs;
      break;
    case Phase::Init:
      LOG_I("ELM", "adapter connected, initialising");
      state.adapter_state = LinkState::Connected;
      state.link_state = LinkState::Connecting;
      init_step_ = 0;
      break;
    case Phase::Probe:
      state.link_state = LinkState::Connecting;
      probe_step_ = 0;
      break;
    case Phase::ProbeWait:
      state.link_state = LinkState::Connecting;
      retry_at_ms_ = now_ms + kProbeRetryMs;
      break;
    case Phase::Running:
      LOG_I("ELM", "vehicle connected (protocol %u)", state.obd_protocol);
      state.link_state = LinkState::Connected;
      no_data_run_ = 0;
      for (uint8_t i = 0; i < kPollCount; ++i) {
        last_poll_ms_[i] = now_ms - kPoll[i].period_ms;  // everything is due
        no_data_[i] = 0;
      }
      break;
  }
}

void Elm327Link::poll(VehicleState& state, uint32_t now_ms) {
  if (transport_ == nullptr || phase_ == Phase::Unconfigured) return;
  const ObdTransport::State ts = transport_->state();

  switch (phase_) {
    case Phase::AdapterRetry:
      if (static_cast<int32_t>(now_ms - retry_at_ms_) >= 0) {
        transport_->open();
        enter(Phase::Adapter, state, now_ms);
      }
      return;
    case Phase::Adapter:
      if (ts == ObdTransport::State::Connected) {
        init_attempts_ = 0;
        enter(Phase::Init, state, now_ms);
      } else if (ts == ObdTransport::State::Failed || ts == ObdTransport::State::Closed) {
        LOG_W("ELM", "adapter connection failed");
        transport_->close();
        enter(Phase::AdapterRetry, state, now_ms);
      }
      return;
    default:
      break;
  }

  if (ts != ObdTransport::State::Connected) {
    LOG_W("ELM", "adapter connection lost");
    if (dtc_op_ != DtcOperation::Idle) finish_dtc(state, now_ms, false);
    state.invalidate_all();
    state.obd_protocol = 0;
    transport_->close();
    enter(Phase::AdapterRetry, state, now_ms);
    return;
  }

  receive(state, now_ms);
  if (busy_ && elapsed(now_ms, sent_ms_) >= timeout_ms_) on_timeout(state, now_ms);
  if (!busy_) next_request(state, now_ms);
}

void Elm327Link::send(const char* command, Req req, uint8_t arg, uint32_t timeout_ms, uint32_t now_ms) {
  LOG_D("ELM", "> %s", command);
  transport_->write(command);
  transport_->write("\r");
  busy_ = true;
  req_ = req;
  req_arg_ = arg;
  sent_ms_ = now_ms;
  timeout_ms_ = timeout_ms;
  rx_len_ = 0;
}

void Elm327Link::receive(VehicleState& state, uint32_t now_ms) {
  uint8_t chunk[64];
  size_t n;
  while ((n = transport_->read(chunk, sizeof(chunk))) > 0) {
    for (size_t i = 0; i < n; ++i) {
      const char c = static_cast<char>(chunk[i]);
      if (c == '>') {
        rx_[rx_len_] = '\0';
        if (busy_) on_response(state, now_ms);
        rx_len_ = 0;
      } else if (c != '\0' && rx_len_ < kRxSize - 1) {
        rx_[rx_len_++] = c;
      }
    }
  }
}

void Elm327Link::on_response(VehicleState& state, uint32_t now_ms) {
  busy_ = false;
  timeouts_ = 0;
  LOG_D("ELM", "< %s", rx_);
  switch (req_) {
    case Req::Init:
      if (++init_step_ >= kInitCount) enter(Phase::Probe, state, now_ms);
      break;
    case Req::Probe:
      on_probe_response(state, now_ms);
      break;
    case Req::Pid:
    case Req::Voltage:
      on_pid_response(state, now_ms);
      break;
    case Req::Dtc:
      on_dtc_response(state, now_ms);
      break;
    case Req::None:
      break;
  }
}

void Elm327Link::on_timeout(VehicleState& state, uint32_t now_ms) {
  busy_ = false;
  ++timeouts_;
  LOG_W("ELM", "timeout (%u)", timeouts_);
  switch (phase_) {
    case Phase::Init:
      // Clones are sometimes slow after ATZ: start over a few times, then reconnect.
      if (++init_attempts_ >= kMaxInitAttempts) {
        transport_->close();
        enter(Phase::AdapterRetry, state, now_ms);
      } else {
        init_step_ = 0;
      }
      break;
    case Phase::Probe:
      enter(Phase::ProbeWait, state, now_ms);
      break;
    case Phase::Running:
      if (dtc_op_ != DtcOperation::Idle) {
        finish_dtc(state, now_ms, false);
      } else if (timeouts_ >= kMaxTimeouts) {
        ecu_lost(state, now_ms);
        init_attempts_ = 0;
        enter(Phase::Init, state, now_ms);
      }
      break;
    default:
      break;
  }
}

void Elm327Link::ecu_lost(VehicleState& state, uint32_t now_ms) {
  LOG_W("ELM", "vehicle not responding");
  if (dtc_op_ != DtcOperation::Idle) finish_dtc(state, now_ms, false);
  state.invalidate_all();
  enter(Phase::ProbeWait, state, now_ms);
}

bool Elm327Link::pid_supported(uint8_t pid) const {
  if (pid == kPidVoltage) return true;
  if (pid == 0 || pid > 0x60) return false;
  const uint8_t index = static_cast<uint8_t>((pid - 1) / 32);
  const uint8_t bit = static_cast<uint8_t>(31 - (pid - 1) % 32);
  return (supported_[index] >> bit) & 1u;
}

void Elm327Link::next_request(VehicleState& state, uint32_t now_ms) {
  switch (phase_) {
    case Phase::Init:
      send(kInitCommands[init_step_], Req::Init, 0, init_step_ == 0 ? kResetTimeoutMs : kCommandTimeoutMs, now_ms);
      return;
    case Phase::Probe:
      switch (probe_step_) {
        case 0: send("0100", Req::Probe, 0, kSearchTimeoutMs, now_ms); break;
        case 1: send("ATDPN", Req::Probe, 0, kCommandTimeoutMs, now_ms); break;
        case 2: send("0120", Req::Probe, 0, kCommandTimeoutMs, now_ms); break;
        case 3: send("0140", Req::Probe, 0, kCommandTimeoutMs, now_ms); break;
        default: enter(Phase::Running, state, now_ms); break;
      }
      return;
    case Phase::ProbeWait:
      if (static_cast<int32_t>(now_ms - retry_at_ms_) >= 0) enter(Phase::Probe, state, now_ms);
      return;
    case Phase::Running:
      break;
    default:
      return;
  }

  if (dtc_op_ != DtcOperation::Idle) {
    send_dtc_step(now_ms);
    return;
  }

  // Most overdue supported PID.
  int best = -1;
  int32_t best_overdue = 0;
  for (uint8_t i = 0; i < kPollCount; ++i) {
    if (no_data_[i] >= kMaxNoData || !pid_supported(kPoll[i].pid)) continue;
    const int32_t overdue = static_cast<int32_t>(elapsed(now_ms, last_poll_ms_[i]) - kPoll[i].period_ms);
    if (overdue >= 0 && (best < 0 || overdue > best_overdue)) {
      best = i;
      best_overdue = overdue;
    }
  }
  if (best < 0) return;
  last_poll_ms_[best] = now_ms;
  const uint8_t pid = kPoll[best].pid;
  if (pid == kPidVoltage) {
    send("ATRV", Req::Voltage, static_cast<uint8_t>(best), kCommandTimeoutMs, now_ms);
  } else {
    char cmd[8];
    snprintf(cmd, sizeof(cmd), "01%02X", pid);
    send(cmd, Req::Pid, static_cast<uint8_t>(best), kCommandTimeoutMs, now_ms);
  }
}

void Elm327Link::on_probe_response(VehicleState& state, uint32_t now_ms) {
  uint8_t data[8];
  uint8_t len = 0;
  switch (probe_step_) {
    case 0:  // 0100
      if (elm327::parse_pid(rx_, 0x00, data, sizeof(data), len) && len >= 4) {
        supported_[0] = static_cast<uint32_t>(data[0]) << 24 | static_cast<uint32_t>(data[1]) << 16 |
                        static_cast<uint32_t>(data[2]) << 8 | data[3];
        supported_[1] = supported_[2] = 0;
        probe_step_ = 1;
      } else {
        LOG_I("ELM", "no ECU response (ignition off?)");
        enter(Phase::ProbeWait, state, now_ms);
      }
      return;
    case 1: {  // ATDPN: "A6" = automatic, protocol 6
      uint8_t protocol = 0;
      for (const char* p = rx_; *p != '\0'; ++p) {
        const char c = elm327::upper(*p);
        if (elm327::is_hex(c)) protocol = elm327::hex_value(c);  // last hex digit
      }
      state.obd_protocol = protocol;
      can_ = protocol >= 6;
      probe_step_ = (supported_[0] & 1u) ? 2 : 4;  // bit 0 = PIDs 21-40 available
      return;
    }
    case 2:  // 0120
    case 3: {  // 0140
      const uint8_t pid = probe_step_ == 2 ? 0x20 : 0x40;
      const uint8_t index = probe_step_ == 2 ? 1 : 2;
      if (elm327::parse_pid(rx_, pid, data, sizeof(data), len) && len >= 4) {
        supported_[index] = static_cast<uint32_t>(data[0]) << 24 | static_cast<uint32_t>(data[1]) << 16 |
                            static_cast<uint32_t>(data[2]) << 8 | data[3];
      }
      probe_step_ = (probe_step_ == 2 && (supported_[1] & 1u)) ? 3 : 4;
      return;
    }
    default:
      return;
  }
}

void Elm327Link::on_pid_response(VehicleState& state, uint32_t now_ms) {
  const uint8_t index = req_arg_;
  const elm327::Reply reply = elm327::classify(rx_);
  if (reply == elm327::Reply::BusError) {
    ecu_lost(state, now_ms);
    return;
  }

  if (req_ == Req::Voltage) {
    const float volts = strtof(rx_, nullptr);
    if (volts > 3.0f && volts < 30.0f) state.set(SignalId::BatteryVoltage, volts, now_ms);
    return;
  }

  const uint8_t pid = kPoll[index].pid;
  uint8_t data[8];
  uint8_t len = 0;
  if (reply != elm327::Reply::Data || !elm327::parse_pid(rx_, pid, data, sizeof(data), len)) {
    if (reply == elm327::Reply::NoData) {
      if (++no_data_[index] == kMaxNoData) LOG_I("ELM", "PID %02X not available", pid);
      // Only NO DATA from every PID: the ECU went quiet (ignition off) without a bus error.
      if (++no_data_run_ >= kMaxNoDataRun) ecu_lost(state, now_ms);
    }
    return;
  }
  no_data_[index] = 0;
  no_data_run_ = 0;

  if (pid == kPidStatus) {
    const bool mil = (data[0] & 0x80) != 0;
    if (!state.dtc.mil_known || state.dtc.mil_on != mil) {
      state.dtc.mil_on = mil;
      state.dtc.mil_known = true;
      state.dtc.revision++;
    }
    return;
  }

  SignalId id;
  float value;
  if (!elm327::decode_pid(pid, data, len, id, value)) return;
  state.set(id, value, now_ms);

  // Without PID 5E the fuel rate is estimated from the air flow (petrol, lambda 1).
  if (id == SignalId::Maf && !pid_supported(kPidFuelRate)) {
    state.set(SignalId::FuelRate, value * kMafToLitresPerHour, now_ms);
  }
}

// ---- Diagnostic trouble codes --------------------------------------------------------------

bool Elm327Link::start_dtc(VehicleState& state, DtcOperation op) {
  if (phase_ != Phase::Running) {
    state.dtc.last_result = DtcResult::NotConnected;
    state.dtc.revision++;
    return false;
  }
  if (dtc_op_ != DtcOperation::Idle) return false;
  dtc_op_ = op;
  dtc_step_ = 0;
  dtc_count_ = 0;
  dtc_mil_ = state.dtc.mil_on;
  state.dtc.operation = op;
  state.dtc.revision++;
  return true;
}

bool Elm327Link::request_dtc_read(VehicleState& state, uint32_t) { return start_dtc(state, DtcOperation::Reading); }

bool Elm327Link::request_dtc_clear(VehicleState& state, uint32_t) {
  if (!start_dtc(state, DtcOperation::Clearing)) return false;
  dtc_step_ = 0xFF;  // mode 04 first, then read the codes again
  return true;
}

void Elm327Link::send_dtc_step(uint32_t now_ms) {
  if (dtc_step_ == 0xFF) {
    send("04", Req::Dtc, 0, kSearchTimeoutMs, now_ms);  // clearing can take a while on some ECUs
  } else {
    send(kDtcRead[dtc_step_].command, Req::Dtc, 0, kCommandTimeoutMs * 2, now_ms);
  }
}

void Elm327Link::on_dtc_response(VehicleState& state, uint32_t now_ms) {
  const elm327::Reply reply = elm327::classify(rx_);
  if (reply == elm327::Reply::BusError || (reply == elm327::Reply::Unknown && dtc_step_ == 0xFF)) {
    finish_dtc(state, now_ms, false);
    return;
  }

  if (dtc_step_ == 0xFF) {  // mode 04 answered "44"
    dtc_step_ = 0;
    return;
  }

  const DtcStep& step = kDtcRead[dtc_step_];
  if (step.reply == 0x41) {
    uint8_t data[4];
    uint8_t len = 0;
    if (elm327::parse_pid(rx_, kPidStatus, data, sizeof(data), len)) dtc_mil_ = (data[0] & 0x80) != 0;
  } else if (reply == elm327::Reply::Data) {  // NO DATA = no codes of this kind
    uint16_t codes[DtcState::kMaxCodes];
    const uint8_t n = elm327::parse_dtcs(rx_, step.reply, can_, codes, DtcState::kMaxCodes);
    for (uint8_t i = 0; i < n && dtc_count_ < DtcState::kMaxCodes; ++i) dtc_codes_[dtc_count_++] = {codes[i], step.status};
  }

  if (++dtc_step_ >= kDtcReadSteps) finish_dtc(state, now_ms, true);
}

void Elm327Link::finish_dtc(VehicleState& state, uint32_t now_ms, bool ok) {
  DtcState& dtc = state.dtc;
  if (ok) {
    dtc.count = dtc_count_;
    for (uint8_t i = 0; i < dtc_count_; ++i) dtc.codes[i] = dtc_codes_[i];
    dtc.mil_on = dtc_mil_;
    dtc.mil_known = true;
    dtc.scanned = true;
    dtc.last_scan_ms = now_ms;
    dtc.last_result = dtc_op_ == DtcOperation::Clearing ? DtcResult::ClearOk : DtcResult::ReadOk;
  } else {
    dtc.last_result = DtcResult::Failed;
  }
  dtc.operation = DtcOperation::Idle;
  dtc.revision++;
  dtc_op_ = DtcOperation::Idle;
}

}  // namespace core
