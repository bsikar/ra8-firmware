<!-- AI-OK: this is the canonical doc for the HIL suite; whole-file ownership is the point. -->
# HIL Suite

This document is the authoritative reference for the hardware-in-the-
loop (HIL) test suite that gates merges to `main`. It describes the
**contract** every app under `examples/ek_ra8d2/hw_validated/hil/` must
satisfy, the **modes** in which apps are verified, the **infrastructure
required** (Pi setup, jumpers, cables), and a **per-app table** showing
which mode each app uses + its success criterion.

The CI workflow `.github/workflows/hil.yml` runs `bash
scripts/hil/all.sh` on a self-hosted Raspberry Pi 5 runner that has
the EK-RA8D2 wired up; one app fails -> the merge fails.

## The honest-contract rule

Every app under `examples/ek_ra8d2/hw_validated/hil/` MUST prove the
feature it advertises actually works on real hardware. **"The chip
booted and the PC happens to be in MRAM"** is not enough.

Operationally that means every `hil.conf` declares a `HIL_MODE` that
runs at least one of:

  - A UART scrape that matches a banner emitted ONLY on the success
    path (and is paired with a negative regex that rejects the
    matching failure banner).
  - A J-Link memprobe of a named `volatile uint32_t` counter that is
    incremented only on a successful main-loop iteration; the gate
    asserts the counter advances by a minimum amount over a sample
    window.
  - A Pi-as-peer wire-side probe (TCP/UDP/HTTP echo / USB CDC echo)
    that exercises the chip's stack end-to-end.

The pre-commit gate `scripts/checks/check_hil_alive_policy.py` rejects
any new `hil.conf` under `hw_validated/hil/` that uses plain
`HIL_MODE=alive` -- the only exception is the fault-recovery demo
(`HIL_MODE=alive` + `HIL_FAULT_EXPECTED=1`), which has its own
positive signals (a non-zero CFSR + a "fault handled" UART banner).

If you write a new app that has no observable signal yet, either
instrument it (preferred) or place it under
`examples/ek_ra8d2/hw_pending/` until you do.

## HIL modes

| Mode             | Helper script                    | What it asserts |
|------------------|----------------------------------|-----------------|
| `uart_scrape`    | `scripts/hil/run_direct.sh`      | `HIL_EXPECT` appears on the board console (the J-Link OB VCOM, resolved by device identity via `scripts/hil/lib/tty_resolve.sh` -- never by ttyACM number, which changes on a power cycle) within `HIL_TIMEOUT_S` seconds AND `HIL_EXPECT_NEGATIVE` does NOT match in the same capture. Min `HIL_EXPECT` length is 12 chars (override per-app with `HIL_EXPECT_SHORT_OK=1` + comment) and the script rejects expects that overlap a failure banner string in the `.elf` `.rodata`. |
| `usb_cdc`        | `scripts/hil/usb_test.sh`        | The Pi enumerates the chip as a USB CDC ACM device at the given `HIL_VIDPID`, opens the CDC port, runs a correctness chunk + throughput stream, and asserts byte-exact echo + a throughput floor. PPPS re-enumerates the device mid-test. |
| `jlink_memprobe` | `scripts/hil/jlink_memprobe.sh`  | Halts the chip, reads `HIL_PROBE_SYMBOL` (resolved from the matching `.elf` via `arm-none-eabi-nm`), runs the chip for `HIL_PROBE_SECONDS`, halts again, asserts the value advanced by `>= HIL_PROBE_MIN_ADVANCE`. If `HIL_PROBE_FAILURE_SYMBOL` is set, also asserts that counter advanced by `<= HIL_PROBE_MAX_FAILURE` (default 0). |
| `hil_eth_tcp`    | `scripts/hil/eth_tcp.sh`         | The Pi opens a TCP/UDP socket to `HIL_BOARD_IP:HIL_PORT` (or `curl` for `HIL_PROTO=http`), sends a random `HIL_PAYLOAD_BYTES` payload, and asserts byte-exact echo (or HTTP 200 + the "Hello from RA8D2" marker). Uses a USB-Ethernet adapter on the Pi auto-detected via the `enxXX` / `usbX` interface naming. |
| `alive`          | `scripts/hil/check_alive.sh`     | **Reserved for the fault-recovery demo only** (`HIL_FAULT_EXPECTED=1`). Asserts: PC in MRAM/ITCM at both samples, PC not in a fault-spinner symbol (`panic_halt` / `halt_loop` / `exception_halt` / `*_Handler` / `_die`), CycleCnt advances, HFSR with DEBUGEVT masked is zero, CFSR != 0 (the fault DID fire), UART capture contains no negative banner. |

