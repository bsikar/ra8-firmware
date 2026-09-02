# HIL Suite

This document is the authoritative reference for the hardware-in-the-
loop (HIL) test suite for the bench EK-RA8D2. It describes the
**contract** every app under `examples/ek_ra8d2/hw_validated/hil/` must
satisfy, the **modes** in which apps are verified, and the
**infrastructure required** (Pi setup, jumpers, cables). It deliberately
does not list the apps -- see the last section.

The CI workflow `.github/workflows/hil.yml` is a thin driver for the
`hil-all` gate (`just quality::local::gate hil-all`). A dedicated native
listener builds on the dev box, then `scripts/hil/all.sh` operates the
Raspberry Pi 5 bench over SSH; one app fails -> the run fails. The workflow
installs no compiler or Just binary: the `dev_box` Ansible role owns and
asserts that pinned toolchain. A bench with one board is a serial resource, so
how that workflow is triggered is a scheduling decision recorded in its own
`on:` block rather than here.

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
| `hil_eth_tcp`    | `scripts/hil/eth_tcp.sh`         | The Pi opens a TCP/UDP socket to `HIL_BOARD_IP:HIL_PORT` (or `curl` for `HIL_PROTO=http`), sends a random `HIL_PAYLOAD_BYTES` payload, and asserts byte-exact echo (or HTTP 200 + the "Hello from RA8D2" marker). Uses the fleet-declared built-in board-facing interface after the installed policy verifies its MAC, sysfs device, PHC, and non-uplink state. |
| `c6_camera_livestream` | `scripts/hil/camera_livestream.sh` | On the C6 lane, cold-starts the co-processor, proves its SPI link, joins Wi-Fi, checks the camera server health endpoint, decodes two 320x240 JPEG frames and requires their bytes to differ. The verifier builds in a temporary credential-free tree, waits for the firmware's `READY v1` prompt, then provisions Wi-Fi at runtime over UART; credentials never enter compiler arguments, generated sources, build metadata, or logs. |
| `rtt_scrape`     | `scripts/hil/rtt_scrape.sh`      | Same contract as `uart_scrape`, but the capture is read out of the firmware's SEGGER RTT up-buffer (`HIL_RTT_BUF_SYMBOL`, default `s_rtt_up_buf`, `HIL_RTT_BUF_BYTES` wide) via J-Link `mem` reads rather than off the VCOM -- J-Link's own RTT logger resets the target on connect. |
| `alive`          | `scripts/hil/check_alive.sh`     | **Reserved for the fault-recovery demo only** (`HIL_FAULT_EXPECTED=1`). Asserts: PC in MRAM/ITCM at both samples, PC not in a fault-spinner symbol (`panic_halt` / `halt_loop` / `exception_halt` / `*_Handler` / `_die`), CycleCnt advances, HFSR with DEBUGEVT masked is zero, CFSR != 0 (the fault DID fire), UART capture contains no negative banner. |

## Required remote Pi infrastructure

