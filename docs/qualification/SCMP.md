# Software Configuration Management Plan (SCMP)

**Last refreshed**: 2026-05-03 (numbers re-synced to live counts).

**Status**: First draft, 2026-05-02. Populated during Phase 7 of
`docs/QUALIFICATION_ROADMAP.md`. Subject to revision after the first
external assessor review.

**DO-178C reference**: Section 11.4 (SCMP content) and Section 7
(Configuration Management Process).
**IEC 61508-3 reference**: Clause 6.2.3 (Configuration management).
**ISO 26262-8 reference**: Clause 7 (Configuration management).

**Owner**: Brighton Sikarskie (single developer / maintainer).

---

## 1. Configuration items

The configuration item set is the union of everything tracked in this
git repository, plus the pinned versions of pre-existing software
listed in `docs/SOUP/`. The repository root is the single source of
truth.

### 1.1 First-party source code

| Path                              | Description                                                  |
|-----------------------------------|--------------------------------------------------------------|
| `src/`                            | Shared internals (no boot code, no `main`).                  |
| `src/inc/`                        | Internal headers shared between translation units.           |
| `src/secure_app/`                 | Ring 5 secure-side substrate (key vault, secure-only logic). |
| `libs/ra8_core/`                   | `ra8_err`, `ra8_check`, `ra8_log`, `ra8_assert`, helpers.        |
| `libs/ra8_hal/`                    | Peripheral drivers and register header files.                |
| `libs/ra8_nsc/`                    | TrustZone non-secure-callable veneers.                       |
| `libs/ra8_*_pal/`                  | Platform abstraction layers (`ra8_net_pal`, `ra8_usb_pal`, ...).|
| `libs/ra8_psa_crypto/`             | PSA Crypto integration shim.                                 |
| `libs/ra8_modem_at/`               | AT-command modem stack.                                      |
| `libs/ra8_power_profile/`          | Power-profile management.                                    |
| `libs/ra8_ota/`                    | OTA orchestration (Phase 5).                                 |
| `examples/ek_ra8d2/<app>/`        | Self-contained EVM applications (26 today).                  |
| `examples/_unsupported/<app>/`    | Shelved applications (10 today).                             |

### 1.2 Per-application boot code

Each application directory under `examples/<tier>/<app>/` contains
its own boot artifacts so that future divergence between apps is an
explicit design goal:

- `main.c`
- `vector_table.c`
- `system_init.c`
- `secure_exception.c`
- `trustzone_init.{c,h}`
- `linker_script.ld`
- `CMakeLists.txt`
- `Makefile`

### 1.3 Build configuration

| Path                              | Description                                                  |
|-----------------------------------|--------------------------------------------------------------|
| `CMakeLists.txt` (root)           | Top-level orchestrator; auto-discovers example apps.         |
| `Makefile` (root)                 | Shorthand wrapper invoking CMake per app.                    |
| `cmake/toolchain-ra8d2.cmake`     | arm-none-eabi cross-compile settings.                        |
| `cmake/ra8_warnings.cmake`         | Warning + stack-usage gate (`-Wstack-usage=2048` default).   |
| `Doxyfile`                        | Doxygen configuration (docs site + warning gate).            |

### 1.4 Test configuration items

| Path                              | Description                                                  |
|-----------------------------------|--------------------------------------------------------------|
| `tests/CMakeLists.txt`            | Host test build configuration; gates MC/DC instrumentation.  |
| `tests/build_tests.sh`            | Host-test build entry point.                                 |
| `tests/run_tests.sh`              | Host-test execution entry point (ctest).                     |
| `tests/test_*.c` (190 files)      | Per-module host unit tests (190/190 PASS).                   |

### 1.5 Verification + audit scripts