## Required Pi infrastructure

The self-hosted Pi runner (`star@star.local`) must have:

  - The EK-RA8D2 wired to the Pi via four USB cables (J7 USBHS, J11
    USBFS, J-Link OB CDC + SWD, plus the on-board Ethernet to a
    USB-Ethernet adapter on the Pi). See `docs/HIL_WIRING.md` for the
    wiring map.
  - `JLinkExe` installed and reachable (invoked with the probe serial
    from `.env` `JLINK_SN`, device `R7KA8D2KF_CPU0`).
  - The `arm-none-eabi-` toolchain on PATH (for `nm` and `addr2line`
    against the `.elf`s that ship alongside each `.hex`).
  - A VIA Labs USB hub on bus path `2-1.3` with PPPS support, so
    `uhubctl` can power-cycle individual ports.
  - A USB-Ethernet adapter that auto-IPs to `192.168.1.1/24` for the
    `hil_eth_tcp` mode (the helper script handles bring-up).
  - A Digilent Analog Discovery 2 (serial `210321A36AAE`, presenting as
    an FTDI FT232H at `0403:6014`) for signal capture: primarily the
    RA8 <-> ESP32-C6 SPI + side-band lines when the C6 harness needs
    diagnosing, and generally any bring-up question that has to be
    answered off the wire rather than from a register read. No HIL mode
    in the table above depends on it -- it is an instrument a human
    reaches for, not a gate.

    Re-provision it with the `ad2_tools` Ansible role
    (`infra/ansible/playbooks/hil-bench.yml`), which pins and installs
    the Digilent Adept runtime, installs the WaveForms SDK (`libdwf`,
    what a headless capture links against), and smoke-tests the
    instrument end to end with `scripts/hil/ad2_smoke.py` -- run that
    by hand any time to answer "can this bench capture?".

    The Adept half is fully unattended. The WaveForms deb is not: it
    has no unattended URL (every direct link is behind a click-through
    licence gate), so a human downloads it once from
    <https://digilent.com/shop/software/digilent-waveforms/download>
    and drops it in `/tmp` or `~/Downloads` -- the role adopts it into
    `/var/cache/ra8-bench/`, checks its sha256 and version, and
    installs it. When the file is absent the role fails the play with
    those instructions rather than skipping.

    WaveForms is installed by **extracting** the deb, never with apt.
    The package declares `libc6 (>= 2.41)` while the bench runs Ubuntu
    24.04 (glibc 2.39), so apt refuses it outright -- but that floor
    belongs to the Qt GUI binaries a headless bench does not install.
    `libdwf` itself tops out at `GLIBC_2.38` and runs correctly here,
    so the role installs only the library, `dwf.h`, and the device
    firmware/configuration resources under
    `/usr/share/digilent/waveforms`. Those resources are not optional:
    without them `FDwfDeviceOpen` fails with "Device not supported. No
    compatible configuration found" even though the device enumerates,
    which looks like a hardware fault and is not one.

## Isolated wireless bench LAN (ESP32-C6)

Wireless testing (the ESP32-C6 co-processor and future WiFi clients) runs on a
self-contained, air-gapped LAN with **no uplink** to the home network:

  - **FortiGate 81E-POE** (`ra8-bench-fw`) -- router / DHCP / switch on
    `10.0.40.1/24`, admin over ssh + https, console on the Pi at
    `/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_*-if00-port0` (9600 8N1).
  - **Meraki MR18** (OpenWrt) -- access point at static `10.0.40.10`, PoE-fed by
    FortiGate `port1`, publishing the 2.4 GHz bench SSID `ra8-bench` (WPA2-PSK).

All of it is codified in `infra/network/` (config artifacts, the pyserial
console driver, the OpenWrt uci script, and the wlan0 verification harness).
Every credential -- FortiGate admin, AP root, the per-SSID PSKs, and the
generated `ra8-bench` PSK -- lives in OpenBao at `secret/ra8d2/bench-network`;
nothing is committed. See `infra/network/README.md` for the topology diagram,
subnet plan, re-provision steps, and current bring-up status.

## Running a single app locally on a Mac (no Pi)

