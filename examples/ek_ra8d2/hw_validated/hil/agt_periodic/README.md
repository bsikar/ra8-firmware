# agt_periodic

AGT 1 Hz periodic-tick + LED1 blink demo for the bare EK-RA8D2 EVM.

Starts AGT0 in free-running mode and toggles board LED1 (BLUE / P600)
on every counter underflow, logging `agt: tick` over the J-Link OB CDC
console (SCI8 @ 115200 8N1).

Build / flash:

```
make agt_periodic
make -C examples/ek_ra8d2/agt_periodic flash
```
