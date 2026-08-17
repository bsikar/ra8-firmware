# power_profiler

Smoke test for `ra8_power_profile`, the enter/exit dwell-time accumulator. It
walks the four interesting RA8D2 power states in a loop -- a busy spin under
ACTIVE, then SLEEP, DEEP_SLEEP and SOFTWARE_STANDBY, each woken by SysTick --
and reports the accumulated dwell of each region. LED1 toggles per cycle and
LED2 latches on if any HAL call hard-fails.

The profiler takes its timestamps from `ra8_time_ms`, so the identical source
builds and runs on the host under `RA8_OFF_TARGET`. That also means what it
measures is dwell time, not current: the accumulator says how long the part sat
in each state, and a real power number still needs a current probe on the bench.

Bare EK-RA8D2; no external hardware.
