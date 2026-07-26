# uart_irq_echo

Interrupt-driven SCI8 UART echo: the counterpart to the poll-based `uart_hello`.
It registers the SCI8 RXI / TXI / TEI events through the ICU IELSR table with
`ra8_isr_register`, prints a one-line banner over the polled path
(`ra8_sci_write_polling`), then attaches an RX callback and arms an interrupt
receive (`ra8_sci_read`). Each received byte raises RXI; the ISR drives
`ra8_sci_dispatch_rxi`, the callback echoes the byte with `ra8_sci_write` (which
arms TIE so the echo streams out through TXI), re-arms the next receive, and
toggles LED1. The main loop only idles, so any echoed byte proves the real
NVIC -> ISR -> driver path ran.

On the EK-RA8D2 the SCI8 pins (PD02 TXD / PD03 RXD) surface as the J-Link OB
VCOM port; open it at 115200 8N1 and every character you type echoes back with
LED1 toggling.

This is the interrupt-driven UART proof target for `tools/ra8_emulator`: feed the
console RX with `--input` and the emulator raises RXI, takes the echo out
through TXI, and captures it as `[uart]` output.

```
make uart_irq_echo
./tools/ra8_emulator/build/ra8_emulator \
    examples/ek_ra8d2/hw_validated/hil/uart_irq_echo/build/uart_irq_echo.elf \
    --input "PING\r\n"
# stdout: [uart] SCI8: uart_irq_echo ready
#         [uart] SCI8: PING
```
