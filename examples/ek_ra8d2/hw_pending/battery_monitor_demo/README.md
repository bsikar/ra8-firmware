# battery_monitor_demo

Battery **state-of-charge** monitor over a MAX17048-class I2C fuel gauge, read
through `ra8_smbus` on the IIC_B (I3C-in-I2C-mode) bus.

## What it does

The target is a **MAX17048**-style ModelGauge fuel gauge (7-bit address `0x36`)
on the Pmod1 I2C side -- a plain register-indexed I2C device, so its registers
answer the SMBus byte protocols natively (SMBus is electrically I2C). Once a
second the app reads two registers and reports the result on the SCI8 console:

| Register      | Addr | Meaning                                                |
|---------------|------|--------------------------------------------------------|
| SOC (6.5.5)   | 0x04 | State of charge; **high byte = integer percent**.      |
| CRATE         | 0x16 | Signed charge rate; sign bit of the high byte = sign.  |

A single Read-Byte-Data of `0x04` yields the battery percent directly. The CRATE
high byte's sign bit is `0` while charging (positive rate) and `1` while
discharging, so reading that one byte gives the charge direction. The banner is:

```
battery: soc=72% chg=N PASS
```

`soc` is the percent (0..100); `chg` is `Y` when charging, `N` on battery. A NAK
(no fuel gauge fitted) prints `battery: NAK (no fuel gauge)` and halts on a BKPT
before any PASS line, so the gate is exact. `g_bat_soc` / `g_bat_chg` /
`g_bat_nag` / `g_bat_heartbeat` mirror the result for headless probing.

## Low-battery nag

Each reading is folded into the **`ra8_batt`** nag policy (`libs/ra8_batt`), which
raises a one-shot warning on the descent into each band:

```
battery: NAG LOW soc=20%        # SOC fell to <=20%
battery: NAG CRITICAL soc=10%   # SOC fell to <=10%
```

The policy is **edge-triggered with hysteresis**: each band warns once, stays
quiet while the battery sits in the band, and only re-arms after SOC recovers
past the band threshold by `k_ra8_batt_rearm_margin` (3%) or while charging -- so
a steady or jittering low battery does not spam. Charging suppresses warnings.
The decision logic is pure (no MMIO), so it is host-unit-tested with full MC/DC
in `tests/test_ra8_batt.c`; this app is the on-target consumer.

In the simulator window you can drag the POWER slider down through 20% and 10%
to watch each nag fire once in the console, then back up and down to see it
re-arm.

## Build + run

```
make battery_monitor_demo
scripts/hil/run_local.sh battery_monitor_demo      # flash + scrape the banner
```

## Simulator: battery control + on-screen gauge

`tools/ra8_emulator` models the MAX17048 fuel gauge, so this app reads a real
percent over the modelled I2C bus and reaches its banner headlessly. The battery
state is driven two ways:

```
# headless: set the percent and/or charge state before the run
ra8_emulator battery_monitor_demo.elf --battery 30           # 30%, on battery
ra8_emulator battery_monitor_demo.elf --battery 90 --charge  # 90%, charging
```

In the live window (`--view`), the sidebar's **POWER** section draws a battery
slider whose fill mirrors the SOC (red <=20%, amber <=50%, green above, green
while charging) with the percent over it, plus a **CHG** toggle. Dragging the
slider sets the percent and clicking CHG flips the charge state -- both write the
same fuel-gauge model the firmware reads, so the banner tracks the slider live.

## Result (validated 2026-06-21, ra8_emulator)

```
$ ra8_emulator battery_monitor_demo.elf
[uart] SCI8: battery-monitor: boot
[uart] SCI8: battery: soc=72% chg=N PASS

$ ra8_emulator battery_monitor_demo.elf --battery 55 --charge
[uart] SCI8: battery: soc=55% chg=Y PASS
```

`scripts/emu/smoke.sh battery_monitor_demo` PASS -- ra8_emulator models the
fuel gauge on the modelled IIC_B bus, so the SOC + CRATE reads return through the
genuine `ra8_smbus -> ra8_i3c` I2C path (no stub) and the banner is deterministic.

## On real silicon (hw_pending)

This app **requires a MAX17048-class fuel gauge** fitted to the Pmod1 I2C side
(`P511/SDA1` + `P512/SCL1`). It is in `hw_pending` until validated on the bench
with the part fitted; without it the bring-up NAKs. (The EK-RA8D2 has no native
fuel gauge -- a stock board prints the NAK banner.)
