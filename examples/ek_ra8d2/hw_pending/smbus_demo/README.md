# smbus_demo

Standalone **SMBus 3.2 protocol-layer** demo + HIL gate (`ra8_smbus`, #128).
`ra8_smbus` frames Send Byte / Receive Byte / Read Byte Data / Block transactions
on top of the IIC_B (I3C-in-I2C-mode) controller and delegates raw byte movement
to `ra8_i3c`. It had no example and no CI gate.

## What it does

The target is the ST **LSM6DSO** 6-DoF IMU (IMU 12 Click, 7-bit address `0x6B`)
on the Pmod1 I2C side -- a plain register-indexed I2C device, so it answers the
SMBus byte protocols natively (SMBus is electrically I2C). The app reads the
fixed `WHO_AM_I` register (`0x0F` -> `0x6C`) **two ways**, exercising three
protocol shapes:

| SMBus protocol (3.2 spec)      | Wire frame (PEC off)                   |
|--------------------------------|----------------------------------------|
| Read Byte Data (6.5.5)         | `S addr_w [0x0F] Sr addr_r [0x6C] P`   |
| Send Byte (6.5.2)              | `S addr_w [0x0F] P`  (sets reg pointer)|
| Receive Byte (6.5.3)           | `S addr_r [0x6C] P`                    |

Both reads must return `0x6C`; the banner is:

```
smbus: whoami=6C sendrecv=6C PASS
```

PEC is disabled (the IMU is not an SMBus PEC device). A NAK (no IMU fitted)
prints `smbus: NAK (no LSM6DSO)`; a wrong value prints `smbus: mismatch`; either
halts on a BKPT before the PASS line, so the gate is exact.

## Build + run

```
make smbus_demo
scripts/hil/run_local.sh smbus_demo      # flash + scrape the banner
```

## Result (validated 2026-06-19, ra8_emulator)

```
$ ra8_emulator smbus_demo.elf
[uart] SCI8: smbus-demo: boot
[uart] SCI8: smbus: whoami=6C sendrecv=6C PASS
  I3C/I2C LSM6DSO: 2 register read(s) answered (WHO_AM_I + samples)
```

`scripts/emu/smoke.sh smbus_demo` PASS -- ra8_emulator models the LSM6DSO on
the modelled IIC_B bus, so the SMBus transactions return through the genuine
`ra8_smbus -> ra8_i3c -> GT911/IMU` I2C path (no stub) and the banner is
deterministic. The SMBus PEC CRC-8 helper is covered on the host by
`tests/test_ra8_smbus.c`.

## On real silicon (hw_pending)

This app **requires the IMU 12 Click (ST LSM6DSO)** fitted to the Pmod1 I2C side
(`P511/SDA1` + `P512/SCL1`) -- the same prerequisite as `imu_lsm6dso_demo`. It is
in `hw_pending` until validated on the bench with the part fitted; without it the
bring-up NAKs. (The EK-RA8D2 has no native MikroBUS socket.)

Using the IMU as the SMBus target is a convenience: SMBus byte protocols are
I2C-compatible, so any register-file I2C device demonstrates the protocol-layer
framing end to end. A production SMBus target (smart battery, power IC) would
additionally use PEC; flip `cfg.pec_enabled = true` to append + verify it.
