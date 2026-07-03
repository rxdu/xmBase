/*
 * checksum.hpp
 *
 * Canonical integrity-check routines shared by protocol drivers, which otherwise
 * each re-derive the same polynomials. Bitwise (table-free) implementations —
 * fine for the small frames these serve; swap in tables if a hot path ever needs
 * them.
 *
 * Variants provided (matched to the devices in the family):
 *   - crc8_maxim         CRC-8/MAXIM-DOW (poly 0x8C reflected, init 0x00) — DDSM-210, 1-Wire
 *   - crc16_xmodem       CRC-16/XMODEM  (poly 0x1021, init 0x0000)        — VESC
 *   - checksum_inverted_sum  ~(sum of bytes) & 0xFF                        — Feetech/SCServo bus servos
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace xmotion::serialization {

// CRC-8/MAXIM-DOW. Check value: crc8_maxim("123456789", 9) == 0xA1.
inline std::uint8_t crc8_maxim(const std::uint8_t* data, std::size_t len) {
  std::uint8_t crc = 0x00;
  for (std::size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b)
      crc = (crc & 0x01) ? static_cast<std::uint8_t>((crc >> 1) ^ 0x8C)
                         : static_cast<std::uint8_t>(crc >> 1);
  }
  return crc;
}

// CRC-16/XMODEM. Check value: crc16_xmodem("123456789", 9) == 0x31C3.
inline std::uint16_t crc16_xmodem(const std::uint8_t* data, std::size_t len) {
  std::uint16_t crc = 0x0000;
  for (std::size_t i = 0; i < len; ++i) {
    crc ^= static_cast<std::uint16_t>(std::uint16_t(data[i]) << 8);
    for (int b = 0; b < 8; ++b)
      crc = (crc & 0x8000) ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021)
                           : static_cast<std::uint16_t>(crc << 1);
  }
  return crc;
}

// Feetech / Dynamixel-style additive checksum: bitwise-NOT of the byte sum.
inline std::uint8_t checksum_inverted_sum(const std::uint8_t* data,
                                          std::size_t len) {
  std::uint8_t sum = 0;
  for (std::size_t i = 0; i < len; ++i) sum = static_cast<std::uint8_t>(sum + data[i]);
  return static_cast<std::uint8_t>(~sum);
}

}  // namespace xmotion::serialization