| Path                                       | Description                                          |
|--------------------------------------------|------------------------------------------------------|
| `scripts/report/mcdc_report.sh`             | MC/DC measurement and gate.                          |
| `scripts/checks/misra_check_inner.sh`             | MISRA-C 2012 advisory audit.                         |
| `scripts/checks/stack_usage_check.py`       | Stack-bound aggregator.                              |
| `scripts/checks/cite_check.py`              | HUM citation validator.                              |
| `scripts/checks/check_world_tags.py`        | TrustZone world-tag enforcement.                     |
| `scripts/checks/check_obsolete_standards.py`| Rejects superseded safety-standard references.       |
| `scripts/checks/check_no_dynamic_alloc.py`  | NASA P10 Rule 3 enforcement.                         |
| `scripts/checks/check_mcdc_block.py`        | `@par MC/DC:` block enforcement on tests.            |
| `scripts/checks/check-since-version.py`     | Doxygen `@since` enforcement.                        |
| `scripts/checks/check-copyright.py`         | Copyright + SPDX header enforcement.                 |
| `scripts/report/roadmap_stats.py`           | ROADMAP.md summary block freshness gate.             |
| `scripts/checks/coverage.sh`                      | gcovr coverage gate.                                 |
| `scripts/checks/format_code.sh`                   | clang-format wrapper.                                |
| `scripts/checks/clang_tidy.sh`                    | clang-tidy wrapper.                                  |
| `scripts/dev/flash.sh`                         | J-Link flash wrapper (HW operations).                |
| `scripts/git/pre-commit`                   | Pre-commit hook (the authoritative gate suite).      |

### 1.6 Documentation

| Path                              | Description                                                  |
|-----------------------------------|--------------------------------------------------------------|
| `CLAUDE.md`                       | Coding rules; informs both human and AI contributors.        |
| `docs/STYLE_GUIDE.md`             | Authoritative human-facing style guide.                      |
| `docs/RING_AND_WORLD.md`          | Architectural-ring and TrustZone-world tagging system.       |
| `docs/MEMORY_MAP.md`              | Linker memory map.                                           |
| `docs/MCDC.md`, `docs/MCDC_GAPS.md` | MC/DC infrastructure and gap list.                         |
| `docs/MISRA.md`, `docs/MISRA_GAPS.csv` | MISRA-C 2012 audit baseline.                            |
| `docs/STACK_USAGE.md`             | Stack-bound analysis.                                        |
| `docs/HARDWARE_BRINGUP.md`        | EVM bring-up + smoke procedure.                              |
| `docs/QUALIFICATION_ROADMAP.md`   | Phase plan to SIL 3 / DAL B.                                 |
| `docs/qualification/`             | All planning + verification + accomplishment documents.      |
| `docs/SOUP/`                      | Per-component pre-existing-software qualification basis.     |
| `docs/reference/`                 | Renesas datasheets and HUM (committed PDFs).                 |

### 1.7 Vendor SOUP (frozen in tree)

`libs/third_party/` holds the pinned source of every SOUP component
listed in `docs/SOUP/README.md`. Each component is vendored at the
exact version recorded in its `docs/SOUP/<name>.md` file. Updates
require:

1. A bump of `libs/third_party/<name>/` to the new upstream tag.
2. A diff review against the previous vendored version.
3. An update of `docs/SOUP/<name>.md` (version, last-review date,
   any new advisories considered).

### 1.8 CI configuration

| Path                              | Description                                                  |
|-----------------------------------|--------------------------------------------------------------|
| `.github/workflows/firmware.yml`  | Primary CI workflow (build, test, MC/DC, coverage, lint).    |
| `.github/mcdc-baseline.txt`       | Pinned MC/DC baseline (regression gate).                     |
| `.clang-format`                   | Formatter configuration.                                     |
| `.clang-tidy`                     | Linter configuration (NASA P10 Rule 4 thresholds).           |
| `.clangd`                         | Editor integration; strips ARM-only flags.                   |
| `.cppcheck-suppressions`          | MISRA deviation justifications inline.                       |
| `.editorconfig`, `.gitattributes`, `.gitignore` | Repository hygiene.                            |

---

## 2. Configuration management activities

### 2.1 Version-control system

- Tool: **Git**.
- Authoritative remote: GitHub repository `ra8-firmware` under user
  account `bsikar` (origin).
- Working directory: `/Users/bsikar/Documents/github/ra8-firmware`.

