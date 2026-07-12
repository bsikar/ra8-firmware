# wdt_window_demo

Demonstrates the IWDT in **window mode** (distinct from
`watchdog_demo`, which simply refreshes-until-stop). Polls the live
14-bit IWDTSR.CNTVAL counter via `ra8_iwdt_get_counter` and only writes
the refresh sequence when the counter sits inside the legal window
(`[k_wdt_window_demo_window_low, k_wdt_window_demo_window_high]`). LED1 toggles on
each in-window refresh; SCI8 logs `iwdt: refresh in window`.

The actual window bounds are programmed by the OFS0 option-setting
register at flash time -- the values in this demo are chosen to match
the conventional EK-RA8D2 OFS0 layout used by the project's shared
linker scripts.

Build:

```
make wdt_window_demo
```
