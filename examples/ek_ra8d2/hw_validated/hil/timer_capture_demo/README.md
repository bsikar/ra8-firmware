# timer_capture_demo

GPT free-running timer capture demo for the bare EK-RA8D2 EVM.

Brings up SCI8 + GPT0 in free-run mode. Once a second snapshots the
GPT counter, toggles LED1, sleeps 50 ms, snapshots again, and prints
`gpt: period=NNNNNNNN` (8 hex digits) on the J-Link OB CDC channel.

The "capture" here is "snapshot the counter twice across a known
software delay" rather than the silicon's GTIOC-edge input-capture
mode, because the bare EK-RA8D2 has no external GTIOC source wired
without a shield.

No external hardware required.

Build / flash:

```
make timer_capture_demo
make -C examples/ek_ra8d2/timer_capture_demo flash
```
