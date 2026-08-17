# lpm_software_standby_demo

Software Standby (`LPSCR.LPMD = 0x5`) with an RTC alarm as the wake source, on a
bare EK-RA8D2. Software Standby is the first of the truly deep LPM states:
almost every clock domain is gated including the CPU and SysTick, and the only
things left running are the sub-clock (SOSC) and the always-on wake-up detectors
it feeds.

Each pass reads the RTC, arms the alarm a few seconds ahead with `RCR1.AIE`,
sets `WUPEN0.RTCALMWUPEN` so the alarm cancels standby, enters standby, and on
wake bumps `g_lpm_swstd_wake_count` and clears the alarm flag.

## The SOSC caveat

The sub-clock crystal on this EVM has been observed to be intermittent -- the
same failure mode that dogged Ethernet bring-up here. If SOSC is not ticking
when standby is entered, the alarm never fires and the WFI hangs until an
external reset. Because bench truth varies run to run, the automated check is
deliberately lenient: it confirms the firmware built, the CGC came up and the
standby-entry path executed, and does not claim the RTC alarm woke the chip.
Confirming the wake itself means a debugger or a scope on a board with a live
crystal, watching `g_lpm_swstd_wake_count` advance.

No external hardware required.
