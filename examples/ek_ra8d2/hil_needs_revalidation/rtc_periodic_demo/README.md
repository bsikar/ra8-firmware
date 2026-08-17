# rtc_periodic_demo

Extends `rtc_alarm` to the RTC's periodic interrupt: after programming the same
few-seconds-out calendar alarm it enables **both** the alarm and periodic IRQ
flags (`RCR1.AIE` and `RCR1.PIE`), then polls `ra8_rtc_get_status` for either
and logs a tick on whichever fires first. On real silicon `RCR1.PIE` drives an
NVIC line; this demo stays polled so the RTC is the only thing under test.

Enabling both flags in one `ra8_rtc_set_irq_enable` call with a combined mask is
the specific thing this app adds over its sibling -- that the driver accepts and
honours a dual-flag mask.

Like `rtc_alarm`, the fire depends on the sub-clock crystal (SOSC), which is
intermittently populated on this EVM. An automated check can therefore only
insist on the boot line, which proves the RTC came up and the dual-flag mask was
accepted; the tick itself needs a board with a live crystal.

No external hardware required.
