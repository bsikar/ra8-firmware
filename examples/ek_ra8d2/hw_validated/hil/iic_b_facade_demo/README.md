# iic_b_facade_demo

IIC_B (the RA8D2 I3C block run in legacy I2C-compatibility mode -- the twin of
the RIIC `ra8_i2c` path) exercised through the `ra8_io_i2c_bus` facade in BOTH
roles the peripheral supports: **controller** and **peripheral (target)**.

Part of the #252 example-coverage sweep; closes #255.

## What it does

On channel 0 (the single I3C/IIC_B instance), once at boot:

1. **Controller.** Binds the facade with `ra8_io_i2c_bus_bind_i3c_compat`, then
   runs the textbook "address a register, read its contents" pattern via
   `ra8_io_i2c_bus_transfer`: write the 2-byte register pointer `0x8140`,
   repeated START, read 4 bytes. The target is the on-board GoodIX **GT911**
   touch controller (7-bit `0x5D`); register `0x8140` is its `PRODUCT_ID`
   (`"911\0"`), the exact bring-up probe `ra8_touch` performs -- so a real
   device answers on a stock EK-RA8D2 (proven by `touch_demo`).

2. **Peripheral (target).** Reprograms the same channel as an addressed I2C
   target (own address `0x42`) with `ra8_i3c_peripheral_open`, then services
   controller write-then-read round-trips: it drains a byte the bus controller
   wrote (`NTST.RDBFF0`) and echoes it back on the following controller-read
   (`NTST.TDBEF0`).

Each second the SCI8 J-Link OB console prints one banner:

```
iic_b facade: up ctrl=OK id0=0x39 periph=4
```

`iic_b facade: up` is the deterministic, device-independent gate substring:
reaching the loop proves `ra8_i3c_init` + the facade bind succeeded on silicon.
`ctrl=` is the controller outcome (`OK` / `NAK` / `IDLE`) and `id0=` the first
`PRODUCT_ID` byte; `periph=` is the completed peripheral round-trip count.

## The facade point

The controller half never names `ra8_i3c_*` at the call site -- it goes through
`ra8_io_i2c_bus_transfer`. A board revision that moved this bus onto a RIIC
channel would swap only the one `ra8_io_i2c_bus_bind_i3c_compat` call for
`ra8_io_i2c_bus_bind_riic`; every transfer stays byte-identical. That is the
Liskov-substitutable "twin I2C backends" property the facade exists to provide.

## SIM vs bench

`tools/ra8_emulator` (`board_periph_i2c.c`) models the IIC_B controller bus (the
GT911 answers `PRODUCT_ID`) AND plays the **external** I2C controller that
drives the firmware's target role, so BOTH halves run headless: the controller
reads `id0=0x39` and the peripheral count is non-zero. `scripts/sim/sil_all.sh`
gates the `iic_b facade: up` banner with no board attached, and
`check_hil_sil_parity` keeps this app in the SIL set.

On a bare bench the controller half still ACKs the real GT911, but the target
half needs an **external I2C controller** wired to the bus (e.g. a Raspberry Pi
running `i2cset`/`i2cget` at `0x42`); the stock rig provides none, so `periph=0`
there. That value is informational and never gated -- exactly the honest
degradation `i2c_peripheral_responder` and `i3c_i2c_peripheral_demo` use for the
target role.

## Build / run

```sh
make                 # cross-compile build/iic_b_facade_demo.elf
make flash           # program the EK-RA8D2 over J-Link
bash ../../../../../scripts/sim/sil_all.sh --only iic_b_facade_demo   # headless SIL gate
```

## Bench setup (full peripheral verification)

To watch the target half round-trip on real silicon, wire an external I2C
controller to the IIC_B SDA/SCL and address `0x42`:

```sh
# from a Raspberry Pi on the same bus
i2cset -y 1 0x42 0xA5    # controller write; firmware drains the byte
i2cget -y 1 0x42         # controller read; firmware echoes 0xA5 back
```
