# i3c_i2c_peripheral_demo

IIC_B peripheral-mode demo on the EK-RA8D2. The chip listens at a
fixed 7-bit I2C address on SDA/SCL and toggles LED1 on every
successful transaction. No UART output -- LED1 is the only
observable signal.

Build / flash:

```
make i3c_i2c_peripheral_demo
make -C examples/ek_ra8d2/i3c_i2c_peripheral_demo flash
```

## HIL plan

**Requires physical stim -- needs an external I2C controller on the
bus.** The chip is in I2C peripheral / target mode and waits for a
controller to initiate transactions. The HIL bench has no I2C
controller wired to the EVM's SDA/SCL pins -- the Pi has I2C
controller capability on its own header, but there is no jumper
wiring to the EK-RA8D2 today.

No UART output is emitted, so `uart_scrape` is not an option.

To make this HIL-able: wire Pi I2C (e.g. /dev/i2c-1 SDA/SCL) to the
EVM IIC_B pins, add a `hil_i2c_stim` helper that issues a write or
read transaction to the chip's peripheral address, and instrument main.c
with a `g_iic_b_xfer_count` global for `jlink_memprobe` to confirm
the peripheral state machine actually accepted the transaction.
Until that wiring exists this stays manual.

Relocated from `hw_validated/manual/` on 2026-05-19 -- author has not
yet bench-confirmed the peripheral-mode path.
