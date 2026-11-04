/**
 * @file sim_elm.h
 * Scripted ELM327 adapter for the simulator.
 *
 * Answers AT and OBD-II commands like a v1.5 clone on an ISO 15765-4 CAN car
 * (echo until ATE0, spaces until ATS0, "SEARCHING..." on the first 0100,
 * multi-frame mode 03 reply). Live values come from the demo vehicle model,
 * so the Elm327Link code path can be exercised and checked without hardware.
 */
#pragma once

#include <string>

#include "../../CarCYD/src/core/obd_transport.h"
#include "../../CarCYD/src/core/sim_link.h"
#include "../../CarCYD/src/core/vehicle_state.h"

namespace sim {

class FakeElm327 final : public core::ObdTransport {
 public:
  explicit FakeElm327(core::AdapterTransport kind) : kind_(kind) {}

  core::AdapterTransport kind() const override { return kind_; }
  bool configured() const override { return true; }
  void open() override;
  void close() override;
  State state() override;
  size_t read(uint8_t* buf, size_t len) override;
  void write(const char* text) override;

  /** The vehicle model behind the adapter (for checks against parsed values). */
  const core::VehicleState& model() const { return model_; }

 private:
  void execute(const std::string& command);
  std::string pid_reply(uint8_t pid);
  void reply(const std::string& command, const std::string& body, uint32_t delay_ms);

  core::AdapterTransport kind_;
  State state_ = State::Closed;
  uint32_t connected_at_ = 0;
  std::string line_;
  std::string out_;
  uint32_t out_ready_at_ = 0;
  bool echo_ = true;
  bool spaces_ = true;
  bool searched_ = false;
  bool codes_cleared_ = false;
  core::SimLink car_;
  core::VehicleState model_;
};

}  // namespace sim
