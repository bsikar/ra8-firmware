# da16600_probe

DA16600 (Wi-Fi 4 + BLE 5.0, US159-DA16600EVZ Pmod, Pmod1/J26) bring-up
ladder plus a self-contained physical-layer lab, reporting on the J-Link
OB VCOM (115200 8N1). One flash gives the complete climb:

```
alive (AT->OK) -> wifi scan -> BLE advertise -> AP join (hil_lab) -> TCP echo (:7)
```

Rungs self-gate: nothing past `alive` runs until the module answers.

## Built-in instruments (run every boot, evidence over VCOM)

| Instrument | What it proves |
|------------|----------------|
| `u15:` latch vs pins read-back | Whether the U15 expander's SW4 override is electrically winning |
| `wire:` P80x + side-band survey | Per-pin driven-high / driven-low / floating verdicts (bare + pull-up) |
| `sci2 self:` internal loopback | SCI2 MSTP/clock/BRR/TX/RX all good (CCR1.SPLP) |
| `sci2 self:` TXD2 PIDR sampling | The transmit signal physically toggles at the P801 pin |
| `edges:` GPIO sampler | ~1.3 M samples/s watch of P801/P802/P808/P809 across a module RESET pulse -- catches any TX at any baud, either pin orientation |
| `reset: ... banner dump` + `bootlog n=` | Boot-window RX capture, replayed every pass (survives VCOM dropout across power cycles) |
| `u15 sweep` (8 focused / 256 exhaustive) | Every firmware-selectable board routing, AT-probed |
| `ard: sci7` | Alt-location hunt: Arduino D0/D1 header UART |
| `sweep <baud>` | Multi-rate raw `AT` hunts (115200/230400/9600/57600/38400/921600) |

## Bench status (2026-06-11, see issue #86 for the full evidence chain)

Driver + TX path proven good; every UART landing point on the board
(Pmod1, Arduino D0/D1; mikroBUS/Grove unpopulated; Pmod2 = microSD) shows
**zero edges** across module resets and cold power-ons. The module is not
electrically present/active on any reachable pin. The fix is on the far
side of the MCU pins (seating / module power / the DA16600's own flashed
firmware image must be the UM-WI-046 AT image).

## Expected output once the wire is alive

```
da16600: alive ok
wifi: scan N=<count>
ble: adv ok
wifi: joined hil_lab ip=<lease>
tcp: echo ok            (needs the Pi hostapd fixture; scripts/hil_da16600_setup.md
                         is recoverable at 1be4943e^)
```
