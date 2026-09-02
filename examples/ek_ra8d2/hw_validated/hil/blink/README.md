# blink

Toggles user LED1 (P600, blue) at a clean 1 Hz off SysTick, with no busy-wait.

This is the smallest surface in the tree that can fail, which is the whole
point: it is the first thing to run on a new board or after a boot change,
because if `blink` is dark then nothing further up the stack is worth debugging.

It deliberately does not bring the CGC up. It runs at the reset-default MOCO
rate -- about 8.4 MHz, measured on `DWT.CYCCNT` and consistent with the
RA-family nominal 8 MHz -- and `k_blink_cpu_hz_at_reset` in `src/main.c` encodes
that. Cache, MPU and TrustZone bring-up are left out for the same reason.
`clock_check` is the app that takes the chip up to its rated CPUCLK0;
`blink_hal` is the same blink driven through the HAL instead.

The stack lives in the first on-chip SRAM bank only; the second bank needs its
own MSTPCR and SRAMSAR programming before anything can be placed there.

LED1 / P600 wiring follows EK-RA8D2 v1 UM (R20UT5523EG0101) Table 24 "EK-RA8D2
Board LED Functions" p 31.
