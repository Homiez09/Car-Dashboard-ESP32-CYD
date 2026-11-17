/**
 * @file obd_transport.h
 * Byte stream to an ELM327 OBD-II adapter (Bluetooth SPP or Wi-Fi TCP).
 *
 * Implemented in hal/ (hardware) and in the simulator (a scripted ELM327).
 * Connecting may take seconds, so open() only starts it; the caller polls
 * state(). read() and write() never block.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "types.h"

namespace core {

class ObdTransport {
 public:
  enum class State : uint8_t { Closed = 0, Connecting, Connected, Failed };

  virtual ~ObdTransport() = default;

  virtual AdapterTransport kind() const = 0;

  /** True if an adapter has been selected (Bluetooth device / Wi-Fi network). */
  virtual bool configured() const = 0;

  /** Starts connecting to the selected adapter. */
  virtual void open() = 0;

  /** Drops the connection (or a connection attempt). */
  virtual void close() = 0;

  virtual State state() = 0;

  /** Reads up to `len` received bytes; returns the number read. */
  virtual size_t read(uint8_t* buf, size_t len) = 0;

  /** Sends `text` unchanged (the caller appends the "\r" terminator). */
  virtual void write(const char* text) = 0;
};

}  // namespace core
