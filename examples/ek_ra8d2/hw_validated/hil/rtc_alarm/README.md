# rtc_alarm

RTC + alarm + UART log demo for the bare EK-RA8D2 EVM.

Sets the on-chip RTC to 2026-01-01 00:00:00, schedules an alarm five
seconds in the future via the new `ra8_rtc_set_alarm` API, polls
RCR1.AIF for the fire, then logs `rtc: alarm fired` over the J-Link OB
CDC port (SCI8 @ 115200 8N1) and re-arms ten seconds later.

Build / flash:

```
make rtc_alarm
make -C examples/ek_ra8d2/rtc_alarm flash
```

Open `picocom -b 115200 /dev/cu.usbmodem...` (macOS) or
`minicom -D /dev/ttyACM0 -b 115200` (Linux) to see the log lines.
