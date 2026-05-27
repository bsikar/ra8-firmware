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
[`scripts/hil_all.sh`](../scripts/hil_all.sh) (invoked from
[`.github/workflows/hil.yml`](../.github/workflows/hil.yml)), which
auto-discovers every app under
`examples/ek_ra8d2/hw_validated/hil/` and verifies each app against
its `hil.conf` manifest. The per-mode helper scripts are:

- `scripts/hil_run_direct.sh` -- UART scrape (`HIL_MODE=uart_scrape`).
- `scripts/hil_usb_test.sh` -- USB CDC echo (`HIL_MODE=usb_cdc`).
- `scripts/hil_jlink_memprobe.sh` -- live counter probe
  (`HIL_MODE=jlink_memprobe`).
- `scripts/hil_eth_tcp.sh` -- ethernet socket echo
  (`HIL_MODE=hil_eth_tcp`).
- `scripts/hil_check_alive.sh` -- fault-recovery demo
  (`HIL_MODE=alive`).

Flashing always goes through `scripts/hil_flash.sh`, which ships
auto-recovery for the AHB-AP-gated / TrustZone-locked / LPM-stuck
failure modes (see `scripts/hil_dlm_reset.sh` for the full DLM
recovery flow). For the legacy quick-smoke harness (PC-resolution
classification only, no hil.conf), `scripts/hw_smoke_test.sh`
remains in tree.

## Pre-push checklist (HIL-equipped contributors)

If you have an EK-RA8D2 attached locally (independent of the Pi
runner), you can pre-check your changes before pushing:

1. Build every EVM app:
   ```sh
   make apps
   ```
2. Confirm the EK-RA8D2 is detected (see "Detecting the J-Link OB"
   below).
3. Run the legacy smoke harness for a fast PC-resolution sweep:
   ```sh
   bash scripts/hw_smoke_test.sh
   ```
4. Verify the exit code (`echo $?`):
   - `0` -- every app PASS / WIP / UNKNOWN. Push allowed.
   - `1` -- at least one app FAILED. **Do not push.** Investigate
     the failing app's `build/smoke/<app>.log`, fix the root cause,
     re-run, then push.
   - `2` -- harness misconfiguration (toolchain or board missing).
     Fix the local environment and re-run.
5. For HIL-suite-managed apps under
   `examples/ek_ra8d2/hw_validated/hil/`, run the same per-app
   helper the CI runs (`scripts/hil_run_direct.sh`,
   `scripts/hil_usb_test.sh`, `scripts/hil_jlink_memprobe.sh`,
   `scripts/hil_eth_tcp.sh`, `scripts/hil_check_alive.sh`) directly.

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

## PASS criteria for the legacy `hw_smoke_test.sh`

The smoke harness classifies via halt-PC pattern matching. The
modern HIL-suite classification (per-app `hil.conf` contract) is
documented in [`HIL_SUITE.md`](HIL_SUITE.md). The legacy harness
rubric is:

- **PASS** -- the firmware reached its main loop or a known
  scheduler entry point. Counts as green.
- **WIP** -- the firmware reached a caught-error sink
  (`panic_halt`, `internal_ra_fatal_error`, etc.). Counts as
  green-with-warning -- the init failed *cleanly*; usually means
  the app needs a vendor blob (see `docs/VENDOR_BLOBS.md`) or
  external hardware that the developer does not have wired up.
- **UNKNOWN** -- the chip is alive but the program counter does not
  match any known PASS / WIP pattern. Counts as green-with-warning;
  add a comment to the PR explaining why if you choose to push.
- **FAIL** -- HardFault, lockup, or fall-through to
  `Default_Handler`. **Blocks the push.** This is a real bug.
- **NOBUILD** -- the `.elf` / `.hex` was never built. Re-run
  `make apps`.

## Cross-references

- [`HIL_SUITE.md`](HIL_SUITE.md) -- the authoritative HIL contract,
  per-app table, modes, and Pi-runner infrastructure.
- [`scripts/hil_all.sh`](../scripts/hil_all.sh) -- the
  HIL-suite driver invoked from CI.
- [`scripts/hw_smoke_test.sh`](../scripts/hw_smoke_test.sh) -- the
  legacy halt-PC classification harness.
- [`scripts/hil_flash.sh`](../scripts/hil_flash.sh) -- the
  authoritative flash path with auto-recovery.
- [`scripts/hil_dlm_reset.sh`](../scripts/hil_dlm_reset.sh) -- DLM
  recovery for the AHB-AP-gated / TrustZone-locked failure modes.
- [`.github/workflows/hil.yml`](../.github/workflows/hil.yml) -- the
  Pi-attached HIL gate.
- [`CERTIFICATION_SCOPE.md`](CERTIFICATION_SCOPE.md) -- the
  "no third-party certification" decision.
