/*
 * test_serialization.cpp — byte-order and checksum primitives.
 *
 * Checksums are pinned to their standard catalogue check values so they are
 * verified independently of any driver, and endian load/store are round-tripped
 * and cross-checked (be vs le) including signed reinterpretation.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <cstdint>

#include "gtest/gtest.h"

#include "xmsigma/serialization/byte_order.hpp"
#include "xmsigma/serialization/checksum.hpp"

using namespace xmotion::serialization;

TEST(ByteOrderTest, LoadBigAndLittleEndian) {
  const std::uint8_t b[4] = {0x12, 0x34, 0x56, 0x78};
  EXPECT_EQ(load_be16(b), 0x1234u);
  EXPECT_EQ(load_le16(b), 0x3412u);
  EXPECT_EQ(load_be32(b), 0x12345678u);
  EXPECT_EQ(load_le32(b), 0x78563412u);
}

TEST(ByteOrderTest, SignedReinterpretation) {
  const std::uint8_t be[2] = {0xFF, 0xFE};  // -2 as BE int16
  EXPECT_EQ(load_be16s(be), static_cast<std::int16_t>(-2));
  const std::uint8_t le[2] = {0xFE, 0xFF};  // -2 as LE int16
  EXPECT_EQ(load_le16s(le), static_cast<std::int16_t>(-2));
  const std::uint8_t be32[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  EXPECT_EQ(load_be32s(be32), -1);
}

TEST(ByteOrderTest, StoreRoundTrips) {
  std::uint8_t buf[4];
  store_be16(buf, 0xABCD);
  EXPECT_EQ(load_be16(buf), 0xABCDu);
  store_le16(buf, 0xABCD);
  EXPECT_EQ(load_le16(buf), 0xABCDu);
  store_be32(buf, 0xDEADBEEF);
  EXPECT_EQ(load_be32(buf), 0xDEADBEEFu);
  store_le32(buf, 0xDEADBEEF);
  EXPECT_EQ(load_le32(buf), 0xDEADBEEFu);
}

TEST(ChecksumTest, Crc8MaximStandardCheckValue) {
  const std::uint8_t v[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  EXPECT_EQ(crc8_maxim(v, sizeof(v)), 0xA1);  // CRC-8/MAXIM-DOW catalogue value
}

TEST(ChecksumTest, Crc16XmodemStandardCheckValue) {
  const std::uint8_t v[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  EXPECT_EQ(crc16_xmodem(v, sizeof(v)), 0x31C3u);  // CRC-16/XMODEM catalogue value
}

TEST(ChecksumTest, InvertedSum) {
  const std::uint8_t v[] = {0x01, 0x02};  // sum = 3 -> ~3 = 0xFC
  EXPECT_EQ(checksum_inverted_sum(v, sizeof(v)), 0xFC);
  const std::uint8_t z[] = {0x00, 0x00};
  EXPECT_EQ(checksum_inverted_sum(z, sizeof(z)), 0xFF);
}
