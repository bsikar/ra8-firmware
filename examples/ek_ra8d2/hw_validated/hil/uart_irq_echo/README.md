# uart_irq_echo

Interrupt-driven SCI8 echo, the counterpart to the poll-based `uart_hello`. It
registers the SCI8 RXI / TXI / TEI events through the ICU IELSR table, then
attaches an RX callback and arms an interrupt receive. Each received byte raises
RXI; the callback echoes it back (arming TIE so the echo streams out through
TXI), re-arms the next receive, and toggles LED1. The main loop only idles, so
any byte that comes back proves the real NVIC -> ISR -> driver path ran.

The SCI8 pins (PD02 TXD / PD03 RXD) surface as the J-Link OB VCOM port; type
into it at 115200 8N1 and every character echoes.

This is also the interrupt-driven UART proof target for the emulator: feed its
console RX and the emulator raises RXI, takes the echo out through TXI, and
captures it as UART output.
