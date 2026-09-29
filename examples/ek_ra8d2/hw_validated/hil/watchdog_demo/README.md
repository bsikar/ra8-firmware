# watchdog_demo

Round-trips the IWDT and the reset-cause machinery: log the latched cause on
boot (power-on, IWDT, or other), refresh the IWDT for half a minute with LED1
toggling per refresh, then deliberately stop feeding it. The counter underflows,
the chip resets, and the next boot reads back `iwdt` -- so a pass depends on the
reset actually firing *and* on the cause flag being read correctly afterwards.

The IWDT period is not configurable at runtime: it comes from the OFS0
option-setting register written at flash time. This app carries no
`linker_script.ld` of its own, so `ra8_add_app` falls back to the board map
`libs/ra8_board_ek_ra8d2/ld/linker_script.ld`, and a linker script only *places*
the OFS0 word at its fixed MRAM address. The word itself is
`BSP_CFG_OPTION_SETTING_OFS0` in `libs/ra8_hal/src/ra8_ofs.c`, whose in-tree
default is the erased `0xFFFFFFFF`, so no window value is asserted here: a
deployment sets one by overriding that macro at build time. That flash-time
fixed period is the difference between this app and `wdt_window_demo`, which
drives the separate WWDT.
