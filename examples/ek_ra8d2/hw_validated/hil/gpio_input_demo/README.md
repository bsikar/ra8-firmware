# gpio_input_demo

Polls the on-board user switch SW1 (P009) and drives LED1 to mirror it -- LED1
lit while SW1 is held. There is no UART output; LED1 is the only observable
signal.

Nobody presses a button on an unattended bench, and no stimulus GPIO is wired to
the SW1 net, so the automated gate cannot test the input path at all. It probes
the poll loop's tick counter over SWD instead, which proves the chip is up and
the loop is iterating and says nothing whatever about SW1 -> LED1. Confirming
that path needs a human at the board, or a driver wired to P009.
