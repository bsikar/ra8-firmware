# da16600_probe

DA16600 (Wi-Fi 4 + BLE 5.0, US159-DA16600EVZ Pmod in Pmod1/J26) bring-up
ladder: AT probe -> Wi-Fi scan -> BLE advertise, with physical-layer
diagnostics (U15 SW4 read-back, P80x line survey, reset-pulse banner dump,
baud sweep) printed on the J-Link OB VCOM.

## Hardware prerequisite (bench action!)

The Pmod1 UART only reaches SCI2 when the **physical** SW4 DIP is set to:

| SW4 | Position | Why |
|-----|----------|-----|
| 1   | ON       | Pmod1 mode = UART (EK UM Table 18) |
| 2   | OFF      | Pmod1 mode = UART |
| 3   | ON       | Octo-SPI inactive (frees P800-P804 from the OSPI mux) |

Measured on this bench (2026-06-11): the U15 expander override is
electrically overpowered by the mechanical DIP (`u15: latch=00F2
pins=0000`), so firmware cannot substitute for the flip. Note SW4-3=ON
disconnects the on-board Octo-SPI flash (the LevelX/FileX demos need it
back OFF).

## Expected output (after the flip)

```
da16600: probe boot
u15: latch=00F2 pins=00F2 (override WINNING)
...
da16600: alive ok
wifi: scan N=<count>
ble: adv ok
```
