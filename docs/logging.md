# Logging

Two front-ends, picked by deadline:

* **Soft-RT (default): `XLOG_*`** (`xlogger.hpp`) — async spdlog. The caller formats and enqueues; a worker thread does the I/O. A full queue drops the *oldest* record (`overrun_oldest`) rather than blocking. Use this for everything without a hard deadline.
* **Hard-RT: `XLOG_RT_*`** (`rt_logger.hpp`, `rt_logger_mpsc.hpp`) — a lock-free ring drained by a background thread. The producer path is wait-free (SPSC) / lock-free (MPSC), allocation-free, syscall-free (only a vDSO clock read), and never blocks; a full ring drops the *newest* record and bumps `dropped()`. Use this only inside loops with an explicit hard deadline.

## Environment variables

* XLOG_LEVEL: the log level. Default: 2
    - 0: Trace, 1: Debug, 2: Info, 3: Warn, 4: Error, 5: Fatal, 6: Off
* XLOG_ENABLE_LOGFILE: whether to enable the log file (TRUE/true/1 to enable). Default: false
* XLOG_FOLDER: the folder where log files are stored. Default: ~/.xmotion/log

## Hard-RT logging (`XLOG_RT_*`) notes

* **Level.** An `RtLogger`/`MpscRtLogger` seeds its level from `XLOG_LEVEL` (same as `XLOG_*`) and can be changed at runtime with `SetLevel()`. Below-threshold records are dropped on the hot path *before* the clock read and formatting, so a filtered call is nearly free. The compile-time `XMSIGMA_ACTIVE_LEVEL` floor still removes call sites entirely.
* **Drain thread placement.** Each logger owns one background drain thread that performs the sink I/O. On a tuned target, pin it to a housekeeping CPU and run it *below* the producer's real-time priority so it never preempts the control loop. If many producers need logging, share one logger (`MpscRtLogger`) across them rather than spawning a drain thread per loop.
* **Timestamps.** Records are stamped with `system_clock` (wall clock) for readable output; it can step backward on NTP/`settimeofday` adjustments. Do not derive loop timing from log timestamps — use `steady_clock`.
* **Format safety.** A malformed format string or argument mismatch is caught on the hot path and rendered as a bounded `[xmsigma: log format error]` diagnostic instead of throwing onto the RT thread. Keep hot-path arguments trivially formattable (arithmetic types, string views); a user-defined formatter that allocates would break the allocation-free guarantee.

## Known limitations

* You need to make sure you have write access to the folder you specified for the log files, if XLOG_ENABLE_LOGFILE is
  TRUE.
* Do not make any logging calls after a signal is received. This may result in an undefined behavior as memory may not
  be handled properly.