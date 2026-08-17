# threadx_sdcard_demo

A ThreadX bring-up smoke test: two threads at the same priority blinking two
different LEDs at two different rates. It exercises this repo's ThreadX port
glue -- the low-level init that sets the SysTick reload and fixes up priorities
-- against the vendored upstream Cortex-M85 context-switch code, and it drives
the same GPIO HAL calls the bare-metal blink does, only from an RTOS instead of
a busy-wait.

**Despite the directory name there is no SD-card code in it.** The app is a
copy of the ThreadX blink demo; the name is a leftover. For a real SD-backed
filesystem demo look at the SD and OSPI filesystem apps under `hw_validated/`.

It deliberately skips the clock bring-up and runs on the reset-default MOCO,
which the port's low-level init uses to compute the SysTick reload. The vector
table keeps this project's weak handler aliases and lets the upstream port
supply PendSV and SVC as strong symbols; the application overrides SysTick to
tail-call the ThreadX timer interrupt.

LED pins follow EK-RA8D2 v1 UM Table 24 p 31.
