# icu_extint_demo

Routes the SW1 push-button (P009 -> IRQ13-DS) through the ICU,
configures it for falling-edge detection with the digital filter at
PCLKB / 64, and polls the IRQCR detect bit. Each press toggles LED1 and
emits `icu: irq13 press` over SCI8 (PD_02 / PD_03 -> J-Link OB CDC).

Build:

```
make icu_extint_demo
```

## HIL plan

**Requires physical stim -- needs external IRQ source on P009 (SW1).**
At boot the app emits no banner; UART output is gated on an SW1 press
which routes through IRQ13. The HIL bench has no Pi GPIO wired to
SW1, so the harness has no way to inject the falling edge needed to
trigger the ISR.

To make this HIL-able: wire a Pi GPIO to P009 (gated on SW1 not being
held by a human) and add a small `hil_gpio_stim` helper that drives
the pin low for ~50 ms after the firmware has had time to init. Then
a `uart_scrape` mode could assert `icu: irq13 press` appeared within
the timeout. Until that wiring exists this stays manual.

Relocated from `hw_validated/manual/` on 2026-05-19 -- author has not
yet bench-confirmed the IRQ -> banner path.