### 2.2 Branching model

- **Single long-lived branch**: `main`.
- All feature work occurs on short-lived topic branches that merge
  back into `main` via pull request.
- No release branches today; tags will be added when the first
  release lands (see Section 5).

### 2.3 Commit policy

- Semantic commit prefix per `docs/STYLE_GUIDE.md` (e.g. `tests:`,
  `libs:`, `secure_app+tests:`, `revert:`).
- Commits are small and self-contained; one logical change per
  commit. Recent examples on `main`:
  - `8981092f0 secure_app+tests: properly order key_import enums; wire ra8_psa_crypto`
  - `6cb7f02c7 libs: ra8_modem_at, ra8_power_profile, ra8_sensor_bme280 + unit tests`
  - `10b9eedfc ota: Phase-5 OTA orchestration + secure-side commit veneers`
- **Commit messages contain no AI attribution.** This is a hard
  project rule per `CLAUDE.md`.
- Force-push to `main` is **forbidden**.
- Force-push to topic branches before merge is permitted.

### 2.4 Pre-commit gate (authoritative configuration of "what cannot land")

The hook at `scripts/git/pre-commit` enforces the following gates on
every commit. Failure of any gate refuses the commit:

1. ASCII-only source files (`fix-encoding.py --check`).
2. C23 patterns: no `_Static_assert`, no `= {0}`, no `#include
   <stdbool.h>`.
3. Defensive-paren on numeric `#define` values.
4. `clang-format` (no diff).
5. `clang-tidy` (no warnings).
6. `cppcheck` warning/style/performance/portability (production
   files only).
7. Doxygen `@since` tag enforcement on public headers.
8. Copyright + SPDX header enforcement.
9. HUM citation validator (`cite_check.py --warn`).
10. World-tag validator (`check_world_tags.py --warn`).
11. ROADMAP summary freshness (`roadmap_stats.py --check`, strict).
12. Obsolete-standards reference scan (rejects superseded
    safety-standard references, strict).
13. `@par MC/DC:` block on staged tests
    (`check_mcdc_block.py`).

The hook is not bypassable via `--no-verify` in CI; it is the
developer's responsibility not to bypass locally either.

### 2.5 CI gate (authoritative configuration of "what cannot merge")

`.github/workflows/firmware.yml` mirrors the pre-commit suite and
adds the heavier gates that are too slow for per-commit:

1. ASCII, copyright, `@since` tag scanners (mirrors of pre-commit).
2. clang-format, clang-tidy.
3. Host unit tests (`unit-tests` job).
4. Coverage gate (gcovr 90/90, `coverage` job).
5. MC/DC coverage gate against `.github/mcdc-baseline.txt`
   (`mcdc` job).
6. `pre-commit-checks` job (full mirror of the pre-commit hook).
7. Cross-build matrix over every example app (`build-cross` job).
8. Doxygen warning gate (`docs` job).
9. cppcheck full-tree run (`cppcheck` job).
10. PR-only MC/DC delta comment (`coverage-comment` job).

A PR cannot be merged with any CI gate red. This is the binding
configuration-control point.

---

## 3. Problem reporting

### 3.1 Reporting channel

GitHub Issues on the `ra8-firmware` repository are the
authoritative problem-report log. There is no separate bug tracker.

### 3.2 Severity classification

Per `docs/QUALIFICATION_ROADMAP.md` planning vocabulary, the project
uses the following severity tiers:

| Severity | Meaning                                                                |
|----------|------------------------------------------------------------------------|
| `crit`   | Defect that violates a SIL 3 / Level B safety claim or breaks `main`.  |
| `high`   | Defect on the critical-path module set (ISR, MPU, XSPI, USB, SCI, PSA).|
| `med`    | Defect in any first-party module not on the critical path.             |
| `low`    | Cosmetic, documentation, or style defect.                              |

### 3.3 Linkage convention

- Every PR that closes an issue uses `Fixes #N` in the PR body.
- Every commit that addresses an issue references it as `(#N)` in
  the subject line.
