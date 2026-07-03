/*
 * base_types.hpp
 *
 * Compatibility facade. The contents were split into focused headers
 * (scalar.hpp, time.hpp, vector.hpp); this header re-exports them so existing
 * includes keep working. New code may include those directly, or the umbrella
 * xmbase/types/types.hpp.
 *
 * Copyright (c) 2021-2026 Ruixiang Du (rdu)
 */

#pragma once

#include "xmbase/types/scalar.hpp"
#include "xmbase/types/time.hpp"
#include "xmbase/types/vector.hpp"
