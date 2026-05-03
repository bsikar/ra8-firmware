# ulpt_demo

Ultra-Low-Power Timer (ULPT) 1 Hz wake-from-software-standby demo for
the bare EK-RA8D2 EVM.

Brings up ULPT0 in 1 Hz mode and uses its underflow event as a wake
source. Each underflow logs `ulpt: wake` over the J-Link OB CDC
console (SCI8 @ 115200 8N1) and re-arms.

Build / flash:

```
make ulpt_demo
make -C examples/ek_ra8d2/ulpt_demo flash
```
