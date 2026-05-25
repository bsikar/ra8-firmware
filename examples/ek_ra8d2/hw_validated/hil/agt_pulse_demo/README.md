# agt_pulse_demo

AGT pulse-output / output-compare demo for the bare EK-RA8D2 EVM.

Arms AGT0 in pulse-output mode (HUM Ch 24.3.4 "Pulse Output Mode" p
1177) with compare-match A wired to AGTOAn. On every AGT0 underflow
the demo:

1. Stops + re-arms the channel to clear `AGTCR.TUNDF`.
2. Toggles board LED1 (BLUE / P600).
3. Logs `agt_pulse: tick=XXXXXXXX` over the J-Link OB CDC console
   (SCI8 @ 115200 8N1).

`XXXXXXXX` is the 32-bit tick count rendered big-endian as ASCII hex.
The HIL gate scrapes the prefix `agt_pulse: tick=`.

Build / flash:

```
make agt_pulse_demo
make -C examples/ek_ra8d2/hw_validated/hil/agt_pulse_demo flash
```