The bench host selected by `.env` `PI_HOST` must have:

  - The EK-RA8D2 wired to the Pi via three USB cables (J7 USBHS, J11
    USBFS, and J-Link OB CDC + SWD), plus its on-board Ethernet wired to
    the fleet-declared built-in board-facing interface. The port map is in the header
    comment of `.github/workflows/hil.yml`; what else hangs off the Pi
    is in [`INFRASTRUCTURE.md`](INFRASTRUCTURE.md).
  - `JLinkExe` installed and reachable (invoked with validated `JLINK_SN` and
    `JLINK_DEVICE`; the default device comes only from `rig_contract.sh`).
  - The `arm-none-eabi-` toolchain on PATH (for `nm` and `addr2line`
    against the `.elf`s that ship alongside each `.hex`).
  - A VIA Labs USB hub on bus path `2-1.3` with PPPS support, so
    `uhubctl` can power-cycle individual ports.
  - The Ansible-authenticated built-in Ethernet interface for `hil_eth_tcp`.
    The root-owned helper verifies its permanent identity and PHC before it
    temporarily assigns `192.168.1.1/24`; USB adapters are rejected.
  - A Digilent Analog Discovery 2 (serial `210321A36AAE`, presenting as
    an FTDI FT232H at `0403:6014`) for signal capture: primarily the
    RA8 <-> ESP32-C6 SPI + side-band lines when the C6 harness needs
    diagnosing, and generally any bring-up question that has to be
    answered off the wire rather than from a register read. No HIL mode
    in the table above depends on it -- it is an instrument a human
    reaches for, not a gate.

    Re-provision the declared bench with `just infra::apply star`. The fleet
    dispatcher invokes the `ad2_tools` role through
    `infra/ansible/playbooks/hil-bench.yml`; it pins and installs the Digilent
    Adept runtime, installs the WaveForms SDK (`libdwf`,
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
    The package declares a newer glibc floor than the bench provides, so
    apt refuses it outright -- but that floor belongs to the Qt GUI
    binaries a headless bench does not install. `libdwf` itself needs
    less and runs correctly here, so the role installs only the
    library, `dwf.h`, and the device
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

The bench-side helpers (`scripts/hil/run_direct.sh`,
`scripts/hil/jlink_memprobe.sh`, `scripts/hil/check_alive.sh`) target
the Linux bench selected by `PI_HOST`. When the board is plugged straight into
a developer's Mac, `just hil::run_local <app>` runs one app's gate entirely on
that Mac.

It reads the app's `hil.conf`, builds if needed, flashes via the local
`JLinkExe`, and applies the same pass/fail logic as the bench-side helpers for
all three offline modes (`uart_scrape`, `jlink_memprobe`, `alive`). It
reads the J-Link OB VCOM at `/dev/cu.usbmodem*` (auto-detected; override
with `--uart`) using only macOS-available tools (`stty -f`, a small
unbuffered python3 reader that sets 115200/8N1 on the live fd, since
macOS resets the line discipline on each `open()`). The wire-side Pi
peer modes (TCP/UDP/HTTP/USB-host) are NOT covered -- those still need
the Pi instrument host. This is for spot-checking board-only apps before
promoting them out of `hw_pending/`; the dev-box CI listener still gates
through that instrument host.

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
and `examples/ek_ra8d2/hw_validated/c6/README.md`.

Because that setting cannot coexist with this suite's, the C6 apps are a
SEPARATE LANE rather than a separate runner:

```sh
just hil::c6                    # every app under hw_validated/c6/
just hil::c6 c6_spi_probe       # just one
```

which is `scripts/hil/all.sh --dir examples/ek_ra8d2/hw_validated/c6` -- the
same discovery, the same `hil.conf` manifests, the same bench hold and the
same verifiers as `just hil::suite`. A second copy of the runner would be a
second place for all of that to drift.

They sit outside `hw_validated/hil/` for a second, independent reason:
`ra8_emulator` models no ESP32-C6 (#494), and `check_hil_eil_parity.py`
requires every app in that directory to be EIL-exercised with no skips. That
gate is right; the C6 apps simply cannot satisfy it yet, and punching a hole in
it to house them would cost more than the separate lane does.

## Which apps run, and how each is asserted

Each app's root-level `hil.conf` declares the mode and assertion for the
firmware entry under
`examples/ek_ra8d2/hw_validated/hil/<app>/src/main.c`.
`scripts/hil/all.sh` reads manifests directly,
so there is no second roster here to fall out of step with the tree --
`grep -rl HIL_MODE examples/ek_ra8d2/hw_validated/hil` is the current one.

Two modes carry nearly all of it: `uart_scrape` for anything that can print a
verdict, and `jlink_memprobe` for anything that cannot, where the probe instead
watches a counter in SRAM advance. `alive` is reserved for the fault-recovery
demo; the remaining modes each serve one narrow lane.

An app that fails a bench run is moved to
`examples/ek_ra8d2/hil_needs_revalidation/` rather than being annotated as
failing here, so the directory listing and the last suite result agree by
construction. Apps under `examples/ek_ra8d2/hw_pending/` do not run in CI at
all; each carries its own README saying what would move it into the suite.

## Updating this document

Adding or renaming a HIL app needs only its `hil.conf` (`HIL_MODE` plus the
matching settings), which the pre-commit gate enforces. Nothing in this file
enumerates apps, so nothing here goes stale when the roster changes -- keep it
that way.
