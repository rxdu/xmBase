<h1 align="center">
  <img src="docs/xmsigma.svg" width="96" alt="xmSigma"><br>
  xmSigma&nbsp;·&nbsp;Σ
</h1>

<p align="center"><b>Foundation layer for the xMotion family</b> — interfaces, logging and common types.<br>
The substrate every other component plugs into.</p>

---

`xmSigma` is the Σ foundation of the **xMotion** product family. It provides the low-level
contracts and utilities shared across components:

- **Interfaces** — hardware-driver interfaces (motors, sensors, CAN, serial, …) and control interfaces.
- **Logging** — an spdlog-based logging system configurable via environment variables.
- **Common types** — geometry and trajectory types used throughout the stack.

It builds either standalone or embedded as a module in another project, and ships its own CI +
Debian packaging so downstream components can consume released artifacts rather than source.

> Part of the xMotion family — see the [umbrella](https://github.com/rxdu/xmotion). Sibling
> components include [xmNabla](https://github.com/rxdu/xmNabla) (motion algorithms) and
> [xmMu](https://github.com/rxdu/xmMu) (host hardware drivers).

## Modules

| Module          | Description                                                        |
|-----------------|-------------------------------------------------------------------|
| `src/interface` | hardware-driver and control interface definitions + common types  |
| `src/logging`   | spdlog-based logging with `XLOG_*` environment configuration       |

## Build

```bash
mkdir build && cd build
cmake ..
make -j
```

Key options: `BUILD_TESTING` (build tests, default `OFF`), `ENABLE_LOGGING` (default `ON`),
`USE_SYS_SPDLOG` (use system spdlog, default `ON`).

## Logging configuration

* `XLOG_LEVEL`: 0–6 (0: TRACE, 1: DEBUG, 2: INFO, 3: WARN, 4: ERROR, 5: FATAL, 6: OFF)
* `XLOG_ENABLE_LOGFILE`: 0 or 1
* `XLOG_FOLDER`: folder for log files (default `~/.xmotion/log`)

## License

Apache-2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE). First-party code only; bundled
third-party components retain their own licenses.
