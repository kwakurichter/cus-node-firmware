/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef CONSOLE_INPUT_H
#define CONSOLE_INPUT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint32_t lastByteMs;
  uint16_t remaining;
  uint8_t stage;
  bool version2;
} ConsoleMavlinkFilter;

/* Discard MAVLink framing, not decode it. Binary sequence/payload/checksum bytes
 * must never reach the single-character configuration menu. */
static inline bool consoleConsumeMavlink(ConsoleMavlinkFilter *filter, uint8_t byte, uint32_t nowMs)
{
  // Permit a terminal to recover from a truncated packet after an idle second.
  if (nowMs - filter->lastByteMs > 1000U) {
    filter->stage = 0;
  }
  filter->lastByteMs = nowMs;

  if (filter->stage == 0) {
    if (byte != 0xFE && byte != 0xFD) {
      return false;
    }
    filter->version2 = byte == 0xFD;
    filter->stage = 1;
  } else if (filter->stage == 1) {
    // Bytes remaining after magic and length: header, payload, and checksum.
    filter->remaining = byte + (filter->version2 ? 10U : 6U);
    filter->stage = filter->version2 ? 2 : 3;
  } else {
    if (filter->stage == 2) {
      // MAVLink 2 incompatibility bit 0 adds a 13-byte signature.
      if (byte & 1U) {
        filter->remaining += 13U;
      }
      filter->stage = 3;
    }
    if (--filter->remaining == 0) {
      filter->stage = 0;
    }
  }
  return true;
}

#endif
