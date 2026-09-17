# watchdog_demo

Round-trips the IWDT and the reset-cause machinery: log the latched cause on
boot (power-on, IWDT, or other), refresh the IWDT for half a minute with LED1
toggling per refresh, then deliberately stop feeding it. The counter underflows,
the chip resets, and the next boot reads back `iwdt` -- so a pass depends on the
reset actually firing *and* on the cause flag being read correctly afterwards.

The IWDT period is not configurable at runtime: it comes from the OFS0
option-setting register written at flash time, which is why the app's shared
linker script sets a multi-second window. That fixed period is the difference
between this app and `wdt_window_demo`, which drives the separate WWDT.
