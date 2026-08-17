# threadx_blink

ThreadX bring-up on the EK-RA8D2: two same-priority threads blinking LED1 and
LED2 at different rates. It exercises the project's port glue under
`port/threadx/src/cortex_m85/` and the vendored upstream Cortex-M85 port,
driving the same GPIO paths `blink_hal` uses but from RTOS threads rather than
a busy-wait.

**Bring the CGC up before `tx_kernel_enter`.** The SysTick reload in
`tx_initialize_low_level.S` is computed for the 1 GHz clock. If the chip is
still on the boot-default MOCO (~8.4 MHz) when the scheduler starts, that same
reload takes about 119 ms of wallclock per tick instead of 1 ms, so a
half-second sleep stretches to about a minute and the LED looks frozen in any
reasonable observation window. A wrong clock here reads as a dead board, not as
a slow one.

`main.c` overrides `SysTick_Handler` to tail-call `_tx_timer_interrupt`;
`PendSV_Handler` and `SVC_Handler` arrive from the upstream port as strong
symbols and displace the weak aliases in `vector_table.c` automatically.

LED1 / LED2 are P600 / P303 per EK-RA8D2 v1 UM Table 24 p 31.
