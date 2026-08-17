# iic_b_facade_demo

Exercises IIC_B -- the RA8D2 I3C block in legacy I2C-compatibility mode, twin
of the RIIC path -- through the `ra8_io_i2c_bus` facade in BOTH roles the
peripheral supports, controller and peripheral (target), on the one channel.

As controller it runs the textbook "point at a register, then read it" pattern:
write the 2-byte pointer `0x8140`, repeated START, read 4 bytes. The target is
the on-board GoodIX GT911 touch controller at 7-bit `0x5D`, and `0x8140` is its
`PRODUCT_ID` -- the same probe `ra8_touch` performs at bring-up, so a real
device answers on a stock EK-RA8D2. As peripheral it reprograms the same
channel as an addressed target at own-address `0x42`, draining each byte a bus
controller writes (`NTST.RDBFF0`) and echoing it back on the following
controller-read (`NTST.TDBEF0`).

## The facade point

The controller half never names `ra8_i3c_*` at the call site. A board revision
that moved this bus onto a RIIC channel would change the single bind call and
nothing else; every transfer stays byte-identical. That Liskov-substitutable
"twin I2C backends" property is the reason the facade exists.

## The target half degrades honestly

The target role needs an EXTERNAL I2C controller on the bus -- a Pi running
`i2cset` / `i2cget` against `0x42` is enough. The stock bench rig provides
none, so the peripheral round-trip count stays at zero there, and it is
reported but never gated: the same honest degradation `i2c_peripheral_responder`
and `i3c_i2c_peripheral_demo` use. Under the emulator both halves do run, since
it models the GT911 on the controller bus AND plays the external controller
driving the firmware's target role.

Part of the #252 example-coverage sweep; closes #255.
