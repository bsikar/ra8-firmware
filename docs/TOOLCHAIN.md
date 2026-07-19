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
| **dev box** (`ssh dev`, x86-64 Debian 12, 6 cores) | Host unit tests, coverage, cppcheck, clang-format/tidy, the `check_*.py` suite, ARM cross-build (pinned 13.3 at `~/opt/arm-gnu-toolchain-13.3`) | Everything host-side + cross-build, FAST -- `make ci-native` runs every gate with no container at all | no Docker (which is fine: `make ci` falls back to native on Linux) |
| **CI** (self-hosted Linux runners; `.github/workflows/`) | The authority -- every gate runs here on push/PR | All gates in the Ubuntu 24.04 devcontainer + a self-hosted `/opt` cross toolchain | -- |
| **HIL rig** (`ssh star@star.local`, Pi + on-board J-Link) | Silicon validation (flash + read the real EK-RA8D2) | The only oracle for cache/power/TZ/timing | -- |

**Golden rule:** ARM builds on the **Mac**; host tests + coverage + lint gates on
the **dev box**; silicon validation on the **HIL rig**; **CI is the arbiter**.

Every gate body lives in `scripts/ci.sh` (registry: `RA8_GATE_REGISTRY`) and each
CI step is a thin `bash scripts/ci.sh --gate <name>` driver, so a local run and
the runner execute the *same functions*. `make ci-native` runs them natively --
the supported path on Linux, no container required. `make ci` wraps the same
suite in the Ubuntu 24.04 devcontainer (`.devcontainer/Dockerfile`, image
`ra8-ci`), which is what macOS needs and what the Mac cannot do without. See
section 4 for the fast pre-push recipe.

---

## 2. Version matrix

Target = the version CI uses (the authority). Keep every environment on the
target; the "status" column flags the known skews.

| Tool | Target (CI) | Mac | dev box | Status |
|------|-------------|-----|---------|--------|
| `arm-none-eabi-gcc` (ship binaries) | **13.3.rel1** (`/opt/arm-gnu-toolchain-13.3`: runner + devcontainer, latter by URL+sha256) | **13.3.1** (`~/opt/arm-gnu-toolchain-13.3`) | **13.3.1** (`~/opt/arm-gnu-toolchain-13.3`) | **CONVERGED** -- pinned 13.3.rel1, enforced; see 3.1 |
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

### 3.1 arm-none-eabi-gcc: converged on 13.3.rel1 everywhere (T5-02 / #178)

Codegen correctness on the attacker-facing `miniz` ZIP inflater is
version-specific (arm-gcc 13.3 miscompiles it under strict aliasing where other
majors do not; worked around with `-fno-strict-aliasing` on the SOUP TUs in
`cmake/ra8_add_app.cmake`), so every environment is pinned to the **same** Arm GNU
Toolchain release: **13.3.rel1** (gcc `13.3.1`).

