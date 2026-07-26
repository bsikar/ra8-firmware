# lpm_idle_demo

Sleep-mode wake-count demo for the bare EK-RA8D2 EVM.

Brings up SCI8 + LED1 + the LPM block. Loops:

1. `ra8_lpm_enter_sleep(k_ra8_sleep_mode_sleep)` -- WFI; SysTick keeps
   running and wakes the core every 1 ms.
2. `ra8_delay_ms(100)` accumulates ~100 wakes.
3. Increments an in-RAM `s_wake_count`, toggles LED1, prints
   `lpm: wake_count=NNNNNNNN` on the J-Link OB CDC channel.

Sleep mode is the safest LPM mode for a bare-EVM HIL test --
SysTick keeps clocking, no external IRQ pin or RTC alarm has to be
wired up, and the J-Link debugger stays attached. Software-Standby
and Deep-Standby need an external wake source, which the bare
EK-RA8D2 cannot provide without a shield.

No external hardware required.

Build / flash:

```
make lpm_idle_demo
make -C examples/ek_ra8d2/lpm_idle_demo flash
```
