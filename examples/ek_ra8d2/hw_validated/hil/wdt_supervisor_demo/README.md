# wdt_supervisor_demo

Exercises `libs/ra8_wdt_supervisor` end to end on the bench: the WWDT is
refreshed only when **every** registered worker thread has called
`ra8_wdt_supervisor_checkin` inside its own deadline. Two synthetic ThreadX
workers check in on disjoint cadences while the supervisor thread, auto-spawned
by `ra8_wdt_supervisor_start`, ticks faster than either and decides each time
whether to refresh. A J-Link memprobe watches a liveness counter that worker A
bumps on every check-in, which catches a wedged scheduler, a supervisor that
failed to spawn, a check-in that returned an error, and a worker that died
before its first iteration.

Two deliberate deviations from `wdt_window_demo`:

- **NMI on expiry, not reset.** A supervisor bug then surfaces as a stalled
  liveness counter rather than a reset loop that keeps looking alive.
- **No refresh window** (start 100 %, end 0 %). The supervisor's refresh cadence
  follows its own tick, not the WWDT counter, so an artificial window would only
  read as a flaky test.

Registration and start happen inside worker A's first action rather than in
`tx_application_define`, so those APIs run with a valid thread context.
