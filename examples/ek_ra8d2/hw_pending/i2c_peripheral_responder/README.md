# i2c_peripheral_responder

RIIC (`ra8_i2c`) **target / peripheral** responder for the EK-RA8D2 -- the
target-role counterpart to `i2c_loopback` (which drives the RIIC controller
role). Added for issue #189.

The RA8D2 answers as an I2C target at 7-bit own address `0x42` on **RIIC
channel 1 (P512 SCL1 / P511 SDA1)** -- the board's Grove / Pmod / mikroBUS /
Arduino I2C bus. It arms the own-address match, attaches a callback
(`ra8_i2c_peripheral_attach_handler`) and services controller-driven transfers
by polling `ra8_i2c_peripheral_dispatch`:

- a controller **write** to `0x42` is drained (`ra8_i2c_peripheral_receive`) and
  captured, printing `riic-target: write serviced`;
- a controller **read** of `0x42` echoes the last captured write
  (`ra8_i2c_peripheral_transmit`), printing `riic-target: read serviced`.

Clock stretching (ICMR3.WAIT) is armed so the poll loop always has time to
service each byte before the controller clocks the next.

## Why an external controller

A single RA8D2 core cannot both clock a **blocking** controller transfer and
service its own target at the same time, so a two-channel self-loopback would
deadlock without target interrupts. This app therefore uses the polled
dispatch path and expects an **external** I2C controller on the bus. The RIIC
RXI / TXI / STPI interrupt event numbers live in FSP `bsp_elc.h` (not in this
tree), so the fully interrupt-driven single-board loopback is intentionally
left for a follow-up once those event numbers are pinned down and bench-checked.

## HIL status: pending (hw_pending)

The on-wire target role is **UNVERIFIED on silicon**. `ra8_emulator` models RIIC
only as a controller (see `tools/ra8_emulator/src/periph/board_periph_riic.c`), so it
cannot gate this app; the host MC/DC unit tests in
`tests/test_ra8_riic_peripheral.c` cover the driver logic instead.

## Bench setup (what the parent flashes + runs)

1. Wire an external I2C controller to **RIIC1**: `SDA1` (P511) <-> controller
   SDA, `SCL1` (P512) <-> controller SCL, common ground, and ~2.2-4.7 kOhm
   pull-ups to 3V3 on both lines. A Raspberry Pi on `/dev/i2c-1` works.
2. `make -C examples/ek_ra8d2/hw_pending/i2c_peripheral_responder flash`
3. Open the SCI8 VCOM at 115200 8N1. Expect the boot banner:
   `riic-target: armed 0x42 poll-dispatch`
4. From the controller, write then read address `0x42`, e.g. on a Pi:
   - `i2cset -y 1 0x42 0xDE 0xAD` (2-byte write)
   - `i2cget -y 1 0x42` (read back the echoed first byte, `0xDE`)
5. Expect `riic-target: write serviced` then `riic-target: read serviced` on
   SCI8, with LED1 toggling on each serviced transfer.

## See also

- `i2c_loopback` -- the RIIC controller self-test against on-board U15.
- `tests/test_ra8_riic_peripheral.c` -- host MC/DC tests for the target role.
