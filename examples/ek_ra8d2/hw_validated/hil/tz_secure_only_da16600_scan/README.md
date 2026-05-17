# tz_secure_only_da16600_scan

Wi-Fi scan smoke test for the Renesas DA16600 module on a
US159-DA16600EVZ Pmod daughter card plugged into Pmod1 (J26) of the
EK-RA8D2 evaluation kit.

## What it does

1. Bring up CGC + SysTick + J-Link OB VCOM console (SCI8).
2. Bring up SCI2 in 8-N-1 mode at 115200 baud and route P801/P802 to
   TXD2/RXD2 (Pmod1 UART pins per EK-RA8D2 UM Table 17 p 26).
3. Initialize the `ra_da16600` driver against SCI2 and verify the
   module is alive (`AT` -> `OK`, UM-WI-046 section 2.1).
4. Issue `AT+WFSCAN` (UM-WI-046 section 4.1), parse the count of
   reported BSSIDs, and emit:

   ```
   wifi: scan complete, N=<n>
   ```

   on the J-Link VCOM channel.

## HIL contract

`hil.conf` declares `HIL_MODE=uart_scrape` with the expect string
`wifi: scan complete`. The harness flashes the .hex, watches the
VCOM serial output, and passes once that string appears within
`HIL_TIMEOUT_S=15` seconds.

## Hardware setup

Follow `scripts/hil_da16600_setup.md` for the Pmod jumper / wiring
checklist. No flashing is performed by this commit -- the binary is
built and verified to link clean only.
