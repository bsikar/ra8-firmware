# imu_lsm6dso_demo

Bring-up demo for the ST LSM6DSO 6-DoF IMU (MikroE 6DOF IMU 12 Click,
Digikey 1471-MIKROE-4073-ND) on the EK-RA8D2.

The app boots, brings up IIC_B channel 0 at 100 kHz on SCL1/SDA1
(P512/P511 -- the Pmod1 I2C side), binds the new `ra_lsm6dso` driver
to the I2C bus, reads the WHO_AM_I register and prints the banner
line scraped by `hil.conf`:

```
lsm6dso: who_am_i=0x6c
```

It then configures the part to +-2 g (accel), +-250 dps (gyro), 104 Hz
ODR, and loops every 250 ms reading and printing raw counts:

```
lsm6dso: ax=12 ay=-8 az=16384 gx=2 gy=-1 gz=3 temp=2531
```

`temp` is reported in centi-degrees Celsius (e.g. `2531` = 25.31 C).

## Hardware

The EK-RA8D2 has **no** native MikroBUS socket. The Click board must
be soldered onto a MikroE-to-Pmod adapter (or have its SDA/SCL/3V3/GND
pads hand-wired to the Pmod1 connector J26):

| LSM6DSO pin | EVM pin           | Notes                                     |
| ----------- | ----------------- | ----------------------------------------- |
| SDA         | Pmod1.5 / P511    | SDA1, UM Table 17 p 26.                   |
| SCL         | Pmod1.6 / P512    | SCL1, UM Table 17 p 26.                   |
| 3V3         | Pmod1.12          | EVM supplies 3.3 V on Pmod1.              |
| GND         | Pmod1.11          | EVM ground.                               |
| SA0 / SDO   | tied HIGH (3V3)   | Picks I2C address 0x6B (MikroE default).  |

Until the Click is wired up the demo will print `lsm6dso: I2C NAK`,
latch LED2, and the HIL scrape test in `hil.conf` will fail with
"timeout waiting for `lsm6dso: who_am_i=0x6c`".

## Build + flash

```sh
make imu_lsm6dso_demo
make -C examples/ek_ra8d2/hw_validated/hil/imu_lsm6dso_demo flash
```

The serial console is the J-Link OB VCOM port at 115200 8N1.
