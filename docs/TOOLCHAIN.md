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
| **Mac** (Apple Silicon, this repo's authoring box) | ARM cross-builds + code authoring + full CI through the devcontainer | `arm-none-eabi-gcc` (Cortex-M85), clang-format/clang-tidy, ruff, shfmt, shellcheck, git, and `just ci` | Run the low-address host unit tests / coverage natively (macOS arm64 rejects the `mmap MAP_FIXED <4 GiB` peripheral mock before `main`); use `just ci` for their Linux execution |
| **dev box** (`ssh dev`, x86-64 Debian 12, 12 cores) | Host unit tests, coverage, cppcheck, clang-format/tidy, the `check_*.py` suite, ARM cross-build (pinned 13.3 under `/opt`) | Everything host-side + cross-build, FAST -- `just quality::native` runs every gate with no container at all | no Docker (which is fine: `just ci` falls back to native on Linux) |
| **CI** (the self-hosted `ra8-ci` fleet; `.github/workflows/`) | The authority -- every gate runs here on push/PR | All gates in the Ubuntu 24.04 devcontainer image the runners boot, cross toolchain included | -- |
| **HIL rig** (`ssh star@star.local`, Pi + on-board J-Link) | Silicon validation (flash + read the real EK-RA8D2) | The only oracle for cache/power/TZ/timing | -- |

**Golden rule:** use `just ci` on the **Mac**, `just quality::native` on the
**dev box**, silicon validation on the **HIL rig**, and treat **CI as the
arbiter**.

HIL-facing shell entrypoints use the supported hosts' fixed `/bin/bash -p`.
This is an intentional security exception to portable env shebangs: the HIL
call graph crosses SSH, generated services, and physical-hardware boundaries,
so it must ignore `BASH_ENV` and exported shell functions from its caller.

Every gate body lives in `scripts/ci.sh` (registry: `RA8_GATE_REGISTRY`) and each
CI step is a thin `just quality::local::gate <name>` driver, so a local run and
the runner execute the *same functions*. `just quality::native` runs them natively --
the supported path on Linux, no container required. `just ci` wraps the same
suite in the Ubuntu 24.04 devcontainer (`.devcontainer/Dockerfile`, image
`ra8-ci`), which is what macOS needs and what the Mac cannot do without. See
section 4 for the fast pre-push recipe.

---

## 1.1 Repository bootstrap

`just setup` is the supported bootstrap on macOS, Linux, and WSL2. The only
tools it expects in advance are Git, `just`, a stdlib Python 3.11 through
3.14, trusted CA certificates/network access for the first fetch, and one
usable container runtime (`podman`, `docker`, or `nerdctl`; Docker on macOS
uses Colima). uv creates the environment; host `venv` and `pip` are not
bootstrap prerequisites. It:

1. creates or updates the ignored `.venv` from exact runtime pins in
   `pyproject.toml` and the complete transitive set in `uv.lock`;
2. installs immutable-HEAD Git hook launchers under the shared Git common
   directory (the explicit `just hooks` step; CMake never mutates Git config);
3. converges exact repository-local Ansible Galaxy collections; and
4. asks `scripts/ci/devcontainer_image.sh` to build or refresh `ra8-ci:latest`
   from the tightly allowlisted locked root context.

`just dev-shell` enters that image with the checkout mounted read-write, so
the compiler and analyzer installation created by `just setup` is immediately
usable for ordinary development commands without modifying host packages.

No step writes into the host's system Python. Use `just setup-python` for the
lighter Python-and-hooks refresh when the container image is already current.
Linux fleet machines remain provisioned through the declared Ansible roles;
macOS emulator-only native dependencies remain owned by
`just apps::emulator::setup`. Neither installer is duplicated by bootstrap.
Native Windows is limited to the tested uv bootstrap mapping; use WSL2 for the
full repository. See
[Python Environments and Lock Maintenance](PYTHON_ENVIRONMENTS.md) for group,
managed-service, and lock-update ownership.

---

## 2. Version matrix

Target = the version CI uses (the authority). Keep every environment on the
target; the "status" column flags the known skews.

| Tool | Target (CI) | Mac | dev box | Status |
|------|-------------|-----|---------|--------|
| `arm-none-eabi-gcc` (ship binaries) | **13.3.rel1** (`/opt/arm-gnu-toolchain-13.3`: runner + devcontainer, latter by URL+sha256) | **13.3.1** (`~/opt/arm-gnu-toolchain-13.3`) | **13.3.1** (`/opt/arm-gnu-toolchain-13.3`) | **CONVERGED** -- pinned 13.3.rel1, enforced; see 3.1 |
| host `gcc` (coverage / host tests) | **gcc-14** (Ubuntu 24.04 devcontainer) | n/a (host tests SIGKILL) | **gcc-14.2.0** (built from source, `/usr/local/bin`) | CONVERGED -- Std-A, see the `dev-gcc14-coverage-parity` memory |
| `just` | **1.40.0** (`JUST_VERSION`, devcontainer) | 1.58.0 | 1.40.0 (`/usr/local/bin`) | Mac may run recipes, but CI formatting/reference checks use the pinned devcontainer release |
| GitHub Actions runner | **2.336.0** (ARC image, linux/amd64 manifest digest pinned) | n/a | **2.336.0** (native HIL listener, release archive SHA-256 pinned) | CONVERGED -- both Ansible-managed runner paths use the same reviewed release |
| `clang-format` | **22.1.8** (`clang-format-22`) | 22.1.8 (Homebrew LLVM) | 22.1.8 (`clang-format-22`) | CONVERGED -- see 3.2 |
| `clang-tidy` | **18.1.8** (`clang-tidy-18`, from `clang-tools-18`) | 22.1.8 (Homebrew LLVM) | 18.1.8 (`clang-tidy-18`) | Mac is on a DIFFERENT MAJOR -- not the same LLVM as clang-format; see 3.2 |
| `ruff` | **0.15.19** (`pyproject.toml` / `uv.lock`) | 0.15.19 (`.venv`) | 0.15.19 (`/opt/ra8-python-tools`) | CONVERGED -- see 3.4 |
| `shfmt` | **3.13.1** | 3.13.1 | 3.13.1 (`/usr/local/bin`) | CONVERGED -- see 3.4 |
| `shellcheck` | **0.11.0** | 0.11.0 | 0.11.0 (`/usr/local/bin`) | CONVERGED -- see 3.4 |
| `cppcheck` | **2.13** (Ubuntu 24.04) | 2.21 (Homebrew) | 2.13 (built from source, `/usr/local/bin`) | Mac 2.21 emits VERSION-SPECIFIC FALSE POSITIVES -- do NOT use the Mac's; see 3.3 |
| `cmake-format` / `cmake-lint` | **0.6.13** (`cmakelang`, `pyproject.toml` / `uv.lock`) | 0.6.13 (`.venv`) | 0.6.13 (`/opt/ra8-python-tools`) | CONVERGED -- see 3.5 |
| `yamllint` | **1.37.1** (`pyproject.toml` / `uv.lock`) | 1.37.1 (`.venv`) | 1.37.1 (`/opt/ra8-python-tools`) | CONVERGED -- see 3.5 |
| `actionlint` | **1.7.7** | 1.7.7 (devcontainer) | 1.7.7 (`/usr/local/bin`) | CONVERGED -- see 3.5 |
| `gcovr` | **7.0** (`pyproject.toml` / `uv.lock`) | 7.0 (`.venv`) | 7.0 (`/opt/ra8-python-tools`, uv-synchronized) | Exact pin; gcovr 8.4+ changes counts for white-box source variants |
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
  path: the runner, devcontainer and dev box at
  `/opt/arm-gnu-toolchain-13.3`, and the Mac at
  `~/opt/arm-gnu-toolchain-13.3`. `cmake/toolchain-ra8d2.cmake`
  `find_program`s the cross tools with `HINTS` on those paths (searched before
  `PATH`), so the pinned 13.3 wins regardless of what stray arm-gcc sits on `PATH`
  (e.g. a Homebrew 14.x). Override with `-DRA8_ARM_TOOLCHAIN_BIN=<dir>`.
- **Version assertion.** The toolchain file runs `-dumpfullversion` and requires
  major.minor `13.3` (patch-tolerant; rejects 13.2 and 14.x). A mismatch is a
  **FATAL error by default** (`RA8_STRICT_TOOLCHAIN` defaults ON) -- a build that
  silently picks up a stray arm-gcc fails loudly instead of shipping divergent
  codegen. Pass `-DRA8_STRICT_TOOLCHAIN=OFF` (or `RA8_STRICT_TOOLCHAIN=0` in the
  environment) for a deliberate one-off local build on a different toolchain.
- **Reproducible fetch.** `.devcontainer/Dockerfile` owns the 13.3.rel1 release
  and **URL + sha256** pins for all supported host assets: Linux x86-64 /
  AArch64 and Darwin x86-64 / arm64. The container consumes the matching Linux
  `ARM_GCC_SHA256_X86_64` / `_AARCH64` archive and installs it to
  `/opt/arm-gnu-toolchain-13.3` -- byte-identical to the runner, bundling
  libstdc++ (the old apt `gcc-arm-none-eabi` did not, so C++ apps could not build
  in the devcontainer) and its own newlib. `scripts/emu/setup_macos.sh` derives
  the release and Darwin hash from those same ARGs and selects Arm's
  `darwin-arm64` or `darwin-x86_64` archive. Linux and Darwin archives are
  platform-specific and therefore intentionally have different hashes. The
  base image is digest-pinned.

**To move the pin:** bump `ARM_GCC_RELEASE` + all four `ARM_GCC_SHA256_*` values
in the Dockerfile and `RA8_PINNED_ARM_GCC_VERSION` in the toolchain file. On
the Mac, download from
`developer.arm.com/downloads/-/arm-gnu-toolchain-downloads` and extract to
`~/opt/arm-gnu-toolchain-<rel>` with `--strip-components=1`; converge the dev
box through Ansible so its managed install moves under `/opt`. Keep the
`-fno-strict-aliasing` SOUP guard.

**No longer an out-of-repo residual:** `/opt/arm-gnu-toolchain-13.3` used to be
hand-provisioned on the bare-metal `k3s-runner-*` services. That pool is
retired, and every runner answering `ra8-ci` now boots
`localhost/ra8-ci-runner:v2`, which fetches that exact release to that exact
path by URL + sha256 in `.devcontainer/Dockerfile`. Bumping the pin there and
rebuilding the image reprovisions the whole fleet; the assertion + strict
default still catch a box that drifts off 13.3.

### 3.2 clang-format: converged at 22.1.8 (clang-tidy is pinned separately)

Homebrew LLVM and CI's `clang-format-22` both resolve to 22.1.8. The format
gate still asserts the exact version so a future package update cannot change
the rendered tree silently. `scripts/checks/format_code.sh` honors
`CLANG_FORMAT=<binary>` for an explicit local pin.

`clang-tidy` does **not** share that pin. The `tidy` gate resolves
`clang-tidy-18` -- the `clang-tools-18` major the devcontainer installs -- and
`require_tool_versions clang-tidy-18` fails the gate under any other major, so a
Homebrew clang-tidy from the LLVM 22 snapshot is not a substitute for it.

### 3.3 cppcheck: Mac 2.21 vs CI/dev 2.13

The Mac's cppcheck 2.21 emits version-specific FALSE POSITIVES the gate does not
enforce (startup crt0 `.data`/`.bss` copy-loop `comparePointers`, loop-filled
`uninitvar`, MMIO `knownConditionTrueFalse`, callback `constParameter`). The tree
is CLEAN under 2.13. **Never "fix" off-version cppcheck noise by editing working
code -- run cppcheck 2.13 (dev box, built from source) first.**

### 3.4 ruff / shfmt / shellcheck on the dev box

CI pins ruff==**0.15.19**, shfmt **v3.13.1**, shellcheck **v0.11.0**. The dev box
ships ruff from `/opt/ra8-python-tools` and the native shell tools from
`/usr/local/bin`, all at the Dockerfile versions, so
`just quality::local::gate lint-py-shell` on dev is CI-faithful. Local
`just setup-python` installs the ruff pin in `.venv`; `just setup` also ensures
the devcontainer containing the two native shell tools.

Native release binaries are pinned by version **and** a per-architecture
sha256 in `.devcontainer/Dockerfile`. The container build and dev-box
provisioner download each asset to a temporary file, verify it, and only then
extract or install it. `check_download_installers.py` continuously guards that
flow and rejects the former curl-to-parser installer idioms.

**Resolve managed tools through a login shell.** The fleet profile prepends
`/opt/ra8-python-tools/bin`, so a plain `ssh dev '<cmd>'` may miss a provisioned
Python tool while `ssh dev '/bin/bash -p -lc "<cmd>"'` sees it. The same trap
changes which `clang-tidy` and `gcovr` you get (#333). Always use
`/bin/bash -p -lc`.

### 3.5 cmake-format / cmake-lint / yamllint / actionlint

The `lint-cmake` and `lint-yaml` gates (#362) pin `cmakelang`==**0.6.13**
(which provides both `cmake-format` and `cmake-lint`), `yamllint`==**1.37.1**
and `actionlint` **v1.7.7**. `just setup-python` installs the Python tools in
the repository-local `.venv`; the devcontainer, runner, and provisioned dev box
install their Python tools in `/opt/ra8-python-tools`. `actionlint` is installed
as a pinned native binary in those managed environments. The non-Python
Dockerfile ARGs are the single pin authority: local setup, workflows, and
fleet provisioning
derive their versions instead of duplicating them. Run `just setup` on a fresh
clone to prepare both the local Python environment and the complete pinned
devcontainer toolchain.

The macOS emulator bootstrap follows the same trust boundary for Homebrew: its
installer is fetched from an exact `Homebrew/install` commit, checked against
the recorded sha256, and executed from the verified temporary file. Updating
that pin requires reviewing the new installer and updating both values
together; using the mutable `HEAD/install.sh` URL is forbidden.

Both gates call `require_cmd`, so a missing tool FAILS the gate -- it never
skips. The same `bash -lc` login-shell rule as 3.4 applies.

Configuration lives at the repo root: `.cmake-format.yaml` (shared by the
formatter and the linter) and `.yamllint.yaml`, plus `.github/actionlint.yaml`
for the self-hosted runner labels. Each records WHY any default was widened.

### 3.5 ccache: shared across agents, bypassed for instrumented builds

`cmake/ccache.cmake` wires ccache in as `CMAKE_<LANG>_COMPILER_LAUNCHER` for the
host builds, the arm-none-eabi cross builds and the containerised gate run
alike, and the dev box points every one of them at one shared cache
(`CCACHE_DIR=/var/cache/ccache-ra8`). Measured on that box, a rebuild into a
*different* build directory goes from almost no hits to almost all of them,
for the cross build and the host test build alike.

Hits survive across build directories only because the cache sets `base_dir = /`
and `hash_dir = false` (and `scripts/ci.sh` exports the `CCACHE_BASEDIR` /
`CCACHE_NOHASHDIR` equivalents into the container). Every `just ci` builds in a
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
comic / crypto smoke apps and a wider EIL set fault under `just ci` on dev's own
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
/bin/bash -p scripts/ci/install_unicorn.sh     # -> /usr/local (needs sudo for a system prefix)
RA8_UNICORN_PREFIX=$HOME/opt/unicorn /bin/bash -p scripts/ci/install_unicorn.sh  # per-user
```

On macOS, run `just apps::emulator::setup` from a clone for the one-command setup. It
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
there and never improvise a checkout of your own -- `just workspace::new` exists so you
do not have to:

```bash
# 0. isolated workspace (a linked worktree; costs a checkout, not a clone)
ssh dev '/bin/bash -p -lc "cd ~/ra8-firmware && just workspace::new my-task"'
# ... then work in ~/ra8-ws/my-task, and `just workspace::free my-task` when done.
# 1. put your commit in it -- push a branch and check it out there; do NOT rsync
#    into the shared tree.
# 2. gates -- the SAME functions the runner executes
ssh dev '/bin/bash -p -lc "cd ~/ra8-ws/my-task && just ci"'              # container
ssh dev '/bin/bash -p -lc "cd ~/ra8-ws/my-task && just quality::native"' # native
ssh dev '/bin/bash -p -lc "cd ~/ra8-ws/my-task && just quality::local::gate coverage-tree"'
# 3. push (the pre-push hook runs the suite, which the Mac cannot; dev validated it)
SKIP_CI_PUSH=1 git push origin dev
```

`/bin/bash -p -lc` is not optional -- see section 3.4. And do not poll GitHub with
`gh run watch`: the REST quota is shared and ~18 concurrent watchers have
exhausted it twice in a day. Use `just quality::local::gate ci-status-contract` (exit 3 means UNKNOWN, which
is neither a pass nor a failure).

> **Do not hand-assemble gate commands.** Every check body lives in exactly one
> place -- `scripts/ci.sh`, listed in its `RA8_GATE_REGISTRY` -- and each CI
> step is a thin `just quality::local::gate <name>` driver. A `/tmp/verify_gates.sh`
> that pastes `clang-format-22 ...; the check_*.py suite; clang-tidy-22 ...` out
> of the workflow is a copy of a copy: it silently stops mirroring CI the moment
> a gate is added, and it has already cost real work here.
> `just quality::gate::list` prints the registry;
> `just quality::gate::run <name>` runs one gate in the supported environment
> for the current host. Use `just quality::local::gate <name>` only when the
> native host is CI-compatible.
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
- **Never pipe a tar into `ssh '/bin/bash -p -s' <<EOF`** -- the heredoc and the tar both
  target stdin and collide. Persist the script on dev first, then
  `tar czf - <files> | ssh dev '/bin/bash -p ~/script.sh'` (the script reads the tar via
  `tar xzf -`).
- **Stray `.gcda` poisons gcovr's `--root` scan** -- `rm -rf build/* tests/build-*`
  (all trees, incl. `build/asan`/`build/clean`) before a coverage run.
- **The SIGALRM/setitimer test-injection flake** (i2c/adc/rtc/sdhi/i3c and
  others) aborts ~random tests under coverage instrumentation and kills
  `tree_coverage.sh` at its `set -e` ctest step BEFORE the policy runs.
  Defeat with `ctest --repeat until-pass:4` then run the gcovr +
  `check_tree_coverage.py` steps by hand. Root fix: the T1-01
  deterministic MMIO seam (`ra8_fake_mmio_*`).
- **The dev box is shared** -- another session may `git reset --hard` it between
  your ssh calls, wiping untracked files. Sync + validate in ONE session.

---

See also: `CLAUDE.md` (run the gates before every push; one gate definition, in
`scripts/ci.sh`), the `dev-gcc14-coverage-parity` and `dev-box-ci-workflow`
memories.
