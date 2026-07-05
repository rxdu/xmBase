/*
 * telemetry/telemetry.hpp — the ONE header instrumented code includes.
 *
 * The XMotion telemetry API tier (ADR 0004): four verbs + a health
 * convention, stateless and std-only, callable identically from a 1 kHz
 * control loop and a planning thread. All machinery (rings, drain, sinks)
 * lives in the optional xmTelemetry SDK, bound at runtime via binding.hpp.
 *
 *   event()  — XM_INFO/XM_WARN/... macros, deferred-format records
 *   metric() — GetCounter/GetGauge/GetHistogram handles, atomic updates
 *   scope()  — XM_SPAN causal timing spans
 *   signal() — GetChannel<T>().Publish(), high-rate typed samples
 *   health   — ReportHealth(subsystem, state, detail)
 *
 * Unbound contract (no SDK): events ≥ Warn and non-Ok health → stderr;
 * everything else is a safe no-op. See docs: the scenario suite in the
 * xmTelemetry repo is the executable specification of this API.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include "xmbase/telemetry/binding.hpp"
#include "xmbase/telemetry/context.hpp"
#include "xmbase/telemetry/event.hpp"
#include "xmbase/telemetry/handles.hpp"
#include "xmbase/telemetry/health.hpp"
#include "xmbase/telemetry/span.hpp"
#include "xmbase/telemetry/time.hpp"
