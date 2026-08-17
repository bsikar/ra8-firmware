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
| **CI** (the self-hosted `ra8-ci` fleet; `.github/workflows/`) | The authority -- every gate runs here on push/PR | All gates in the Ubuntu 24.04 devcontainer image the runners boot, cross toolchain included | -- |
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
| `cmake-format` / `cmake-lint` | **0.6.13** (`cmakelang`, `firmware.yml` + devcontainer) | (install per section 3.5) | 0.6.13 (`~/.local/bin`) | CONVERGED -- see 3.5 |
| `yamllint` | **1.37.1** | (install per section 3.5) | 1.37.1 (`~/.local/bin`) | CONVERGED -- see 3.5 |
| `actionlint` | **1.7.7** | (install per section 3.5) | 1.7.7 (`~/.local/bin`) | CONVERGED -- see 3.5 |
| `gcovr` | 8.6 (pip) | 8.6 | 8.6 (`~/.local/bin` + `/usr/bin`) | CONVERGED |
| `libunicorn` (ra8_emulator) | **2.1.4** (source build -> `/usr/local`) | 2.1.4 (source build) | **2.1.4** (source build -> `/usr/local`) | pinned + FAIL-LOUD; dev box needs the source build -- see 3.6 (#354) |

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

**No longer an out-of-repo residual:** `/opt/arm-gnu-toolchain-13.3` used to be
hand-provisioned on the bare-metal `k3s-runner-*` services. That pool is
retired, and every runner answering `ra8-ci` now boots
`localhost/ra8-ci-runner:v2`, which fetches that exact release to that exact
path by URL + sha256 in `.devcontainer/Dockerfile`. Bumping the pin there and
rebuilding the image reprovisions the whole fleet; the assertion + strict
default still catch a box that drifts off 13.3.

### 3.2 clang-format / clang-tidy: Mac 22.1.7 vs CI/dev 22.1.8

Homebrew LLVM on the Mac is one patch behind CI's `clang-format-22` (22.1.8).
For ordinary code they agree (verified: 22.1.7 output passes 22.1.8's
`--dry-run --Werror`), but a patch can flip an edge case and thus the gate.
Handling: format on the Mac for speed, then **verify format on the dev box
(22.1.8) before every push** (section 4). Exact 22.1.8 is not readily installable
on macOS arm64 (LLVM 22 is a pre-release snapshot); revisit if Homebrew catches
up. `scripts/checks/format_code.sh` honors `CLANG_FORMAT=<binary>`.

### 3.3 cppcheck: Mac 2.21 vs CI/dev 2.13

The Mac's cppcheck 2.21 emits version-specific FALSE POSITIVES the gate does not
enforce (startup crt0 `.data`/`.bss` copy-loop `comparePointers`, loop-filled
`uninitvar`, MMIO `knownConditionTrueFalse`, callback `constParameter`). The tree
is CLEAN under 2.13. **Never "fix" off-version cppcheck noise by editing working
code -- run cppcheck 2.13 (dev box, built from source) first.**

### 3.4 ruff / shfmt / shellcheck on the dev box

CI pins ruff==**0.15.19**, shfmt **v3.13.1**, shellcheck **v0.11.0**. The dev box
now ships all three at exactly those versions, in `~/.local/bin`, so
`make ci-gate GATE=lint-py-shell` on dev is CI-faithful. The Mac has them too
(ruff 0.15.20 is one patch ahead -- align with `pipx install ruff==0.15.19` if a
ruff-rule skew ever appears).

**Resolve them through a login shell.** `~/.local/bin` is added to `PATH` by the
profile, so a plain `ssh dev '<cmd>'` does not see any of the three while
`ssh dev 'bash -lc "<cmd>"'` does -- the gate then fails on a missing tool that
is in fact installed. The same trap changes which `clang-tidy` and `gcovr` you
get (#333). Always use `bash -lc`.

### 3.5 cmake-format / cmake-lint / yamllint / actionlint

The `lint-cmake` and `lint-yaml` gates (#362) pin `cmakelang`==**0.6.13**
(which provides both `cmake-format` and `cmake-lint`), `yamllint`==**1.37.1**
and `actionlint` **v1.7.7**. All four are installed on the dev box in
`~/.local/bin`, baked into the devcontainer image, and provisioned on the
runner by the "Install the config-as-code linters" step in `firmware.yml`.
Keep those three places in step when moving a pin.

Install on a fresh box:

```sh
python3 -m pip install --user cmakelang==0.6.13 yamllint==1.37.1
curl -fsSL https://github.com/rhysd/actionlint/releases/download/v1.7.7/actionlint_1.7.7_linux_amd64.tar.gz \
  | tar -xz -C ~/.local/bin actionlint
```

Both gates call `require_cmd`, so a missing tool FAILS the gate -- it never
skips. The same `bash -lc` login-shell rule as 3.4 applies.

Configuration lives at the repo root: `.cmake-format.yaml` (shared by the
formatter and the linter) and `.yamllint.yaml`, plus `.github/actionlint.yaml`
for the self-hosted runner labels. Each records WHY any default was widened.

### 3.5 ccache: shared across agents, bypassed for instrumented builds

`cmake/ccache.cmake` wires ccache in as `CMAKE_<LANG>_COMPILER_LAUNCHER` for the
host builds, the arm-none-eabi cross builds and the containerised gate run
alike, and the dev box points every one of them at one shared cache
(`CCACHE_DIR=/var/cache/ccache-ra8`). Measured on that box: a cross build goes
0% -> **100%** hits on a rebuild into a *different* build directory, and the
host test build 1.25% -> **100%**.

Hits survive across build directories only because the cache sets `base_dir = /`
and `hash_dir = false` (and `scripts/ci.sh` exports the `CCACHE_BASEDIR` /
`CCACHE_NOHASHDIR` equivalents into the container). Every `make ci` builds in a
fresh `mktemp` snapshot, so without that normalisation the hit rate is flat
zero rather than merely lower.

**Coverage and MC/DC builds deliberately bypass the cache** and say so
(`ccache: DISABLED for this build`). gcov records absolute source and object
paths inside the `.gcno`, so a cached object replayed into a different build
directory makes gcovr resolve nothing and report `no_working_dir_found` -- a
failure indistinguishable from a real coverage regression. Do not "fix" that
opt-out.

### 3.6 libunicorn (ra8_emulator CPU emulator): pinned 2.1.4, source-built, fail-loud (#354)

`tools/ra8_emulator` boots the real cross-compiled firmware `.elf` on Unicorn
(QEMU's core as a library). **Different Unicorn versions decode Armv8.1-M
differently** -- notably the Cortex-M85's Helium/MVE store family -- so the
emulator's verdict for a byte-identical `.elf` depends on which Unicorn is
installed. Left unpinned, "same commit, green here, faulting there" is
structural, which is exactly what #354 was: the self-hosted runner linked a
source-built **2.1.4** in `/usr/local`, while the dev box and the devcontainer
linked Debian/Ubuntu apt **2.0.1** -- and 2.0.1 raises a spurious `EXCP_NOCP` on
the MVE stores that 2.1.4 (and real M85 silicon) executes. That made ~6 EPUB /
comic / crypto smoke apps and a wider EIL set fault under `make ci` on dev's own
HEAD while CI stayed green.

**The pin is 2.1.4** -- the version CI (the authority) already links and the one
where the ra8_emulator smoke / matrix / EIL suite is green. It is the same choice
as the arm-gcc pin: one exact upstream release, **built from source**, verified
by sha256, so the library is byte-reproducible on every box rather than
"whatever apt offered the day the runner was provisioned".

How it is enforced (three parts, no assumptions):

- **One source of truth.** `scripts/ci/unicorn_pin.sh` holds the version
  (`RA8_UNICORN_VERSION=2.1.4`), the release-tarball URL, and its sha256. The
  devcontainer duplicates the version + sha256 as `UNICORN_VERSION` /
  `UNICORN_SHA256` build args (`.devcontainer/Dockerfile`); keep the two in step
  when moving the pin, exactly as the arm-gcc pin is duplicated into
  `cmake/toolchain-ra8d2.cmake`.
- **Reproducible install.** `scripts/ci/install_unicorn.sh` downloads the pinned
  tarball, checks the sha256, builds the `arm` target (`-DUNICORN_ARCH=arm`,
  Release, shared), and installs to `/usr/local` on Linux or
  `~/.local/ra8-firmware/unicorn` on macOS (`RA8_UNICORN_PREFIX` overrides
  either). It is idempotent -- already-pinned is a no-op. The devcontainer runs
  the same source build in its `Dockerfile`. It is provisioning, **not** a gate,
  and is deliberately never invoked from a workflow step (the `ci-parity` gate
  forbids an "infra" step from calling anything under `scripts/`).
- **Fail-loud check.** Every emulator gate (`emulator-smoke`,
  `emulator-matrix`, `emulator-io-fabric`, `eil-integration`) calls
  `require_pinned_unicorn` in `scripts/ci.sh`, which runs
  `scripts/checks/check_unicorn_version.sh`. That check compiles a probe against
  the ACTUAL `libunicorn` ra8_emulator will link, reads `uc_version()` + the header
  `UC_VERSION_*` macros + `pkg-config`, and **exits non-zero with remediation**
  when any disagrees with the pin. Run its `--selftest` to see it reject a 2.0.1
  skew and accept a 2.1.4 match. This is the honesty half: the removed guard
  (`if ! ldconfig | grep libunicorn; then apt-get install ...`) checked only that
  *something named libunicorn existed*, never which version -- so a fossil
  install produced green ra8_emulator runs nobody could reproduce. A skewed or
  absent Unicorn now fails the gate instead of quietly passing.

**Provision a Linux box** (dev box, or a fresh runner):

```sh
bash scripts/ci/install_unicorn.sh            # -> /usr/local (needs sudo for a system prefix)
RA8_UNICORN_PREFIX=$HOME/opt/unicorn bash scripts/ci/install_unicorn.sh   # per-user, no sudo
```

On macOS, run `make emu-setup` from a clone for the one-command setup. It
requires only Apple's Command Line Tools (not the full Xcode app), uses the
user-writable default prefix, and configures the emulator to find it without
environment variables. The installer applies a narrow Unicorn 2.1.4
compatibility change that skips an AArch64 `CTR_EL0` cache-register probe on
Darwin; macOS can deny the preceding sysctl query, and that privileged probe
otherwise terminates the emulator with `SIGILL`. The generic fallback cache
line size is safe for Unicorn's JIT.

**No longer an out-of-repo residual.** This used to be a manual step on the
bare-metal `k3s-runner-*` services on `k3s-pve`, whose `/usr/local` Unicorn was
hand-provisioned and would have needed re-running by hand after any re-image.
That pool is retired: every runner answering `ra8-ci` -- the ARC pods and the
truenas container alike -- boots `localhost/ra8-ci-runner:v2`, which builds
Unicorn **2.1.4** from source by URL + sha256 in `.devcontainer/Dockerfile`. The
pin is therefore provisioned by the same file that declares it, and a re-image
reproduces it rather than losing it. The fail-loud check above is unchanged and
is still what guarantees a skew cannot pass silently; `install_unicorn.sh`
remains the recipe for a bare box (a dev box, or a new runner shape).

---

## 4. Fast pre-push validation (dev box)

The Mac cannot run host tests. Run the gates on the dev box, where **Linux
native IS the CI environment**:

**Get your own workspace first.** `~/ra8-firmware` is shared by every agent on
the box, and `git reset --hard` in it is how two agents had their checkouts
clobbered mid-run, corrupting a baseline measurement and an EIL run. Never work
there and never improvise a checkout of your own -- `make ws-new` exists so you
do not have to:

```bash
# 0. isolated workspace (a linked worktree; costs a checkout, not a clone)
ssh dev 'bash -lc "cd ~/ra8-firmware && make ws-new NAME=my-task"'
# ... then work in ~/ra8-ws/my-task, and `make ws-free NAME=my-task` when done.
# 1. put your commit in it -- push a branch and check it out there; do NOT rsync
#    into the shared tree.
# 2. gates -- the SAME functions the runner executes
ssh dev 'bash -lc "cd ~/ra8-ws/my-task && make ci"'                     # container (works from a worktree, #334)
ssh dev 'bash -lc "cd ~/ra8-ws/my-task && make ci-native"'              # every gate, no container
ssh dev 'bash -lc "cd ~/ra8-ws/my-task && make ci-gate GATE=coverage-tree"'   # just one
# 3. push (the pre-push hook runs the suite, which the Mac cannot; dev validated it)
SKIP_CI_PUSH=1 git push origin dev
```

`bash -lc` is not optional -- see section 3.4. And do not poll GitHub with
`gh run watch`: the REST quota is shared and ~18 concurrent watchers have
exhausted it twice in a day. Use `make ci-status` (exit 3 means UNKNOWN, which
is neither a pass nor a failure).

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
  `tree_coverage.sh` at its `set -e` ctest step BEFORE the policy runs. Defeat
  with `ctest --repeat until-pass:4` then run the gcovr +
  `check_tree_coverage.py` steps by hand. Root fix: the T1-01 deterministic MMIO seam (`ra8_fake_mmio_*`).
- **The dev box is shared** -- another session may `git reset --hard` it between
  your ssh calls, wiping untracked files. Sync + validate in ONE session.

---

## 5. Convergence status

- **Done:** arm-gcc **13.3.rel1 pinned + enforced everywhere** (section 3.1, #178)
  -- Mac + dev box at `~/opt/arm-gnu-toolchain-13.3`, devcontainer by URL+sha256,
  runner at `/opt`; `-dumpfullversion` `13.3` assertion FATAL by default. gcc-14
  on the dev box (host coverage matches CI). gcovr 8.6, shfmt 3.13.1 aligned.
- **Done:** libunicorn **2.1.4 pinned + FAIL-LOUD** (section 3.6, #354) -- runner
  already at 2.1.4 (`/usr/local`), devcontainer builds it from source by
  URL+sha256, `require_pinned_unicorn` fails every emulator gate on a skew.
  Provision the dev box with `scripts/ci/install_unicorn.sh` so `make ci`'s
  ra8_emulator gates match CI.
- **Done:** both former manual runner steps are gone (#502). Re-provisioning
  `/opt/arm-gnu-toolchain-13.3` (section 3.1) and re-running
  `install_unicorn.sh` (section 3.6) were only ever needed on the bare-metal
  `k3s-runner-*` pool; that pool is retired and the whole `ra8-ci` fleet now
  boots the pinned runner image, so both tools are provisioned by the same
  Dockerfile that declares their pins.
- **Open (tracked):** Mac clang-format 22.1.8 (section 3.2); Mac ruff 0.15.19
  (section 3.4).

See also: `CLAUDE.md` (run the gates before every push; one gate definition, in
`scripts/ci.sh`), the `dev-gcc14-coverage-parity` and `dev-box-ci-workflow`
memories.
