/*
 * base_types.hpp
 *
 * Compatibility facade. The contents were split into focused headers
 * (scalar.hpp, time.hpp, vector.hpp); this header re-exports them so existing
 * includes keep working. New code may include those directly, or the umbrella
 * xmsigma/types/types.hpp.
 *
 * Copyright (c) 2021-2026 Ruixiang Du (rdu)
 */

#pragma once

#include "xmsigma/types/scalar.hpp"
#include "xmsigma/types/time.hpp"
#include "xmsigma/types/vector.hpp"
