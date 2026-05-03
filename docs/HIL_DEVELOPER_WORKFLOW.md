# Hardware-in-the-Loop (HIL) Developer Workflow

This document is the operating manual for any contributor with an
EK-RA8D2 evaluation kit attached to their workstation. It replaces
the deferred "self-hosted CI runner" model from earlier drafts of
`docs/QUALIFICATION_ROADMAP.md` Section 6.

## Decision: no self-hosted CI runner

Per the 2026-05-02 decision, this project will **not** stand up a
self-hosted GitHub Actions runner with an attached EK-RA8D2 board.
Standing up the runner, hardening it against PR-driven flash abuse,
and budgeting for the host hardware is incompatible with the
project's MIT-licensed, $0 personal/research scope (see
`docs/CERTIFICATION_SCOPE.md`).

Instead, **every contributor with an EK-RA8D2 attached runs the
hardware smoke harness locally before `git push`**. The result is
attached to the PR as a Markdown comment using the format below.
Contributors **without** an EK-RA8D2 may still open PRs; the host
unit-test build (`make test`) and the cross-build CI (`firmware.yml`)
are the gating CI signals for those PRs.

## Pre-push checklist (HIL-equipped contributors)

1. Build every EVM app:
   ```sh
   make apps
   ```
2. Confirm the EK-RA8D2 is detected (see "Detecting the J-Link OB"
   below).
3. Run the smoke harness:
   ```sh
   bash scripts/hw_smoke_test.sh
   ```
4. Verify the exit code (`echo $?`):
   - `0` -- every app PASS / WIP / UNKNOWN. Push allowed.
   - `1` -- at least one app FAILED. **Do not push.** Investigate
     the failing app's `build/smoke/<app>.log`, fix the root cause,
     re-run the harness, then push.
   - `2` -- harness misconfiguration (toolchain or board missing).
     Fix the local environment and re-run.
5. Attach `build/smoke/results.md` to the PR (paste as a Markdown
   comment, prefixed by the marker line below).

The opt-in pre-push hook described below automates steps 2-5 on
every `git push`.

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

## PASS criteria

The smoke harness already encodes the classification in
`scripts/hw_smoke_test.sh`. The summary:

- **PASS** -- the firmware reached its main loop or a known
  scheduler entry point. Counts as green.
- **WIP** -- the firmware reached a caught-error sink
  (`panic_halt`, `internal_ra_fatal_error`, etc.). Counts as
  green-with-warning -- the init failed *cleanly*; usually means
  the app needs a vendor blob (`docs/VENDOR_BLOBS.md`) or external
  hardware that the developer does not have wired up.
- **UNKNOWN** -- the chip is alive but the program counter does not
  match any known PASS / WIP pattern. Counts as green-with-warning;
  add a comment to the PR explaining why if you choose to push.
- **FAIL** -- HardFault, lockup, or fall-through to
  `Default_Handler`. **Blocks the push.** This is a real bug.
- **NOBUILD** -- the `.elf` / `.hex` was never built. Re-run
  `make apps`.

## Reporting on the PR

Paste the following block as a PR comment after every smoke run.
The comment marker `<!-- hw-smoke-comment -->` lets the next-run
script update the same comment in place rather than spamming the
thread.

```markdown
<!-- hw-smoke-comment -->
## EK-RA8D2 hardware smoke (developer-local sweep)

<paste contents of build/smoke/results.md here>

---

- Sweep host: <macOS / Linux distro and version>
- J-Link OB serial: <SN from the detection step above>
- Toolchain: <output of `arm-none-eabi-gcc --version | head -1`>
- Date: <ISO 8601 UTC>
```

The PR reviewer is expected to read this block before approving any
PR that touches `libs/ra_hal/` or `examples/ek_ra8d2/`. If the PR
author has no board, they say so explicitly in the PR description
and the reviewer (or another contributor with a board) re-runs the
sweep.

## Opt-in pre-push hook

A local pre-push hook lives at `scripts/git/pre-push`. It is
**opt-in** and **off by default**: contributors without a board
must not be blocked by it.

To enable it on a workstation that does have a board:

```sh
# 1. Install the hook into your local .git/hooks
ln -s ../../scripts/git/pre-push .git/hooks/pre-push

# 2. Tell the hook to actually run (off by default)
git config ra.hw-smoke true
```

To disable temporarily (e.g. when pushing a docs-only PR):

```sh
git config ra.hw-smoke false
```

When enabled, the hook:

1. Re-runs J-Link OB detection. If no board is detected, the hook
   prints a one-line skip notice and exits 0 (push proceeds).
2. If a board is detected, it runs `bash scripts/hw_smoke_test.sh`
   and gates the push on the exit code.
3. Writes a copy of `build/smoke/results.md` to
   `build/smoke/last-pre-push.md` so the developer can paste it
   into the PR comment.

## Cross-references

- `scripts/hw_smoke_test.sh` -- the underlying harness.
- `scripts/git/pre-push` -- the opt-in hook.
- `docs/HARDWARE_BRINGUP.md` -- per-app bring-up notes and the
  PASS/WIP/UNKNOWN/FAIL classification table.
- `docs/qualification/HW_IN_LOOP_RUNNER.md` -- the deferred
  self-hosted-runner build-out, kept for reference but not
  pursued in the current scope.
- `docs/CERTIFICATION_SCOPE.md` -- the "no third-party
  certification" decision that motivates the choice not to fund a
  CI runner.
- `.github/workflows/hardware-smoke.yml` -- documentation-only
  workflow that explains why this isn't running in CI.
