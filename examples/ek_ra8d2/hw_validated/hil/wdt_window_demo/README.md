# wdt_window_demo

Drives the WWDT in window mode: poll the down-counter and write the refresh
sequence only while it sits inside the legal window, which here is the middle
half of the period. LED1 toggles on each in-window refresh. A refresh outside
the window raises a window violation, so "refreshed at all" is not enough to
pass -- the window math has to be right.

This is the runtime-configurable watchdog. Unlike the IWDT, whose period is
fixed by OFS0 option-setting flash (`watchdog_demo`), the WWDT's timeout,
divider and both window bounds are programmed by `ra8_wdt_init` at boot.

One measured surprise: the WWDT counts on a slow base clock in the tens of Hz,
not on PCLKB as the HUM nomenclature suggests, so a nominally 1024-cycle period
runs for tens of seconds on the bench. Any timing assumption built on the PCLKB
reading will be wrong by orders of magnitude.
