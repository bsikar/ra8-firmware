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

| LSM6DSO MikroBUS pin | J22/Arduino pad | EVM net | Notes                                  |
| -------------------- | --------------- | ------- | -------------------------------------- |
| SDA                  | J22.6 / D14     | P511    | SDA1, UM Table 21 p 29 (SW4-5 = OFF).  |
| SCL                  | J22.5 / D15     | P512    | SCL1, UM Table 21 p 29 (SW4-5 = OFF).  |
| 3V3                  | shield 3V3 rail | -       | EVM supplies 3.3 V.                    |
| GND                  | shield GND      | -       | EVM ground.                            |
| SA0 / SDO            | tied HIGH (3V3) | -       | Picks I2C address 0x6B (MikroE default). |

### Required SW4 DIP positions

Per EK-RA8D2 v1 UM Rev 1.01 Table 3 p 16 + Section 5.3.5 p 29 the
on-board SW4 8-position DIP gates which connectors the chip's muxed
peripherals route to. For this project the I2C-side MikroBUS path
requires:

| SW4 | Position | Reason                                                |
| --- | -------- | ----------------------------------------------------- |
| 3   | ON       | Octo-SPI Inactive (frees the Arduino/mikroBUS pins).  |
| 4   | ON       | Arduino + mikroBUS connectors Active.                 |
| 5   | OFF      | I2C Active on mikroBUS (P511/P512 SDA1/SCL1).         |

The other channels (SW4-1/2 for Pmod1 mode, SW4-6/7/8) are independent
and can stay at the project's other defaults documented in
`libs/ra_board_ek_ra8d2/inc/ra_board_ek_ra8d2.h` ("Project SW4 layout").

Either flip the DIPs manually or call
`ra_board_io_expander_apply_project_sw4_defaults()` early in `main()`
to have the U15 PI4IOE5V6408 expander drive the whole project layout
in firmware -- this overrides whatever the physical switches read.

Until the Click is wired up and the SW4 layout is correct, the demo
will print `lsm6dso: I2C NAK`, latch LED2, and the HIL scrape test in
`hil.conf` will fail with "timeout waiting for `lsm6dso: who_am_i=0x6c`".

## Build + flash

```sh
make imu_lsm6dso_demo
make -C examples/ek_ra8d2/hw_pending/imu_lsm6dso_demo flash
```

The serial console is the J-Link OB VCOM port at 115200 8N1.
