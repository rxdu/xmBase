# xmBase examples — the API usage tour

Runnable reference usage of the telemetry API (and the common types), each mapped to its [guide](../docs/telemetry/guide.md) section. Build with `-DBUILD_EXAMPLES=ON` (or `XMOTION_DEV_MODE=ON`); binaries land in `build/bin/`.

| Example | Shows | Guide § |
|---|---|---|
| `xm_logging_example` | fmt + stream logging, runtime level control, compile floor | Logging |
| `instrumented_control_loop_example` | the RT hot-loop pattern: all 4 verbs + health, pre-registered handles | Metrics / RT checklist |
| `trace_pipeline_example` | one trace across threads: `NewTrace` → stages → envelopes (`Inject`/`Extract`) → fan-in links | Traces |
| `worker_pool_context_example` | context discipline on REUSED pool threads (`ContextGuard` per task — no trace pollution); fan-out/fan-in with span links | Traces |
| `device_health_example` | the driver pattern: `EventSource` attribution, freshness gauge, hysteresis'd health transitions | Health |
| `custom_binding_example` | the binding seam made executable: a ~100-line backend capturing every verb | reference.md → The seam |
| `types_example` | the shared type vocabulary (`xmbase/types/`) | — |

The **application assembly** examples (binding the real SDK) live with the xmTelemetry SDK — libraries instrument (this repo's examples), applications bind.
