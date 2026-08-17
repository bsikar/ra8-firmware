# lpm_deep_standby_2_demo

Deep Software Standby variant 2 (`LPSCR.LPMD = 0x9`) entry on a bare EK-RA8D2.
Variant 2 stops the voltage-monitor and sub-clock-detection domains that variant
1 (`lpm_deep_standby_1_demo`) leaves running, trading wake-up coverage for lower
standby current (HUM Ch 11.1 Table 11.3 p 431-432). `lpm_deep_standby_3_demo`
stops the LOCO on top of that.

The demo seeds the RTC, arms an alarm a few seconds out, programmes the
deep-standby cancel matrix (`DPSIER2.DRTCAIE` so the alarm cancels deep standby,
`WUPEN0.RTCALMWUPEN` so it is armed in the wake-up matrix), and enters standby.

**Waking from deep standby is a reset, not a resume.** Control lands back in
`Reset_Handler` and the whole bring-up runs again, so the observable signal is a
boot banner that re-emits on every wake rather than anything after the WFI. On a
board whose sub-clock crystal is silent the banner emits exactly once and the
WFI never returns.

That crystal is the standing caveat, shared with `lpm_software_standby_demo`:
the RTC alarm runs off SOSC, which is intermittent on this EVM, so the automated
check is deliberately boot-banner-only. It verifies the build, the bring-up and
the standby-entry path; the wake itself needs a board with a live crystal. A
core halted in deep standby is also unreachable to the probe, which is the other
reason this is hard to gate automatically.

No external hardware required.