**How the pin is enforced (#178):**
- **Install path + HINTS.** Each environment installs 13.3.rel1 at a standard
  path: the runner + devcontainer at `/opt/arm-gnu-toolchain-13.3`, the Mac + dev
  box at `~/opt/arm-gnu-toolchain-13.3`. `cmake/toolchain-ra8d2.cmake`
  `find_program`s the cross tools with `HINTS` on those paths (searched before
  `PATH`), so the pinned 13.3 wins regardless of what stray arm-gcc sits on `PATH`
  (e.g. a Homebrew 14.x). Override with `-DRA8_ARM_TOOLCHAIN_BIN=<dir>`.
- **Version assertion.** The toolchain file runs `-dumpfullversion` and requires
  major.minor `13.3` (patch-tolerant; rejects 13.2 and 14.x). A mismatch is a
  **FATAL error by default** (`RA8_STRICT_TOOLCHAIN` defaults ON) -- a build that
  silently picks up a stray arm-gcc fails loudly instead of shipping divergent
  codegen. Pass `-DRA8_STRICT_TOOLCHAIN=OFF` (or `RA8_STRICT_TOOLCHAIN=0` in the
  environment) for a deliberate one-off local build on a different toolchain.
- **Reproducible fetch.** `.devcontainer/Dockerfile` fetches 13.3.rel1 by
  **URL + sha256** (per-arch, `ARM_GCC_SHA256_X86_64` / `_AARCH64`) to
  `/opt/arm-gnu-toolchain-13.3` -- byte-identical to the runner, bundling
  libstdc++ (the old apt `gcc-arm-none-eabi` did not, so C++ apps could not build
  in the devcontainer) and its own newlib. The base image is digest-pinned.

**To move the pin:** bump `ARM_GCC_RELEASE` + both `ARM_GCC_SHA256_*` in the
Dockerfile and `RA8_PINNED_ARM_GCC_VERSION` in the toolchain file, then re-install
at the standard path on the Mac + dev box (download from
`developer.arm.com/downloads/-/arm-gnu-toolchain-downloads`, extract to
`~/opt/arm-gnu-toolchain-<rel>` with `--strip-components=1`). Keep the
`-fno-strict-aliasing` SOUP guard.

**Out-of-repo residual:** the self-hosted runner's `/opt/arm-gnu-toolchain-13.3`
is provisioned outside this repo; the assertion + strict default catch a runner
that drifts off 13.3, but re-provisioning it to a new release is a manual runner
step (not blocking -- the pin is enforced, not just documented).

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

The Mac cannot run host tests. Sync your change onto a clean `origin/dev` and run
the gates on the dev box, where **Linux native IS the CI environment**:

```bash
# 1. clean sync (dev's origin ref lags -- always fetch first)
ssh dev 'cd ~/ra8-firmware && git fetch origin -q && git reset --hard origin/dev -q && git clean -fdq'
COPYFILE_DISABLE=1 tar czf - <changed files> | ssh dev 'cd ~/ra8-firmware && tar xzf - && find . -name "._*" -delete'
# 2. gates -- the SAME functions the runner executes, no container needed
ssh dev 'cd ~/ra8-firmware && make ci-native'        # every gate
ssh dev 'cd ~/ra8-firmware && make ci-native-fast'   # quick pre-push smoke
ssh dev 'cd ~/ra8-firmware && make ci-gate GATE=coverage-report'   # just one
# 3. push (the pre-push hook runs the suite, which the Mac cannot; dev validated it)
SKIP_CI_PUSH=1 git push origin dev
```

> **Do not hand-assemble gate commands.** Every check body lives in exactly one
> place -- `scripts/ci.sh`, listed in its `RA8_GATE_REGISTRY` -- and each CI
> step is a thin `bash scripts/ci.sh --gate <name>` driver. A `/tmp/verify_gates.sh`
> that pastes `clang-format-22 ...; the check_*.py suite; clang-tidy-22 ...` out
> of the workflow is a copy of a copy: it silently stops mirroring CI the moment
> a gate is added, and it has already cost real work here. `make ci-list` prints
> the registry; `make ci-gate GATE=<name>` runs any single gate.
>
> Gates fail loudly when a tool is missing -- they never skip. If a gate reports
> "nothing to check", that is a bug to fix, not a pass.

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
  steps by hand. Root fix: the T1-01 deterministic MMIO seam (`ra8_sim_mmio_*`).
- **The dev box is shared** -- another session may `git reset --hard` it between
  your ssh calls, wiping untracked files. Sync + validate in ONE session.

---

## 5. Convergence status

- **Done:** arm-gcc **13.3.rel1 pinned + enforced everywhere** (section 3.1, #178)
  -- Mac + dev box at `~/opt/arm-gnu-toolchain-13.3`, devcontainer by URL+sha256,
  runner at `/opt`; `-dumpfullversion` `13.3` assertion FATAL by default. gcc-14
  on the dev box (host coverage matches CI). gcovr 8.6, shfmt 3.13.1 aligned.
- **Open (tracked):** Mac clang-format 22.1.8 (section 3.2); Mac ruff 0.15.19
  (section 3.4); re-provisioning the self-hosted runner's `/opt` toolchain on a
  future pin bump (manual runner step, section 3.1).

See also: `CLAUDE.md` (run the gates before every push; one gate definition, in
`scripts/ci.sh`), the `dev-gcc14-coverage-parity` and `dev-box-ci-workflow`
memories.
