# kint_demo

Programs the on-board user switch SW1 (P009 -> IRQ13-DS, EK-RA8D2 v1 UM
Table 25 p 32) as a falling-edge external interrupt source via
`ra8_icu_configure_irq_pin`, then polls the switch and logs each press. It
polls deliberately rather than wiring the NVIC: the goal is to exercise the
IRQCRb programming path on a stock board with nothing plugged in.

As with `icu_extint_demo`, the press needs a human -- no bench GPIO is wired to
the SW1 net -- so the unattended gate can only scrape the banner emitted after
the IRQ is armed. That covers clock, console and ICU bring-up, not the edge.
