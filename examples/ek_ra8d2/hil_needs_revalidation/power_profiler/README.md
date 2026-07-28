# power_profiler

Power-mode profiler smoke app for the EK-RA8D2. Drives
`ra8_power_profile` (DTS-style enter/exit accumulator) across the
four interesting RA8D2 low-power modes -- ACTIVE, SLEEP,
DEEP_SLEEP, and SOFTWARE_STANDBY -- and reports the dwell time of
each region over SCI8.

Each cycle:

1. Spin a busy-loop while `ACTIVE` is open.
2. Enter SLEEP via `ra8_lpm_enter_sleep(k_ra8_sleep_mode_sleep)`;
   SysTick wakes the CPU within a millisecond.
3. Same for DEEP_SLEEP and SOFTWARE_STANDBY.
4. Print accumulator stats once a second.

SCI8 (115200 8N1, J-Link OB CDC) prints

```
pp: a=NN s=NN d=NN st=NN us
```

LED1 toggles per cycle; LED2 latches ON if any HAL call hard-fails.

## Build + flash

```sh
make power_profiler
make -C examples/ek_ra8d2/power_profiler flash
```

Bare EK-RA8D2 only -- the profiler hooks into `ra8_time_ms` for the
timestamp source so the same binary works on the host test build
under `RA8_OFF_TARGET`.
