# icu_extint_demo

Routes the SW1 push-button (P009 -> IRQ13-DS) through the ICU,
configures it for falling-edge detection with the digital filter at
PCLKB / 64, and polls the IRQCR detect bit. Each press toggles LED1 and
emits `icu: irq13 press` over SCI8 (PD_02 / PD_03 -> J-Link OB CDC).

Build:

```
make icu_extint_demo
```
