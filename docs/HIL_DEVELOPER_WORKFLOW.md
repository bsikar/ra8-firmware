# Hardware-in-the-Loop (HIL) Developer Workflow

This document is the operating manual for any contributor with an
EK-RA8D2 evaluation kit attached to their workstation. The
authoritative reference for the actual HIL CI suite (modes, gating
contract, per-app table) lives in [`HIL_SUITE.md`](HIL_SUITE.md); this
file covers the developer-side workflow that surrounds that suite.

## How HIL CI is wired

The project runs a self-hosted **Raspberry Pi 5 runner** (labels
`self-hosted, hil, pi5, ra8d2`, host alias `star@star.local`) that has
the EK-RA8D2 wired to it. The runner exists -- this is not a
deferred / planned workflow.

The authoritative driver is
[`scripts/hil/all.sh`](../scripts/hil/all.sh) (invoked from
[`.github/workflows/hil.yml`](../.github/workflows/hil.yml)), which
auto-discovers every app under
`examples/ek_ra8d2/hw_validated/hil/` and verifies each app against
its `hil.conf` manifest. The per-mode helper scripts are:

- `scripts/hil/run_direct.sh` -- UART scrape (`HIL_MODE=uart_scrape`).
- `scripts/hil/usb_test.sh` -- USB CDC echo (`HIL_MODE=usb_cdc`).
- `scripts/hil/jlink_memprobe.sh` -- live counter probe
  (`HIL_MODE=jlink_memprobe`).
- `scripts/hil/eth_tcp.sh` -- ethernet socket echo
  (`HIL_MODE=hil_eth_tcp`).
- `scripts/hil/check_alive.sh` -- fault-recovery demo
  (`HIL_MODE=alive`).

Flashing always goes through `scripts/hil/flash.sh`, which ships
auto-recovery for the AHB-AP-gated / TrustZone-locked / LPM-stuck
failure modes (see `scripts/hil/dlm_reset.sh` for the full DLM
recovery flow).

## Pre-push checklist (HIL-equipped contributors)

If you have an EK-RA8D2 attached locally (independent of the Pi
runner), you can pre-check your changes before pushing:

1. Build every EVM app:
   ```sh
   make apps
   ```
2. Confirm the EK-RA8D2 is detected (see "Detecting the J-Link OB"
   below).
3. Run the HIL driver locally:
   ```sh
   bash scripts/hil/all.sh
   ```
   Subsets and per-mode runs are documented at the top of
   `scripts/hil/all.sh`.
4. For HIL-suite-managed apps under
   `examples/ek_ra8d2/hw_validated/hil/`, run the same per-app
   helper the CI runs (`scripts/hil/run_direct.sh`,
   `scripts/hil/usb_test.sh`, `scripts/hil/jlink_memprobe.sh`,
   `scripts/hil/eth_tcp.sh`, `scripts/hil/check_alive.sh`) directly.

Contributors **without** an EK-RA8D2 may still open PRs; the host
unit-test build (`make test`) and the cross-build CI
(`firmware.yml`) gate those PRs locally, and the Pi-attached
`hil.yml` runs on every push to `main` and on every PR that
touches HIL-relevant paths.

## Detecting the J-Link OB

The EK-RA8D2's on-board J-Link OB enumerates as a SEGGER USB device
once **J10** is plugged in. The detection one-liners differ by host:

### macOS

```sh
system_profiler SPUSBDataType 2>/dev/null \
  | grep -E "SEGGER|J-Link" -A 4 \
  | grep -E "Serial Number" \
  | head -1
```

If the line prints, the board is attached. The serial number is the
J-Link OB SN; record it in any bug report so the on-board firmware
revision can be cross-checked at <https://www.segger.com>.

### Linux

```sh
lsusb -d 1366: -v 2>/dev/null | grep iSerial | head -1
```

Vendor ID `1366` = SEGGER Microcontroller GmbH.

### Cross-platform (via JLinkExe)

```sh
JLinkExe -nogui 1 -CommandFile <(echo -e "ShowEmuList\nexit") \
  | grep -E "J-Link OB" | head -1
```

If `ShowEmuList` returns nothing, the board is not attached or the
J-Link USB driver is not installed.

## Cross-references

- [`HIL_SUITE.md`](HIL_SUITE.md) -- the authoritative HIL contract,
  per-app table, modes, and Pi-runner infrastructure.
- [`scripts/hil/all.sh`](../scripts/hil/all.sh) -- the
  HIL-suite driver invoked from CI.
- [`scripts/hil/flash.sh`](../scripts/hil/flash.sh) -- the
  authoritative flash path with auto-recovery.
- [`scripts/hil/dlm_reset.sh`](../scripts/hil/dlm_reset.sh) -- DLM
  recovery for the AHB-AP-gated / TrustZone-locked failure modes.
- [`.github/workflows/hil.yml`](../.github/workflows/hil.yml) -- the
  Pi-attached HIL gate.
- [`CERTIFICATION_SCOPE.md`](CERTIFICATION_SCOPE.md) -- the
  "no third-party certification" decision.
