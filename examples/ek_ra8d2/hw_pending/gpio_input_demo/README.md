# gpio_input_demo

SW1 -> LED1 mirror demo for the bare EK-RA8D2 EVM. Brings up CGC +
SysTick + LED1 + on-board user switch SW1, then polls SW1 in a loop
and drives LED1 to mirror its state (LED1 lit while SW1 is held).
No UART output -- LED1 is the only observable signal.

Build / flash:

```
make gpio_input_demo
make -C examples/ek_ra8d2/gpio_input_demo flash
```

## HIL plan

**Requires physical stim -- needs SW1 press or Pi GPIO on SW1 net.**
With no human pressing the button SW1 stays inactive and LED1 stays
off. The HIL bench has no Pi GPIO wired to the SW1 net (P009), so the
harness has no way to drive the input.

No UART output exists, so `uart_scrape` is not an option. A
`hil_check_alive` proves the chip is up but says nothing about the
GPIO-input path. A `jlink_memprobe` of a `g_sw1_observed_press`
counter would also report 0 forever without external stim.

To make this HIL-able: wire a Pi GPIO to P009 and add a
`hil_gpio_stim` helper, plus instrument main.c with a press-count
global for memprobe. Until that wiring exists this stays manual.

Relocated from `hw_validated/manual/` on 2026-05-19 -- author has not
yet bench-confirmed the SW1 -> LED1 path.
