# imu_lsm6dso_demo

Bring-up demo for the ST LSM6DSO 6-DoF IMU (MikroE 6DOF IMU 12 Click,
Digikey 1471-MIKROE-4073-ND) on the EK-RA8D2.

The app boots, brings up IIC_B channel 0 at 100 kHz on the MikroBUS
SDA/SCL pair (Arduino D14 = SDA1 = P511, D15 = SCL1 = P512 -- see
`k_ra_board_mikrobus_i2c_*` in the BSP), binds the new `ra_lsm6dso`
driver to the I2C bus, reads the WHO_AM_I register and prints the
banner line scraped by `hil.conf`:

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

The EK-RA8D2 has no native MikroBUS socket on the PCB. In this project's
physical bring-up:

- Pmod1 (J26) is occupied by the US159-DA16600EVZ Wi-Fi+BLE card.
- Pmod2 (J25) is occupied by the Digilent PMOD MicroSD.
- The LSM6DSO Click sits in a MikroBUS slot adapted onto the EK-RA8D2
  via a MikroE Click-Shield-style breakout on the Arduino headers,
  which routes MikroBUS SDA/SCL to the Arduino D14/D15 pads
  (= SDA1/SCL1 = P511/P512 per EK-RA8D2 v1 UM Table 20 p 28).

| LSM6DSO MikroBUS pin | Arduino-header pad | EVM net | Notes                                  |
| -------------------- | ------------------ | ------- | -------------------------------------- |
| SDA                  | D14                | P511    | SDA1, UM Table 20 p 28.                |
| SCL                  | D15                | P512    | SCL1, UM Table 20 p 28.                |
| 3V3                  | shield 3V3 rail    | -       | EVM supplies 3.3 V.                    |
| GND                  | shield GND         | -       | EVM ground.                            |
| SA0 / SDO            | tied HIGH (3V3)    | -       | Picks I2C address 0x6B (MikroE default). |

Until the Click is wired up the demo will print `lsm6dso: I2C NAK`,
latch LED2, and the HIL scrape test in `hil.conf` will fail with
"timeout waiting for `lsm6dso: who_am_i=0x6c`".

## Build + flash

```sh
make imu_lsm6dso_demo
make -C examples/ek_ra8d2/hw_pending/imu_lsm6dso_demo flash
```

The serial console is the J-Link OB VCOM port at 115200 8N1.
