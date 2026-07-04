<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 Brighton Sikarskie -->

# Toolchain Reference

Authoritative record of which tool version runs on which machine, why the three
build/verify environments differ, and how to keep them from silently diverging.
Read this before diagnosing a gate that passes locally but fails in CI (or vice
versa) -- the cause is almost always a version skew documented below.

> Scope: this is a **reference** document (per the docs/ policy: architecture,
> policy, reference, and certification artifacts only). It does not track
> transient work -- use GitHub issues for that.

---

## 1. The three environments

| Environment | Role | Can do | Cannot do |
|-------------|------|--------|-----------|
| **Mac** (Apple Silicon, this repo's authoring box) | ARM cross-builds + code authoring + `.py`/`.sh` lint | `arm-none-eabi-gcc` (Cortex-M85), clang-format/clang-tidy, ruff, shfmt, shellcheck, git | Run host unit tests / coverage (macOS arm64 SIGKILLs the `mmap MAP_FIXED <4 GiB` peripheral mock before `main`); `make ci` (Docker + resources) is unreliable |
| **dev box** (`ssh dev`, x86-64 Debian 12, 6 cores) | Host unit tests, coverage, cppcheck, clang-format/tidy, the `check_*.py` suite | Everything host-side and FAST | ARM cross-build for Cortex-M85 (its apt arm-gcc is too old); no Docker |
| **CI** (self-hosted Linux runners; `.github/workflows/`) | The authority -- every gate runs here on push/PR | All gates in the Ubuntu 24.04 devcontainer + a self-hosted `/opt` cross toolchain | -- |
| **HIL rig** (`ssh star@star.local`, Pi + on-board J-Link) | Silicon validation (flash + read the real EK-RA8D2) | The only oracle for cache/power/TZ/timing | -- |

**Golden rule:** ARM builds on the **Mac**; host tests + coverage + lint gates on
the **dev box**; silicon validation on the **HIL rig**; **CI is the arbiter**.

`make ci` reproduces the gate suite inside the Ubuntu 24.04 devcontainer
(`.devcontainer/Dockerfile`, image `ra8d2-ci`). It is Docker-only and heavy; the
dev-box recipe (section 4) is the fast pre-push path.

---

## 2. Version matrix

Target = the version CI uses (the authority). Keep every environment on the
target; the "status" column flags the known skews.

| Tool | Target (CI) | Mac | dev box | Status |
|------|-------------|-----|---------|--------|
| `arm-none-eabi-gcc` (ship binaries) | **13.3** (`/opt/arm-gnu-toolchain-13.3`, `firmware.yml` build-cross); devcontainer apt `15:13.2.rel1-2` | **14.3.1** | too old (unused for cross) | **DIVERGED (major)** -- see 3.1 |
| host `gcc` (coverage / host tests) | **gcc-14** (Ubuntu 24.04 devcontainer) | n/a (host tests SIGKILL) | **gcc-14.2.0** (built from source, `/usr/local/bin`) | CONVERGED -- Std-A, see the `dev-gcc14-coverage-parity` memory |
| `clang-format` | **22.1.8** (`clang-format-22`) | 22.1.7 (Homebrew LLVM) | 22.1.8 (`clang-format-22`) | Mac PATCH-behind -- see 3.2 |
| `clang-tidy` | **22.1.8** | 22.1.7 (Homebrew LLVM) | 22.1.8 | Mac PATCH-behind (same LLVM as clang-format) |
| `ruff` | **0.15.19** (`firmware.yml`) | 0.15.20 | (install per section 3.4) | Mac PATCH-ahead -- see 3.4 |
| `shfmt` | **3.13.1** | 3.13.1 | (install per section 3.4) | Mac CONVERGED |
| `shellcheck` | **0.11.0** | 0.11.x (Homebrew) | (install per section 3.4) | confirm patch |
| `cppcheck` | **2.13** (Ubuntu 24.04) | 2.21 (Homebrew) | 2.13 (built from source, `/usr/local/bin`) | Mac 2.21 emits VERSION-SPECIFIC FALSE POSITIVES -- do NOT use the Mac's; see 3.3 |
| `gcovr` | 8.6 (pip) | 8.6 | 8.6 (`~/.local/bin` + `/usr/bin`) | CONVERGED |

---

## 3. Known divergences + how to handle each

### 3.1 arm-none-eabi-gcc: CI 13.3 vs Mac 14.3.1 (T5-02, version assertion landed)

CI produces the shipping `.elf` with arm-gcc **13.3**; the Mac authors + smoke-
builds with **14.3.1**. Different major -> different codegen. This is the exact
class behind the documented `miniz` inflate strict-aliasing miscompile (arm-gcc
13.3 miscompiles it at `-Og`/`-O2`; mitigated by `-fno-strict-aliasing` on the
SOUP TUs in `cmake/ra_add_app.cmake`).

**Landed (#178):** `cmake/toolchain-ra8d2.cmake` now runs a `-dumpversion` check
against `RA_PINNED_ARM_GCC_MAJOR` (13). By default a mismatch is a **warning** so
a developer on a different major still builds locally; with
`RA_STRICT_TOOLCHAIN=1` in the environment it is a **FATAL error**. The
`firmware.yml` `build-cross` job (the release path) sets `RA_STRICT_TOOLCHAIN=1`,
so a runner whose arm-gcc is not 13.x fails the shipping cross-build loudly
instead of silently shipping version-divergent codegen. The base image is already
digest-pinned (`FROM ubuntu:24.04@sha256:...`) and the devcontainer apt arm-gcc is
version-pinned via `ARM_GCC_VERSION`.

**Remaining (needs runner/Dockerfile access, not autonomous):** pick ONE
arm-gnu-toolchain release and fetch it **by URL + sha256** identically in
`.devcontainer/Dockerfile`, the self-hosted runner's `/opt`, and the Mac setup so
every environment is byte-identical -- then bump the assertion to
`-dumpfullversion` (exact 13.3, not just major 13) and enable
`RA_STRICT_TOOLCHAIN` everywhere.

Until then: build ARM on the Mac (14.3.1) for authoring (you will see the pin
warning), but trust CI's 13.3 build as the shipping artifact, and keep the
`-fno-strict-aliasing` SOUP guard.

### 3.2 clang-format / clang-tidy: Mac 22.1.7 vs CI/dev 22.1.8

Homebrew LLVM on the Mac is one patch behind CI's `clang-format-22` (22.1.8).
For ordinary code they agree (verified: 22.1.7 output passes 22.1.8's
`--dry-run --Werror`), but a patch can flip an edge case and thus the gate.
Handling: format on the Mac for speed, then **verify format on the dev box
(22.1.8) before every push** (section 4). Exact 22.1.8 is not readily installable
on macOS arm64 (LLVM 22 is a pre-release snapshot); revisit if Homebrew catches
up. `scripts/format_code.sh` honors `CLANG_FORMAT=<binary>`.

### 3.3 cppcheck: Mac 2.21 vs CI/dev 2.13

The Mac's cppcheck 2.21 emits version-specific FALSE POSITIVES the gate does not
enforce (startup crt0 `.data`/`.bss` copy-loop `comparePointers`, loop-filled
`uninitvar`, MMIO `knownConditionTrueFalse`, callback `constParameter`). The tree
is CLEAN under 2.13. **Never "fix" off-version cppcheck noise by editing working
code -- run cppcheck 2.13 (dev box, built from source) first.**

### 3.4 ruff / shfmt / shellcheck on the dev box

CI pins ruff==**0.15.19**, shfmt **v3.13.1**, shellcheck **v0.11.0**. The Mac has
these (ruff 0.15.20 is one patch ahead -- align with `pipx install ruff==0.15.19`
or a venv if a ruff-rule skew ever appears). The dev box does not ship them; run
`.py`/`.sh` lint on the Mac (pinned) or install on dev:
```bash
python3 -m venv ~/lintenv && ~/lintenv/bin/pip install ruff==0.15.19
curl -fsSL https://github.com/koalaman/shellcheck/releases/download/v0.11.0/shellcheck-v0.11.0.linux.x86_64.tar.xz | tar xJ
curl -fsSL -o shfmt https://github.com/mvdan/sh/releases/download/v3.13.1/shfmt_v3.13.1_linux_amd64 && chmod +x shfmt
```

---

## 4. Fast pre-push validation (dev box)

`make ci` (Docker) is the faithful reproduction; the dev box is the fast path.
The Mac cannot run host tests. Sync your change onto a clean `origin/dev` and run
the gates there:

```bash
# 1. clean sync (dev's origin ref lags -- always fetch first)
ssh dev 'cd ~/ra8d2-firmware && git fetch origin -q && git reset --hard origin/dev -q && git clean -fdq'
COPYFILE_DISABLE=1 tar czf - <changed files> | ssh dev 'cd ~/ra8d2-firmware && tar xzf - && find . -name "._*" -delete'
# 2. gates (gcc-14 host compiler is auto-selected by scripts/utils/select_host_compiler.sh)
ssh dev 'cd ~/ra8d2-firmware && export PATH=/usr/local/bin:$HOME/.local/bin:$PATH && bash scripts/coverage.sh --gate'   # aggregate 90/80 + per-file floor
ssh dev '... clang-format-22 --dry-run --Werror <files>; the check_*.py suite; clang-tidy-22 ...'
# 3. push (the pre-push hook runs `make ci` which the Mac cannot; bypass -- dev already validated)
SKIP_CI_PUSH=1 git push origin dev
```

Gotchas (each has bitten a push):
- **`ssh dev 'cmd | tail'` masks the exit code** (a pipeline returns `tail`'s
  status = 0). Read the gate's own `[PASS]`/`[FAIL]` line or capture
  `${PIPESTATUS[0]}`.
- **Mac `tar` embeds AppleDouble `._*` sidecars** -- `COPYFILE_DISABLE=1` on the
  Mac side and `find . -name "._*" -delete` on dev, or `file(GLOB test_*.c)`
  compiles the `._` junk and the build breaks.
- **`-T <listfile>` is read on the side it runs.** `ssh dev 'tar -T /tmp/x'`
  reads `/tmp/x` on **dev**, not the Mac -- pass explicit paths, or the tar
  silently archives nothing.
- **Never pipe a tar into `ssh 'bash -s' <<EOF`** -- the heredoc and the tar both
  target stdin and collide. Persist the script on dev first, then
  `tar czf - <files> | ssh dev 'bash ~/script.sh'` (the script reads the tar via
  `tar xzf -`).
- **Stray `.gcda` poisons gcovr's `--root` scan** -- `rm -rf build/* tests/build-*`
  (all trees, incl. `build/asan`/`build/clean`) before a coverage run.
- **The SIGALRM/setitimer test-injection flake** (i2c/adc/rtc/sdhi/i3c and
  others) aborts ~random tests under coverage instrumentation and kills
  `coverage.sh` at its `set -e` ctest step BEFORE the floor check. Defeat with
  `ctest --repeat until-pass:4` then run the gcovr + `check_coverage_floor.py`
  steps by hand. Root fix: the T1-01 deterministic MMIO seam (`ra_sim_mmio_*`).
- **The dev box is shared** -- another session may `git reset --hard` it between
  your ssh calls, wiping untracked files. Sync + validate in ONE session.

---

## 5. Convergence status

- **Done:** gcc-14 on the dev box (host coverage now matches CI's gcc-14; the
  per-file floor is locally verifiable). gcovr 8.6, shfmt 3.13.1 aligned.
- **Open (tracked):** arm-gcc pin (section 3.1, T5-02 / #178); Mac clang-format
  22.1.8 (section 3.2); Mac ruff 0.15.19 (section 3.4); the `-dumpfullversion`
  FATAL assert once all runners are on the pinned arm-gcc.

See also: `CLAUDE.md` (run `make ci` before every push), the
`dev-gcc14-coverage-parity` and `dev-box-ci-workflow` memories.
