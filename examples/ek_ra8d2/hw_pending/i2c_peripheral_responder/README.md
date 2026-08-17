# i2c_peripheral_responder

RIIC target/peripheral responder (#189) -- the target-role counterpart to
`i2c_loopback`, which drives the controller role. The RA8D2 answers as an I2C
target at 7-bit own address `0x42` on **RIIC channel 1** (P512 SCL1 / P511
SDA1), the board's Grove / Pmod / mikroBUS / Arduino I2C bus. It arms the
own-address match, attaches a handler, and services controller-driven transfers
by polling: a controller write is drained and captured, and a controller read
echoes the last captured write.

Clock stretching (ICMR3.WAIT) is armed so the poll loop always has time to
service each byte before the controller clocks the next.

## Why an external controller

A single RA8D2 core cannot both clock a **blocking** controller transfer and
service its own target at the same time, so a two-channel self-loopback would
deadlock without target interrupts. Hence the polled dispatch path and an
external controller on the bus. The RIIC RXI / TXI / STPI interrupt event numbers
live in FSP `bsp_elc.h`, which is not in this tree, so the fully
interrupt-driven single-board loopback is deliberately left until those event
numbers are pinned down and bench-checked.

## Blocked on

An external I2C controller wired to RIIC1 -- SDA1 and SCL1 to the controller,
common ground, and roughly 2.2-4.7 kOhm pull-ups to 3V3 on both lines. A
Raspberry Pi on `/dev/i2c-1` works. The on-wire target role is unverified on
silicon, and nothing off-target can arbitrate it: the emulator models RIIC only
as a controller. The driver logic is covered by host MC/DC tests in
`tests/test_ra8_riic_peripheral.c` instead.
