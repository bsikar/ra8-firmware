# Hardware-in-the-Loop (HIL) Developer Workflow

This document is the operating manual for any contributor with an
EK-RA8D2 evaluation kit attached to their workstation. The
authoritative reference for the actual HIL CI suite (modes, gating
contract, per-app table) lives in [`HIL_SUITE.md`](HIL_SUITE.md); this
file covers the developer-side workflow that surrounds that suite.

## How HIL CI is wired

The project runs a dedicated native Actions listener on the **dev box** (labels
`self-hosted, hil, ra8d2`). It builds there, while the existing HIL scripts use
an isolated SSH identity to operate the Raspberry Pi 5 bench wired to the
EK-RA8D2. The `dev_box` Ansible role owns the pinned host tools; the workflow
does not install dependencies. Its bench target comes from the protected
controller `.env` as `PI_HOST`. Ansible installs `PI_HOST`, `JLINK_SN`, and
`JLINK_DEVICE` into the runner's root-only service environment. The interactive
`~/.config/ra8/hil.env` also receives `PI_REPO`, so `just hil::*` works from
every linked worktree without copying configuration into each checkout. The
interactive file preserves that account's user-qualified `PI_HOST`; the
isolated service strips the username and uses only its role-owned SSH identity.

[`rig_contract.sh`](../scripts/hil/lib/rig_contract.sh) is the single typed
authority for all four values. It accepts user-qualified DNS/IPv4 SSH targets,
identifier-shaped probe/device names, and traversal-free absolute or relative
repository paths. Leading-option hosts, control characters, whitespace, shell
metacharacters, malformed addresses, and unsafe path segments fail before any
value reaches ssh, scp, a heredoc, or a remote command. Ansible parses only the
allowlisted assignments and delegates their values to that same authority.
IPv6 targets, including bracketed literals, are intentionally rejected: the
current Ansible, systemd-environment, and SSH configuration path is specified
and tested only for DNS names and IPv4. Give an IPv6-only bench a DNS name
rather than adding a second grammar in one consumer.

A checkout-local `.env` remains the workstation override, and `RA8_RIG_ENV`
selects an explicit protected file when needed. The interactive loader never
executes that file. The isolated declarative parser requires a current-user-
owned, non-symlink, mode-0600 regular file, accepts literal assignments for the
four declared fields and the documented non-secret wait/actor/TTY controls, and
rejects command syntax and expansion. TAPO secrets and unknown keys never enter
the shell representation. It reads through the authenticated descriptor, so
replacing the pathname cannot change the bytes being parsed. Use `chmod 0600
.env` after copying the example.

The authoritative driver is
[`scripts/hil/all.sh`](../scripts/hil/all.sh), which
[`.github/workflows/hil.yml`](../.github/workflows/hil.yml) reaches through
the `hil-all` gate (`just quality::local::gate hil-all`). It
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

## Converging the dev-box HIL listener

The registered native listener can be checked through its focused tag. A real
converge enters the complete authenticated dev-box transaction:

```sh
just infra::check dev dev-box hil-runner
just --yes infra::apply dev dev-box
```

Both are canonical fleet entry points. The dry run may select the
`hil-runner` task slice because it changes nothing. The real apply deliberately
accepts no public tag selector: the fleet dispatcher previews the complete
play, authenticates the whole-bench hold, and stops only an idle listener
before any dev-box mutation. First registration uses the separately typed
`infra::register_hil` path.

## Board-facing Ethernet topology

Ethernet HIL does not auto-detect an `enx*`/`usb*` adapter. The EK-RA8D2 is
wired to the bench host's fleet-declared built-in Ethernet port. The
`hil_bench` role owns its interface name, permanent MAC, canonical sysfs device,
and PHC index through `hil_bench_eth_iface`, `hil_bench_eth_mac`,
`hil_bench_eth_sysfs_device`, and `hil_bench_eth_phc_index`. Convergence fails
if that physical identity drifts, if it is an uplink, or if hardware transmit
and receive timestamping/its PTP hardware clock are absent. A USB-Ethernet
adapter is therefore intentionally rejected rather than selected as a
fallback. At runtime `scripts/hil/eth_tcp.sh` asks the installed root-owned
policy helper for the already-authenticated interface; it does not rediscover
one by name or carrier state.

## Pre-push checklist (HIL-equipped contributors)

If you have an EK-RA8D2 attached locally (independent of the Pi
runner), you can pre-check your changes before pushing:

1. Build the repository:
   ```sh
   just build_all
   ```
2. Confirm the EK-RA8D2 is detected (see "Detecting the J-Link OB"
   below).
3. Run one offline-capable app against the board attached to this Mac:
   ```sh
   just hil::run_local <app>
   ```
   This covers `uart_scrape`, `jlink_memprobe`, and the fault-recovery `alive`
   mode. Wire-side peer modes still require the Pi.
4. Run the full suite on the guarded remote rig when the change affects a
   wire-side mode or shared HIL infrastructure:
   ```sh
   just hil::run
   ```
   Pass a comma-separated app list as the first argument to select a subset.

Contributors **without** an EK-RA8D2 may still open PRs: the host
unit-test build (`just quality::local::test`) and the cross-build CI
(`firmware.yml`) are what gate them. Fork PRs are never admitted to the
persistent HIL runner; same-repository PRs and pushes matching `hil.yml`'s path
filter are scheduled separately because a bench with one board is a serial
resource.

## Detecting the J-Link OB

The EK-RA8D2's on-board J-Link OB enumerates as a SEGGER USB device
once **J10** is plugged in. Use the repository entry point first:

```sh
just hil::find_jlink
```

When `PI_HOST` is configured, the command queries that bench host over
SSH without connecting to the target. On the Pi or a directly attached
rig it scans locally. In both cases it prefers `JLinkExe ShowEmuList`
and falls back to native USB enumeration. The direct host-specific
checks below remain useful when bootstrapping without a `.env`.

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
  per-app table, modes, and remote-Pi infrastructure.
- [`scripts/hil/all.sh`](../scripts/hil/all.sh) -- the
  HIL-suite driver invoked from CI.
- [`scripts/hil/flash.sh`](../scripts/hil/flash.sh) -- the
  authoritative flash path with auto-recovery.
- [`scripts/hil/dlm_reset.sh`](../scripts/hil/dlm_reset.sh) -- DLM
  recovery for the AHB-AP-gated / TrustZone-locked failure modes.
- [`.github/workflows/hil.yml`](../.github/workflows/hil.yml) -- the
  dev-built, remote-bench HIL gate.
- [`CERTIFICATION_SCOPE.md`](CERTIFICATION_SCOPE.md) -- the
  "no third-party certification" decision.
