# kint_demo

Key-interrupt (KINT / IRQ pin) input demo for the bare EK-RA8D2 EVM.

Configures the on-board user switch SW1 (P009 -> IRQ13-DS) as a
falling-edge external interrupt source via `ra_icu_configure_irq_pin`,
then logs `kint: SW1 press` over the J-Link OB CDC console (SCI8 @
115200 8N1) on every press.

Build / flash:

```
make kint_demo
make -C examples/ek_ra8d2/kint_demo flash
```

## HIL plan

**Requires physical stim -- needs SW1 button press.** Identical
situation to `icu_extint_demo`: no UART output at boot, all signal is
gated on a human press of SW1 (P009 / IRQ13). The HIL bench has no
Pi GPIO wired to the SW1 net.

Theoretical workaround: J-Link memprobe could write directly to the
ICU IRQ flag (force the interrupt pending bit) to fake an edge, but
that races with the ICU debounce hardware and is fragile. Cleaner
solution is a real Pi GPIO -> P009 jumper, then a `uart_scrape` mode
asserts `kint: SW1 press` appeared. Until that wiring exists this
stays manual.

Relocated from `hw_validated/manual/` on 2026-05-19 -- author has not
yet bench-confirmed the press -> banner path.
