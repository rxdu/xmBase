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

#include "xmsigma/types/scalar.hpp"
#include "xmsigma/types/time.hpp"
#include "xmsigma/types/vector.hpp"
#include "xmsigma/types/geometry.hpp"
#include "xmsigma/types/stamped.hpp"