The Pi runners (`hil_run_direct.sh`, `hil_jlink_memprobe.sh`,
`hil_check_alive.sh`) target the Linux bench and SSH to
`star@star.local`. When the board is plugged straight into a developer's
Mac, `scripts/hil/run_local.sh <app>` runs one app's gate entirely on
that Mac:

```
scripts/hil/run_local.sh flash_journal
scripts/hil/run_local.sh threadx_filex_levelx_demo --uart /dev/cu.usbmodemXXXX
```

It reads the app's `hil.conf`, builds if needed, flashes via the local
`JLinkExe`, and applies the same pass/fail logic as the Pi runners for
all three offline modes (`uart_scrape`, `jlink_memprobe`, `alive`). It
reads the J-Link OB VCOM at `/dev/cu.usbmodem*` (auto-detected; override
with `--uart`) using only macOS-available tools (`stty -f`, a small
unbuffered python3 reader that sets 115200/8N1 on the live fd, since
macOS resets the line discipline on each `open()`). The wire-side Pi
peer modes (TCP/UDP/HTTP/USB-host) are NOT covered -- those still need
the Pi rig. This is for spot-checking board-only apps before promoting
them out of `hw_pending/`; CI still gates on the Pi.

## Required board switches / jumpers

The board switches/jumpers are documented in `libs/ra8_board_ek_ra8d2/inc/ra8_board_ek_ra8d2.h` ("Project SW4 layout") and `docs/reference/ek-ra8d2-v1-users-manual.pdf` Tables 3 / 18. The
project default `0xF2` (programmed into U15 PI4IOE5V6408 via
`ra8_board_io_expander_apply_project_sw4_defaults()`) maps to:

  - SW4-1 ON  + SW4-2 OFF  -> Pmod1 = UART (Wi-Fi/BLE Pmod slot)
  - SW4-3 ON               -> Octo-SPI Inactive (frees Arduino pins)
  - SW4-4 ON               -> Arduino + mikroBUS connectors Active
  - SW4-5 OFF              -> I2C on mikroBUS (SDA1/SCL1 = P511/P512)

If a HIL app needs a different layout, that goes in the app's
README.md + hil.conf comment.

The ESP32-C6 link is the notable exception, and it is mutually exclusive
with this default: it needs **SW4-1 OFF + SW4-2 OFF** (Pmod1 = SPI, not
UART) and **SW4-4 OFF**, which takes the Arduino and mikroBUS connectors
offline. Those are mechanical DIP positions -- the U15 expander cannot
override the Pmod1 SPI mux (issue #44) -- so the bank has to be flipped by
hand and flipped back. See
[`design/c6_wireless_architecture.md`](design/c6_wireless_architecture.md)
and `examples/ek_ra8d2/hw_pending/c6_spi_probe/README.md`.

## Per-app table

The table below is generated from the live `hil.conf` files and the
firmware in each app's `main.c`. Apps that are RED today (the
tightened gate correctly fails them) are flagged "RED" -- those need
follow-up firmware fixes; the test config itself is correct.

### `uart_scrape` mode

| App                          | Expect                          | Notes |
|------------------------------|---------------------------------|-------|
| `uart_hello`                 | `hello, ra8d2!`                 | basic UART up |
| `crypto_aes_demo`            | `aes: round-trip OK`            | |
| `eth_loopback`               | `etha: loopback ok`             | |
| `iwdt_demo`                  | `iwdt: poll counter`            | |
| `lpm_idle_demo`              | `lpm: wake_count=`              | |
| `watchdog_demo`              | `wdt: boot reason=`             | |
| `timer_capture_demo`         | `gpt: period=`                  | |
| `dma_memcopy_demo`           | `dma: copied 1024B match=Y`     | **RED**: DMA emits `match=N` |
| `i2c_loopback`               | `iic_b: scan 0x77 ack=1`        | **RED**: emits `scan ERROR` |
| `threadx_ipc_demo`           | `[ipc_demo] <- pong`            | **RED**: queue not passing |
| `threadx_filex_levelx_demo`  | `[fxlx] booting xSPI flash`     | **RED**: `lx_nor_flash_format failed` |
| `adc_b_demo`                 | `adc: raw=`                     | short OK |
| `agt_periodic`               | `agt: tick`                     | short OK |
| `crc_demo`                   | `crc: hw=`                      | short OK; **RED**: emits `match=N` |
| `elc_event_demo`             | `elc: en=`                      | short OK |
| `power_profiler`             | `pp: a=`                        | short OK |
| `rng_demo`                   | `trng:`                         | short OK |
| `rtc_alarm`                  | `rtc: boot`                     | short OK (SOSC may be dead on EVM) |
| `sdram_benchmark`            | `sdram: w=`                     | short OK |
| `ulpt_demo`                  | `ulpt: wake`                    | short OK |

