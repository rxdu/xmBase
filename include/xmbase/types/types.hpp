/*
 * types.hpp
 *
 * Umbrella for the xMotion common type vocabulary. Include this to get the full
 * ergonomic set (scalars, time, POD vectors, Eigen geometry, Stamped<T>).
 *
 * The opt-in strong-typed quantities (quantities.hpp) are intentionally NOT
 * included here — pull that header in explicitly where you want them.
 *
 * Copyright (c) 2021-2026 Ruixiang Du (rdu)
 */

#pragma once

#include "xmbase/types/scalar.hpp"
#include "xmbase/types/time.hpp"
#include "xmbase/types/vector.hpp"
#include "xmbase/types/geometry.hpp"
#include "xmbase/types/stamped.hpp"
