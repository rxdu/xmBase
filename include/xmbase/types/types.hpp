/*
 * types.hpp
 *
 * Umbrella for the xMotion common type vocabulary. Include this to get the full
 * ergonomic set (scalars, time, POD vectors, Eigen geometry, Stamped<T>).
 *
 * GEOMETRY-TIER HEADER (0.5.0 target split, ADR 0007): this umbrella pulls
 * geometry.hpp and therefore Eigen — including it requires linking
 * xmotion::xmBaseGeometry. Core-tier (Eigen-free) consumers include the
 * granular headers they need instead: scalar.hpp, time.hpp, vector.hpp,
 * stamped.hpp (or the base_types.hpp facade, which bundles exactly those).
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
