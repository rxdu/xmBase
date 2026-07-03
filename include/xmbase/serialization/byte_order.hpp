/*
 * byte_order.hpp
 *
 * Endian-aware load/store of fixed-width integers from/to a raw byte buffer.
 * Every protocol driver otherwise hand-rolls `(p[0] << 8) | p[1]` style packing;
 * these primitives make wire (de)serialization explicit and consistent, and
 * cannot silently disagree on endianness across drivers.
 *
 * These operate on caller-supplied buffers and do NO bounds checking — the
 * caller guarantees at least 2 (…16) or 4 (…32) bytes are available.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace xmotion::serialization {

// --- big-endian (network order) loads ------------------------------------
inline std::uint16_t load_be16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>((std::uint16_t(p[0]) << 8) | p[1]);
}
inline std::uint32_t load_be32(const std::uint8_t* p) {
  return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
         (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}

// --- little-endian loads --------------------------------------------------
inline std::uint16_t load_le16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>((std::uint16_t(p[1]) << 8) | p[0]);
}
inline std::uint32_t load_le32(const std::uint8_t* p) {
  return (std::uint32_t(p[3]) << 24) | (std::uint32_t(p[2]) << 16) |
         (std::uint32_t(p[1]) << 8) | std::uint32_t(p[0]);
}

// --- signed convenience (two's complement reinterpretation) --------------
inline std::int16_t load_be16s(const std::uint8_t* p) {
  return static_cast<std::int16_t>(load_be16(p));
}
inline std::int32_t load_be32s(const std::uint8_t* p) {
  return static_cast<std::int32_t>(load_be32(p));
}
inline std::int16_t load_le16s(const std::uint8_t* p) {
  return static_cast<std::int16_t>(load_le16(p));
}
inline std::int32_t load_le32s(const std::uint8_t* p) {
  return static_cast<std::int32_t>(load_le32(p));
}

// --- stores ---------------------------------------------------------------
inline void store_be16(std::uint8_t* p, std::uint16_t v) {
  p[0] = static_cast<std::uint8_t>(v >> 8);
  p[1] = static_cast<std::uint8_t>(v);
}
inline void store_be32(std::uint8_t* p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>(v >> 24);
  p[1] = static_cast<std::uint8_t>(v >> 16);
  p[2] = static_cast<std::uint8_t>(v >> 8);
  p[3] = static_cast<std::uint8_t>(v);
}
inline void store_le16(std::uint8_t* p, std::uint16_t v) {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
}
inline void store_le32(std::uint8_t* p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
  p[2] = static_cast<std::uint8_t>(v >> 16);
  p[3] = static_cast<std::uint8_t>(v >> 24);
}

}  // namespace xmotion::serialization
