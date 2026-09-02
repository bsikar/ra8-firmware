# smbus_demo

Standalone demo and gate for the SMBus 3.2 protocol layer (`ra8_smbus`, #128),
which frames Send Byte / Receive Byte / Read Byte Data / Block transactions on
top of the IIC_B (I3C-in-I2C-mode) controller and delegates raw byte movement to
`ra8_i3c`. It previously had no example and no gate.

The target is the ST LSM6DSO IMU at 7-bit address `0x6B` on the Pmod1 I2C side --
a plain register-indexed I2C device, so it answers the SMBus byte protocols
natively, SMBus being electrically I2C. The app reads the fixed `WHO_AM_I`
register two ways, exercising three protocol shapes:

| SMBus protocol (3.2 spec) | Wire frame (PEC off)                    |
|---------------------------|-----------------------------------------|
| Read Byte Data (6.5.5)    | `S addr_w [0x0F] Sr addr_r [0x6C] P`    |
| Send Byte (6.5.2)         | `S addr_w [0x0F] P`  (sets reg pointer) |
| Receive Byte (6.5.3)      | `S addr_r [0x6C] P`                     |

Both reads must return `0x6C`. PEC is disabled because the IMU is not an SMBus
PEC device; a production SMBus target such as a smart battery or power IC would
use it, which `cfg.pec_enabled` turns on. A NAK or a wrong value halts on a BKPT
**before** the PASS line, so the gate cannot pass by accident. The PEC CRC-8
helper is covered on the host by `tests/misc/src/test_ra8_smbus.c`.

Using the IMU as the target is a convenience: any register-file I2C device
demonstrates the protocol-layer framing end to end.

## Blocked on

The IMU 12 Click has to be fitted to the Pmod1 I2C side (`P511/SDA1` +
`P512/SCL1`) -- the same prerequisite as `imu_lsm6dso_demo`, and the EK-RA8D2 has
no native MikroBUS socket. Without the part the bring-up NAKs. The device *is*
modelled on the IIC_B bus off-target, so the transactions return through the
genuine `ra8_smbus` -> `ra8_i3c` path with no stub.
