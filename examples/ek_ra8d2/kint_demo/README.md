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