- A defect that surfaces a missing test must be paired with a new
  `tests/test_*.c` entry in the same PR.

### 3.4 Audit trail

The combined `git log` + GitHub Issues + PR review history form the
complete defect-resolution audit trail. No additional tracker is
required for the qualification claim.

---

## 4. Change control

### 4.1 Inbound change procedure

- Every change to `main` arrives via a pull request.
- The pre-commit hook enforces the per-commit gates; CI enforces the
  per-PR gates (Section 2.5).
- A PR may not be merged with any CI gate red.
- The `CLAUDE.md` zero-backward-compatibility policy means that
  breaking changes are explicit: API renames, deletions, and type
  changes update every call site in the same commit. There is no
  deprecation window.

### 4.2 Baselining

A "baseline" in this project is a git commit hash on `main`.
External references (the SVR, the SAS, the SOUP review records) cite
commits by their abbreviated SHA. Until the first signed tag lands
(Section 5), the latest commit on `main` is the working baseline.

### 4.3 Configuration of build environment

- The cross-compiler is pinned to **Arm GNU Toolchain 13.3.rel1**
  (gcc 13.3.1) and enforced by `cmake/toolchain-ra8d2.cmake`
  (a `-dumpfullversion` `13.3` assertion, FATAL by default) -- not
  the Ubuntu apt `gcc-arm-none-eabi`. The host verification tools
  (`clang-18`/`llvm-18`, `cppcheck`, `clang-format-22`, `clang-tidy`,
  `doxygen`, `graphviz`) run from the pinned devcontainer image.
- The devcontainer image (`.devcontainer/Dockerfile`) is checked in
  and pins the base image by digest plus exact tool versions, so a
  developer-local `make ci` reproduces CI. CI remains the
  authoritative environment. See `docs/TOOLCHAIN.md`.

### 4.4 SOUP change control

Any update to a vendored library under `libs/third_party/` requires:

1. An updated `docs/SOUP/<name>.md` (version, last-review date,
   reviewed advisories).
2. A diff review against the previous vendored version.
3. PR-level approval; gate as for any other change.

---

## 5. Configuration status accounting

### 5.1 Status records

- `git log` is the authoritative change history.
- `git tag` is the authoritative release register. **No release tags
  exist today.** The first signed tag will be cut at the end of
  roadmap Phase 7.
- CI run history (GitHub Actions) is the authoritative
  build-and-verification record.

### 5.2 Release tagging procedure (for future use)

When a release is cut, the procedure is:

1. Update `CHANGELOG.md` (when introduced) with the release scope.
2. Run `make mcdc`, `make misra`, `make stack-usage`,
   `make test`, `make smoke` and archive the reports under
   `docs/qualification/release/<tag>/`.
3. Tag the release: `git tag -s vX.Y.Z -m "Release X.Y.Z"`.
4. Push the tag: `git push origin vX.Y.Z`.
5. Update `docs/qualification/SAS.md` to cite the new tag.

The signing key fingerprint and key-management procedure will be
documented at the time of the first signed release.

### 5.3 Reporting cadence

- Per-PR: CI logs (automatic, persisted by GitHub).
- Per quarter: re-run `make misra`, refresh `docs/MISRA_GAPS.csv`,
  re-stamp `docs/MISRA.md` audit table.
- Per quarter: re-run `make mcdc`, refresh `docs/MCDC_GAPS.md`,
  re-stamp `docs/MCDC.md` measurement-history table.
- Per release: full audit-pack regeneration (Section 5.2).

---

## 6. Archive, retrieval, and release

### 6.1 Archive

- Primary record: this git repository (the GitHub remote is the
  authoritative copy).
- Vendored SOUP source: `libs/third_party/` is checked into the same
  repository so that a single `git clone` retrieves the full build
  configuration. There is no external dependency fetch at build
  time.
- CI artifacts: `coverage-html` and `mcdc-report` archives uploaded
  per CI run with **14-day retention** (per `firmware.yml`). For
  qualification claims, CI artifacts will be re-archived under
  `docs/qualification/release/<tag>/` at release time.
