# lpm_idle_demo

Counts wakes out of plain Sleep mode: enter WFI, let SysTick wake the core a
millisecond later, accumulate a hundred or so of those, then bump an in-RAM
counter, toggle LED1 and report.

Sleep is the only LPM mode a bare EVM can gate honestly. SysTick keeps clocking,
so no external IRQ pin or RTC alarm has to be wired up, and the J-Link debugger
stays attached across the WFI -- which is exactly what the deeper modes give up.
Software Standby and Deep Standby need a wake source the stock board cannot
provide without a shield, and they put the core somewhere the probe cannot
follow.

No external hardware required.
