/*
 * scalar.hpp
 *
 * Fundamental scalar aliases shared across the xMotion family. Dependency-free
 * (no Eigen, no spdlog) so the lowest layers — including wire/driver structs —
 * can use them without pulling in heavy headers.
 *
 * Copyright (c) 2021-2026 Ruixiang Du (rdu)
 */

#pragma once

#include <cstdint>

namespace xmotion {

// Underlying type for scoped enums that need a fixed, wire-stable width.
using EnumBaseType = std::uint32_t;

// Deprecated legacy spelling — kept for source compatibility. Prefer
// EnumBaseType in new code.
using RSEnumBaseType = EnumBaseType;

}  // namespace xmotion