- Reference manuals: Renesas datasheets and HUM committed under
  `docs/reference/` so they are always retrievable from the same
  archive.

### 6.2 Retrieval

A bit-exact rebuild of any historical state is achieved by:

1. `git clone <remote>` the repository.
2. `git checkout <commit-or-tag>`.
3. Reproduce the CI environment via the apt package versions pinned
   in `firmware.yml`.
4. `make <app>` to cross-build, or `make test` to host-test.

### 6.3 Retention

- `git log` retained indefinitely (this is the primary record).
- CI logs retained per GitHub Actions defaults (90 days for logs;
  14 days for the explicitly-configured artifact uploads).
- Long-term audit-pack retention is via the `docs/qualification/
  release/<tag>/` snapshot taken at release time.

---

## 7. Software load control

### 7.1 Build outputs

- The cross-build emits per-app ELF, HEX, and BIN files under
  `build/<app>/`.
- The hex file is the authoritative production binary.
- The build records the toolchain version (printed by the CI step
  `Print toolchain version`) and the source SHA in the build log.

### 7.2 Flash procedure

- Tool: `JLinkExe` invoked via `scripts/dev/flash.sh <hex-path>`.
- Probe: on-board J-Link OB SN 1086567198 on the EK-RA8D2.
- Verification: the J-Link `verify` command is the post-flash
  readback. A flash run that fails verify is treated as a load
  failure.

### 7.3 Hash recording

- For each release-tagged build, the hex file SHA-256 is recorded in
  the release notes and in `docs/qualification/release/<tag>/
  hashes.txt`.
- For PR builds, the hex hash is captured in the CI log of the
  `build-cross` job. CI log retention bounds the audit window.

### 7.4 Load control on production hardware

There is no production fleet today; all loads are developer-bench
loads against the EK-RA8D2 evaluation kit. When a production fleet
is established the load-control procedure will be extended with a
signed-update path (the OTA orchestration in `libs/ra8_ota/` is the
foundation).

---

## 8. Software life cycle environment control

### 8.1 Toolchain pinning

The CI workflow `.github/workflows/firmware.yml` pins the toolchain
by apt-package name on Ubuntu 24.04. The pinned set is:

- `gcc-arm-none-eabi`, `libnewlib-arm-none-eabi` (cross compiler).
- `clang-18`, `llvm-18`, `libclang-rt-18-dev` (host MC/DC chain).
- `clang-format`, `clang-tidy` (style + lint).
- `cppcheck` (static + MISRA).
- `cmake`, `ninja-build`, `gcc` (build orchestration).
- `doxygen`, `graphviz` (docs gate).
- `gcovr` (coverage gate, installed via pip).

Any toolchain bump is a `firmware.yml` edit and follows the same
PR + CI gating as a code change.

### 8.2 Devcontainer (planned)

A devcontainer that pins the same toolchain set is on the roadmap;
it is not yet checked in. Until then, developer-local builds may
drift; CI is authoritative.

### 8.3 Operating-system + runner control

- CI runner: `ubuntu-latest` (currently 24.04). Bumps are reviewed
  on the GitHub Actions release notes and accepted by ratcheting
  `firmware.yml`.
- HW-in-the-loop runner: developer-laptop pre-push only
  (`docs/HIL_DEVELOPER_WORKFLOW.md`); a self-hosted runner is **out
  of scope** per `docs/CERTIFICATION_SCOPE.md`.

---

## 9. References

- `CLAUDE.md` -- coding standard, character-encoding policy,
  no-AI-attribution policy, zero-backward-compatibility policy.
- `docs/QUALIFICATION_ROADMAP.md` -- phase plan and gap analysis.
- `docs/SOUP/` -- pre-existing software register.
- `scripts/git/pre-commit` -- authoritative pre-commit gate suite.
- `.github/workflows/firmware.yml` -- authoritative CI gate suite.
- IEC 61508-3:2010 Clause 6.2.3.
- RTCA DO-178C:2011 Sections 7 and 11.4.
- ISO 26262-8:2018 Clause 7.
