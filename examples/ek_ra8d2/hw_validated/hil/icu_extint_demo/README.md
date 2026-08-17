# icu_extint_demo

Routes the SW1 push-button (P009 -> IRQ13-DS) through the ICU with
falling-edge detection and the digital filter at PCLKB / 64, then polls the
IRQCR detect bit directly. Each press toggles LED1 and prints a line.

Presses need a human. No bench GPIO is wired to the SW1 net, so nothing
automated can inject the falling edge, and the unattended gate can only scrape
the banner emitted once after the IRQ is armed. That covers clock, console and
ICU bring-up -- not the interrupt itself. Wiring a stimulus GPIO to P009 is
what would close the gap.

`kint_demo` reaches the same pin through the board switch API instead of
reading IRQCR.
