/**
 * @file elm327_link.h
 * Data source for ELM327 OBD-II adapters (Bluetooth or Wi-Fi).
 *
 * Talks the ELM327 AT/OBD command protocol over an ObdTransport, one command
 * at a time and without blocking:
 *
 *   adapter connect -> ATZ/ATE0/ATL0/ATS0/ATH0/ATAT1/ATSP0 -> 0100 (protocol
 *   search, ignition must be on) -> ATDPN, 0120, 0140 (supported PIDs)
 *   -> polling of mode 01 PIDs and ATRV, scheduled by priority.
 *
 * DTC requests run modes 0101 / 03 / 07 / 0A (and 04 to clear) between polls.
 * The adapter and ECU connections are reported separately in VehicleState
 * (adapter_state / link_state), so the UI can show both.
 */
#pragma once

#include <stdint.h>

#include "data_link.h"
#include "obd_transport.h"

namespace core {

/** Response parsing, exposed for testing with recorded adapter output. */
namespace elm327 {

enum class Reply : uint8_t { Data, NoData, BusError, Unknown, Stopped };

/** Classifies a complete response (text before the '>' prompt). */
Reply classify(const char* response);

/** Finds the mode 01 reply for `pid` ("41 <pid> A B ..."); copies the data bytes. */
bool parse_pid(const char* response, uint8_t pid, uint8_t* data, uint8_t max, uint8_t& len);

/**
 * Extracts DTCs from a mode 03 / 07 / 0A response (`reply_mode` 0x43 / 0x47 /
 * 0x4A). `can` selects the ISO 15765 layout (count byte after the mode byte).
 * Codes use the SAE J2012 encoding of core::Dtc. Returns the number of codes.
 */
uint8_t parse_dtcs(const char* response, uint8_t reply_mode, bool can, uint16_t* codes, uint8_t max);

/** Converts mode 01 data bytes to a signal value (base units). */
bool decode_pid(uint8_t pid, const uint8_t* data, uint8_t len, SignalId& id, float& value);

}  // namespace elm327

class Elm327Link final : public DataLink {
 public:
  void set_transport(ObdTransport* transport) { transport_ = transport; }

  const char* name() const override { return "elm327"; }
  void begin(VehicleState& state, uint32_t now_ms) override;
  void end(VehicleState& state) override;
  void poll(VehicleState& state, uint32_t now_ms) override;
  bool request_dtc_read(VehicleState& state, uint32_t now_ms) override;
  bool request_dtc_clear(VehicleState& state, uint32_t now_ms) override;

  static constexpr uint8_t kPollCount = 18;

 private:
  enum class Phase : uint8_t { Unconfigured, Adapter, AdapterRetry, Init, Probe, ProbeWait, Running };
  enum class Req : uint8_t { None, Init, Probe, Pid, Voltage, Dtc };

  void enter(Phase phase, VehicleState& state, uint32_t now_ms);
  void send(const char* command, Req req, uint8_t arg, uint32_t timeout_ms, uint32_t now_ms);
  void receive(VehicleState& state, uint32_t now_ms);
  void on_response(VehicleState& state, uint32_t now_ms);
  void on_timeout(VehicleState& state, uint32_t now_ms);
  void next_request(VehicleState& state, uint32_t now_ms);
  void on_probe_response(VehicleState& state, uint32_t now_ms);
  void on_pid_response(VehicleState& state, uint32_t now_ms);
  void on_dtc_response(VehicleState& state, uint32_t now_ms);
  void send_dtc_step(uint32_t now_ms);
  void finish_dtc(VehicleState& state, uint32_t now_ms, bool ok);
  void ecu_lost(VehicleState& state, uint32_t now_ms);
  bool pid_supported(uint8_t pid) const;
  bool start_dtc(VehicleState& state, DtcOperation op);

  ObdTransport* transport_ = nullptr;
  Phase phase_ = Phase::Unconfigured;
  uint32_t retry_at_ms_ = 0;

  // Command in flight.
  static constexpr uint16_t kRxSize = 512;
  char rx_[kRxSize] = {};
  uint16_t rx_len_ = 0;
  bool busy_ = false;
  Req req_ = Req::None;
  uint8_t req_arg_ = 0;
  uint32_t sent_ms_ = 0;
  uint32_t timeout_ms_ = 0;
  uint8_t timeouts_ = 0;

  // Initialisation / protocol.
  uint8_t init_step_ = 0;
  uint8_t init_attempts_ = 0;
  uint8_t probe_step_ = 0;
  uint32_t supported_[3] = {};  ///< PIDs 01-20, 21-40, 41-60 (bit 31 = first PID)
  bool can_ = false;

  // Polling.
  uint32_t last_poll_ms_[kPollCount] = {};
  uint8_t no_data_[kPollCount] = {};
  uint8_t no_data_run_ = 0;  ///< consecutive NO DATA replies over all PIDs

  // Diagnostic trouble codes.
  DtcOperation dtc_op_ = DtcOperation::Idle;
  uint8_t dtc_step_ = 0;
  Dtc dtc_codes_[DtcState::kMaxCodes] = {};
  uint8_t dtc_count_ = 0;
  bool dtc_mil_ = false;
};

}  // namespace core