All `uart_scrape` apps also declare a `HIL_EXPECT_NEGATIVE` regex
catching that app's failure banner + generic HAL failure strings.

### `jlink_memprobe` mode

| App                              | Match symbol                       | Mismatch symbol               | Notes |
|----------------------------------|------------------------------------|-------------------------------|-------|
| `blink`                          | `g_blink_tick`                     | --                            | LED-toggle loop @ MOCO |
| `blink_hal`                      | `g_blink_hal_tick`                 | --                            | multi-LED via HAL |
| `threadx_blink`                  | `g_threadx_blink_tick`             | --                            | scheduler liveness |
| `can_classic_loopback`           | `g_can_match`                      | `g_can_mismatch`              | **RED**: 0 matches, 5 mismatches |
| `canfd_loopback`                 | `g_canfd_match`                    | `g_canfd_mismatch`            | **RED**: same as above |
| `canfd_filter_demo`              | `g_canfd_filter_match`             | `g_canfd_filter_mismatch`     | **RED**: filter leaks |
| `threadx_canfd_demo`             | `g_threadx_canfd_match`            | --                            | stubbed CANFD (LED only) |
| `clock_check`                    | `g_clock_check_match`              | `g_clock_check_mismatch`      | full CGC tree readback |
| `cpu1_pingpong`                  | `g_cpu1_pingpong_match`            | `g_cpu1_pingpong_mismatch`    | **RED**: CPU1 not responding |
| `gpt_pwm_demo`                   | `g_gpt_pwm_match`                  | `g_gpt_pwm_mismatch`          | **RED**: GTCNT frozen |
| `flash_journal`                  | `g_fj_match`                       | `g_fj_mismatch`               | **RED**: round-trip miscompare |
| `doc_demo`                       | `g_doc_match`                      | `g_doc_mismatch`              | **RED**: HW != SW |
| `threadx_mpu_partition_demo`     | `g_threadx_mpu_partition_match`    | --                            | positive-path MPU |
| `tz_nsc_cgc_usb`                 | `g_tz_nsc_cgc_usb_match`           | `g_tz_nsc_cgc_usb_mismatch`   | NSC veneer + USBX CDC |

### `usb_cdc` mode

| App                     | VID:PID    | Port (J7/J11)         |
|-------------------------|-----------|------------------------|
| `tz_secure_only_usb_fs`    | 1209:000a | J11 (USBFS)            |
| `tz_secure_only_usb_hs` | 1209:000a | J7  (USBHS)            |

### `hil_eth_tcp` mode

| App                       | Board IP        | Proto | Port |
|---------------------------|----------------|-------|------|
| `threadx_netx_tcp_echo`   | 192.168.1.42   | tcp   | 7    |

All ethernet apps are currently **RED** -- the chip's ARP/ICMP path
does not return; same pre-existing root cause as the parked ethernet
work. The test config is correct; the firmware fix is a separate
work item.

### `alive` (fault-recovery only)

| App                       | Notes |
|---------------------------|-------|
| `mpu_partition_simple`    | Deliberate MemManage fault + recovery. Probe requires `CFSR != 0` AND PC NOT in a fault-spinner AND UART banner `mpu: fault handled, recovered`. **RED** today: SCB->SHCSR.MEMFAULTENA isn't set so the MemFault escalates to HardFault; recovery needs a SHCSR fix to actually run. |

## Apps in `hw_pending/`

These do not run in CI; they are either parked pending physical
hardware (Wi-Fi card, IMU Click, MicroSD card seated, etc.) or
pending a firmware fix that has not landed yet. Each `hil.conf` in
`hw_pending/` carries a comment explaining its parked state and what
moves it back to `hw_validated/hil/`.

## Updating this document

If you add or rename a HIL app, update both:

  - the per-app table above, and
  - the `hil.conf` for the app (`HIL_MODE` + matching settings).

The pre-commit gate enforces the `hil.conf` side. This document is
human-maintained; consider regenerating from the `hil.conf`s if it
drifts.
