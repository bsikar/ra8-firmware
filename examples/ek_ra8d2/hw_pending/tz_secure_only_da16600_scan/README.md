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
checklist.

### Required SW4 DIP positions

Per EK-RA8D2 v1 UM Rev 1.01 Table 3 p 16 + Table 18 p 26, Pmod1's
mode is selected by SW4-1 and SW4-2:

| SW4 | Position | Reason                                                |
| --- | -------- | ----------------------------------------------------- |
| 1   | ON       | Pmod1 Mode-Sel-1 = UART (with SW4-2 OFF).             |
| 2   | OFF      | Pmod1 Mode-Sel-2 = UART (Table 18).                   |
| 3   | ON       | Octo-SPI Inactive -- frees Pmod1 SPI/UART pads.       |

The DA16600 Pmod card uses Pmod1's UART pair (TXD2/RXD2 on P801/P802),
so SW4-1 ON + SW4-2 OFF + SW4-3 ON is mandatory. Either flip the
physical DIPs to match, or call
`ra_board_io_expander_apply_project_sw4_defaults()` from `main()` so
the U15 expander drives the project's full SW4 layout regardless of
DIP positions. See "Project SW4 layout" in
`libs/ra_board_ek_ra8d2/inc/ra_board_ek_ra8d2.h`.
