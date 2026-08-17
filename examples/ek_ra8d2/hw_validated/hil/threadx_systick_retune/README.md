# threadx_systick_retune

Proves the ThreadX kernel tick stays 1 ms whatever CPUCLK0 the app happened to
boot at (#287).

`tx_initialize_low_level.S` programs SysTick.LOAD from a compile-time clock
assumption -- the post-CGC CPUCLK0 target. That is only correct if the app
raised CPUCLK0 to exactly that rate before `tx_kernel_enter()`. Enter the
kernel on the boot-default MOCO instead and the nominal 1 ms tick runs scaled
by the clock ratio, two orders of magnitude slow, silently stretching every
`tx_thread_sleep` and every timer.

`ra8_threadx_systick_retune()`, called from `tx_application_define`, re-derives
SysTick.LOAD from the *live* CPUCLK0 rate reported by `ra8_cgc_get_clock_hz`,
so the tick is right regardless of clock. A live clock too fast for the 24-bit
SYST_RVR returns an error rather than truncating.

A worker thread toggles LED1 (P600, EK-RA8D2 v1 UM Table 24 p 31) and bumps a
liveness counter. The app also recomputes the expected reload independently and
reads SYST_RVR back, raising a failure flag if the two disagree; a J-Link
memprobe watches both. The SYST_RVR write lands in the emulated System Control
Space as well, so the emulator runs the identical ARM path and its verdict
matches the on-hardware one.

No per-app `SysTick_Handler` override is needed: the shared weak handler in
`libs/ra8_core` already dispatches to `_tx_timer_interrupt` (#8), and
`PendSV_Handler` / `SVC_Handler` arrive as strong symbols from the upstream
ThreadX port.
