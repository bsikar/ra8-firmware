<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 Brighton Sikarskie
-->

# power_profile_stats_demo

First consumer of `libs/ra8_power_profile` outside the unit tests.

`ra8_power_profile` accounts for time spent in each power region behind two
caller-supplied hooks: a GPIO edge emitter and a microsecond time base. Both
are injected, so the whole library runs with no clock peripheral and no scope
on a pin. This app supplies a synthetic clock it advances by hand and an edge
hook that records what it was asked to emit, then checks each behaviour the
header promises.

## What it checks

| Leg          | Check                                                                    |
| ------------ | ------------------------------------------------------------------------ |
| `timeline`   | Every `mark_enter` / `mark_exit` is accepted.                            |
| `accounting` | Two disjoint active stays sum; the idle gap between them is not counted. |
| `accounting` | An open region reports `is_open`, keeps `last_enter_us`, accrues 0 us.   |
| `edges`      | The edge hook fires twice per closed region, once per polarity.          |
| `reset`      | `reset_stats` zeroes the accumulators; hooks stay wired afterwards.      |

## Timeline

The clock starts at 1000 us and only this app moves it:

```
active  [1000 .. 1250]        250 us, closed
(gap)   [1250 .. 1290]         40 us, no region open
active  [1290 .. 1440]        150 us, closed   -> active total 400 us
sleep   [1440 .. 2340]        900 us, closed
standby [2340 ..     ]        500 us elapsed, NEVER EXITED -> total 0 us
```

After the reset, one more closed active stay of 75 us must land on a clean
slot (entries 1, exits 1, total 75 us).

## Determinism

Nothing here is measured off real silicon. The time base is a counter this
file owns and no GPIO is configured, so the expected totals are arithmetic on
the constants in `pp_const_t` rather than a measurement. A board is needed
only to confirm the console path, which is why this lives in `hw_pending`.

## Build and run

```sh
cmake --preset ra8d2-debug
ninja -C cmake-build-debug power_profile_stats_demo.elf
```

Flash and watch SCI8 (J-Link OB VCOM, 115200 8N1). A good run prints one
verdict per leg then:

```
power_profile_stats_demo: ALL PASS
```

## Related

- `libs/ra8_power_profile/inc/ra8_power_profile.h`
- `tests/misc/src/test_ra8_power_profile.c`
- `examples/ek_ra8d2/hil_needs_revalidation/power_profiler/`
