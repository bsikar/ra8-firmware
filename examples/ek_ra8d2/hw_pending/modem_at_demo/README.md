# modem_at_demo

Drives the `libs/ra8_modem_at` cellular AT command/response driver against a 3GPP
AT-command modem (SIM7600 / Quectel BG95 class) over the MikroBUS UART on SCI
channel 7 (#259) -- a MikroE cellular Click presents its modem UART on exactly
those pads.

It brings SCI7 up, registers a `+CREG` URC handler, and walks a small state
machine: probe and disable echo and select numeric CME errors; check the SIM;
read signal strength; enable and poll registration; check PS attach. The last
step is the interesting one -- it issues a command the modem does not support and
treats the resulting `+CME ERROR` as the **expected** outcome, so the error path
through the driver is asserted rather than merely avoided. Any unexpected step
failure latches LED2 and parks.

The app uses the central `ra8_board_*` console and MikroBUS pin helpers rather
than hand-encoded pins, and it carries no per-app boot files or linker script:
`cmake/ra8_add_app.cmake` supplies the shared startup and the canonical
single-core linker script, so only `main.c` is app-specific. The compound
decisions -- registration OK, signal valid, per-step expected outcome, overall
verdict -- are host-tested with MC/DC in `tests/test_app_modem_at_demo.c`.

## Blocked on

A live cellular modem on the wire: a populated MikroBUS cellular Click with a
provisioned SIM and an antenna, none of which is on the bench.

The **AT protocol itself is faithfully modelled** off-target -- an SCI7
AT-responder answers the exact script above, including the `+CREG` URC and the
`+CME ERROR` for the unsupported command -- so `ra8_modem_at` -> `ra8_sci` runs
byte-for-byte as it would on silicon and the state machine completes with zero
skips. Only the RF link is unmodelled, and the antenna is the only unproven link
in the chain.
