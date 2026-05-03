# i2c_loopback

IIC_B (I3C-in-I2C-mode) self-test smoke app for the EK-RA8D2.
Brings up `ra_iic_b` on channel 0 (SCL1 = P512, SDA1 = P511 -- the
on-board I2C bus shared with Pmod1, the Arduino headers, and the
DSI / camera buses) at 100 kHz Sm and probes a vacant 7-bit address
(0x77, BME280 default) once a second. The bus is unpopulated on a
bare EVM so the peripheral NACKs -- we treat that as "the controller is
alive and clocking SCL" and print

```
iic_b: scan 0x77 ack=0
iic_b: scan 0x77 ack=0
...
```

over the SCI8 console (115200 8N1 on the J-Link OB CDC port). LED1
toggles each cycle; LED2 latches ON if the driver itself returns a
hard error.

## Build + flash

```sh
make i2c_loopback
make -C examples/ek_ra8d2/i2c_loopback flash
```

The dedicated I2C pull-ups on P511 / P512 are populated on the EVM
(R5 / R6) so no external wiring is required.
