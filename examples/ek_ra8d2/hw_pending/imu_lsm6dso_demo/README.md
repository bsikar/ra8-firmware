# imu_lsm6dso_demo

Bring-up demo for the ST LSM6DSO 6-DoF IMU (MikroE 6DOF IMU 12 Click, Digikey
1471-MIKROE-4073-ND). The app brings up IIC_B channel 0 at 100 kHz on the
MikroBUS SDA/SCL pair, binds the `ra8_lsm6dso` driver, reads `WHO_AM_I`
(expecting `0x6C`), then configures the part to +-2 g, +-250 dps and 104 Hz ODR
and loops printing raw accel, gyro and temperature counts. Temperature is
reported in centi-degrees Celsius.

## Hardware

The EK-RA8D2 has **no native MikroBUS socket**. In this project's physical
bring-up, Pmod1 (J26) is free and Pmod2 (J25) is occupied by the Digilent PMOD
MicroSD, so the Click sits in a MikroBUS slot on a Click-Shield-style breakout
over the Arduino headers, which routes MikroBUS SDA/SCL to the Arduino D14/D15
pads.

| LSM6DSO MikroBUS pin | J22/Arduino pad | EVM net | Notes                                    |
| -------------------- | --------------- | ------- | ---------------------------------------- |
| SDA                  | J22.6 / D14     | P511    | SDA1, UM Table 21 p 29 (SW4-5 = OFF).    |
| SCL                  | J22.5 / D15     | P512    | SCL1, UM Table 21 p 29 (SW4-5 = OFF).    |
| 3V3                  | shield 3V3 rail | --      | EVM supplies 3.3 V.                      |
| GND                  | shield GND      | --      | EVM ground.                              |
| SA0 / SDO            | tied HIGH (3V3) | --      | Picks I2C address 0x6B (MikroE default). |

Pin assignments follow EK-RA8D2 v1 UM Table 20 p 28 and Table 21 p 29.

### Required SW4 DIP positions

The on-board SW4 8-position DIP gates which connectors the chip's muxed
peripherals route to (EK-RA8D2 v1 UM Rev 1.01 Table 3 p 16 and Section 5.3.5
p 29). The I2C-side MikroBUS path requires:

| SW4 | Position | Reason                                               |
| --- | -------- | ---------------------------------------------------- |
| 3   | ON       | Octo-SPI Inactive (frees the Arduino/mikroBUS pins). |
| 4   | ON       | Arduino + mikroBUS connectors Active.                |
| 5   | OFF      | I2C Active on mikroBUS (P511/P512 SDA1/SCL1).        |

The remaining channels are independent and can stay at the project defaults
documented in the board header. Either flip the DIPs by hand or call
`ra8_board_io_expander_apply_project_sw4_defaults()` early in `main()` to have
the U15 expander drive the whole project layout in firmware -- **that overrides
whatever the physical switches read.**

## Blocked on

The Click board is not on the bench. Without it the I2C bus is unpopulated,
`WHO_AM_I` reads `0xFF` with no ack, and the demo prints a NAK and latches LED2.
The same is true if the SW4 layout above is wrong.
