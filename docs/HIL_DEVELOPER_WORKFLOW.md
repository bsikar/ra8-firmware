# Hardware-in-the-Loop (HIL) Developer Workflow

This document is the operating manual for any contributor with an
EK-RA8D2 evaluation kit attached to their workstation. The
authoritative reference for the actual HIL CI suite (modes, gating
contract, per-app table) lives in [`HIL_SUITE.md`](HIL_SUITE.md); this
file covers the developer-side workflow that surrounds that suite.

## How HIL CI is wired

The project runs a self-hosted **Raspberry Pi 5 runner** (labels
`self-hosted, hil, ra8d2`, host alias `star@star.local`) that has
the EK-RA8D2 wired to it.

The authoritative driver is
[`scripts/hil/all.sh`](../scripts/hil/all.sh), which
[`.github/workflows/hil.yml`](../.github/workflows/hil.yml) reaches through
the `hil-all` gate (`bash scripts/ci.sh --gate hil-all`). It
auto-discovers every app under
`examples/ek_ra8d2/hw_validated/hil/` and verifies each app against
its `hil.conf` manifest. Each manifest names a `HIL_MODE`, and
`all.sh` dispatches to the matching per-mode helper alongside it in
`scripts/hil/` -- a console scrape, a wire-side peer on the Pi, or a
J-Link probe of a live counter. [`HIL_SUITE.md`](HIL_SUITE.md) is
the authority on what each mode asserts.

Flashing always goes through `scripts/hil/flash.sh`, which ships
auto-recovery for the AHB-AP-gated / TrustZone-locked / LPM-stuck
failure modes (see `scripts/hil/dlm_reset.sh` for the full DLM
recovery flow).

## Pre-push checklist (HIL-equipped contributors)

If you have an EK-RA8D2 attached locally (independent of the Pi
runner), you can pre-check your changes before pushing:

1. Build every EVM app:
   ```sh
   make build-all
   ```
2. Confirm the EK-RA8D2 is detected (see "Detecting the J-Link OB"
   below).
3. Run the HIL driver locally:
   ```sh
   bash scripts/hil/all.sh
   ```
   Subsets and per-mode runs are documented at the top of
   `scripts/hil/all.sh`.
4. Or run one app's per-mode helper from `scripts/hil/` directly --
   the same script CI runs for that app's declared `HIL_MODE`.

Contributors **without** an EK-RA8D2 may still open PRs: the host
unit-test build (`make test`) and the cross-build CI
(`firmware.yml`) are what gate them. The Pi-attached `hil.yml` is
scheduled separately from those (see its `on:` block for how it is
triggered), because a bench with one board is a serial resource and
cannot gate every push.

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
