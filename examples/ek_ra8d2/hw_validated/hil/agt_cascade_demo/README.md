# agt_cascade_demo

AGT0 + AGT1 cascade (32-bit virtual counter) demo for the bare
EK-RA8D2 EVM.

Arms the AGT0 + AGT1 pair via `ra_agt_start_cascade`. AGT0 in plain
timer mode counts PCLKB; its underflow feeds AGT1's count source
(HUM Ch 24.2.5 "AGTMR1" p 1168 note 6: TCK[2:0] = 101b on AGT1 only
== "Underflow event signal from AGT0"). On every AGT1 underflow the
demo:

1. Stops both halves + re-arms the cascade to clear AGT1's
   `AGTCR.TUNDF`.
2. Toggles board LED1 (BLUE / P600).
3. Logs `agt_cas: tick=XXXXXXXX` over the J-Link OB CDC console
   (SCI8 @ 115200 8N1).

`XXXXXXXX` is the 32-bit tick count rendered big-endian as ASCII hex.
The HIL gate scrapes the prefix `agt_cas: tick=`.

Build / flash:

```
make agt_cascade_demo
make -C examples/ek_ra8d2/hw_validated/hil/agt_cascade_demo flash
```
