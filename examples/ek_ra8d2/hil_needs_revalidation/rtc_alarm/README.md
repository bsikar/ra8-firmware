# rtc_alarm

Sets the on-chip RTC to a known seed time, schedules an alarm a few seconds out
through `ra8_rtc_set_alarm`, polls `RCR1.AIF` for the fire, logs over the
console, then advances the seed and re-arms. No NVIC line is involved -- the
polled form keeps the demo to the RTC itself.

The alarm line is the only success-only output: the boot line prints
unconditionally, so it proves nothing about the alarm. That distinction is what
an automated check has to key on.

The fire depends on the sub-clock crystal (SOSC), which is intermittently
populated on this EVM. On a board where SOSC is silent the seed is set and the
alarm is armed but never matches, so the app is healthy and the alarm line still
never appears -- rule that out before treating a miss as a driver defect.

No external hardware required.
