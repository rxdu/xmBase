# xmBase examples — the API usage tour

Runnable reference usage of the telemetry API (and the common types), each mapped to the [guide](../docs/telemetry/guide.md) section and the [scenario](https://github.com/rxdu/xmTelemetry/blob/main/docs/scenarios.md) it mirrors. Build with `-DBUILD_EXAMPLES=ON` (or `XMOTION_DEV_MODE=ON`); binaries land in `build/bin/`.

| Example | Shows | Guide § | Mirrors |
|---|---|---|---|
| `xm_logging_example` | fmt + stream logging, runtime level control, compile floor | Logging | — |
| `instrumented_control_loop_example` | the RT hot-loop pattern: all 4 verbs + health, pre-registered handles | Metrics / RT checklist | S1 |
| `trace_pipeline_example` | one trace across threads: `NewTrace` → stages → envelopes (`Inject`/`Extract`) → fan-in links | Traces | S2 |
| `worker_pool_context_example` | context discipline on REUSED pool threads (`ContextGuard` per task — no trace pollution); fan-out/fan-in with span links | Traces | S2-A5 |
| `device_health_example` | the driver pattern: `EventSource` attribution, freshness gauge, hysteresis'd health transitions | Health | S3 |
| `custom_binding_example` | the binding seam made executable: a ~100-line backend capturing every verb (the blueprint's data flow, live) | reference.md → The seam | seam tests |
| `types_example` | the shared type vocabulary (`xmbase/types/`) | — | — |

The **application assembly** example (linking the real SDK: `Init`/`Shutdown`, sinks) lives with the SDK: [`xmTelemetry/examples/`](https://github.com/rxdu/xmTelemetry/tree/main/examples) — libraries instrument (this repo's examples), applications bind (that one).
