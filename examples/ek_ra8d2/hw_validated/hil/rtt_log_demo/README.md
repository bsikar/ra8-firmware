# rtt_log_demo

Minimal SEGGER RTT logging demo: allocates an RTT control block in SRAM and
writes numbered lines through the in-tree `rtt_write` shim, toggling LED1
alongside each one.

The output leaves over SWD, not UART. The J-Link OB CDC port stays silent, so
anything that scrapes a serial console sees nothing at all here -- a host-side
`JLinkRTTViewer` / `JLinkRTTLogger` is what captures it, and the automated
check reads the up-buffer region out of memory over J-Link instead of a serial
port.

The emulator models the host side of that protocol: it scans emulated SRAM for
the `"SEGGER RTT"` control-block ID exactly as the J-Link OB does, drains
up-buffer 0 respecting the ring wrap, and stores the advanced read offset back
-- so the same banner check runs headlessly.
