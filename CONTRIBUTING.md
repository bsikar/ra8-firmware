# Contributing to ra8-firmware

Welcome. This is a personal exploration of the Renesas RA8D2 MCU
(EK-RA8D2 evaluation kit) targeting the same safety-integrity bar that
shipping safety-critical embedded code is held to: **IEC 61508 SIL 3**
(industry-agnostic) and the matching **DO-178C Level B** /
**ISO 26262 ASIL C** derivatives. The repository is FOSS under the MIT
licence (see `LICENSE.txt`) and is developed entirely from the command
line -- no vendor IDE artifacts, no e2 studio.

The authoritative style guide is [`docs/STYLE_GUIDE.md`](docs/STYLE_GUIDE.md)
and the AI-assistant-facing rule restatement is `CLAUDE.md` at the
repository root. Read both before contributing non-trivial code.

## 1. Project goal

* Hand-written bare-metal firmware targeting the Renesas RA8D2
  (Cortex-M85 @ 1 GHz primary core, Cortex-M33 @ 250 MHz secondary, 1 MB
  MRAM, 2 MB ECC SRAM).
* Build the entire HAL and PAL surface area without copying Renesas FSP
  code. FSP headers are reference material only; every line in `src/`,
  `libs/`, and `examples/` is hand-written under this project's style
  rules.
* Every new feature ships with host-side unit tests and (for any
  compound boolean decision) MC/DC vectors. See
  [`docs/MCDC.md`](docs/MCDC.md) and the running gap report
  [`docs/MCDC_GAPS.md`](docs/MCDC_GAPS.md).
* Long-term the firmware tree should be qualifiable end-to-end, even
  though there are no plans to ship it into a regulated product.

## 2. Hardware target

* Board: **Renesas EK-RA8D2** (part number 968-K7EKA8D2S01001BE).
* MCU: R7KA8D2KFLCAC (RA8D2 group, 289-pin BGA).
* On-board debug probe: SEGGER J-Link OB (SWD) -- no separate probe
  needed; a single USB cable is enough to flash and debug.
* Other on-board peripherals exercised by the example apps: 7.0-inch
  1024x600 parallel TFT, OV5640 5 MP camera, 64 MB Octo-SPI flash,
  64 MB SDRAM.
* The committed reference manuals live under `docs/reference/`. The
  Hardware User's Manual (`r01uh1065ej0130-ra8d2.pdf`) is the primary
  citation for any register-level change.

## 3. First-time setup

```bash
git clone https://github.com/<your-fork>/ra8-firmware.git
cd ra8-firmware
git config core.hooksPath scripts/git
```

The hook configuration above wires `scripts/git/pre-commit` so every
commit you author is gated locally with the same checks CI runs.

You need two toolchains:

* **Host clang/gcc** for the unit-test build. The supported path is the
  project's Ubuntu 24.04 dev container, invoked through
  [`scripts/test-docker.sh`](scripts/test-docker.sh). On macOS you
  need [Colima](https://github.com/abiosoft/colima) (or Docker Desktop)
  running because the host test simulator uses `mmap(MAP_FIXED, ...)`
  at MCU peripheral addresses, which macOS arm64 refuses below 4 GiB.
* **arm-none-eabi-gcc** (ARM GNU Toolchain) for the cross-compiled
  firmware build. Any reasonably recent version that knows about
  `cortex-m85` works; the project is verified with the toolchain
  shipped in the dev container.

CMake and clang-tidy/clang-format are pulled into the dev container
automatically, so the easiest workflow is "make changes, then
`bash scripts/test-docker.sh`".

## 4. Daily workflow

```text
edit -> bash scripts/format_code.sh -> bash scripts/test-docker.sh -> git commit
```

* `scripts/format_code.sh` runs clang-format over every C/H file the
  project owns. Always run it before committing -- the pre-commit hook
  rejects the commit otherwise.
* `scripts/test-docker.sh` runs the full host-side test suite inside
  the project's pinned Ubuntu 24.04 image. This is the only sanctioned
  way to run tests on macOS hosts.
* `git commit` triggers `scripts/git/pre-commit` (formatting,
  clang-tidy, ASCII check, doxygen audit, citation check, world-tag
  check, MC/DC block check, ...). Do **not** bypass with `--no-verify`
  -- if a hook fails, fix the underlying issue and re-stage.

For cross-compiled firmware iteration:

```bash
make blink                       # build one example app for the target
make apps                        # list every discovered example app
make -C <app-dir> flash          # flash via the on-board J-Link OB
make -C <app-dir> debug          # launch GDB attached to the running target
```

## 5. Adding an application

The full procedure lives in `CLAUDE.md` under "Adding a new application".
The short version:

1. Create `examples/<tier>/.../<newapp>/` -- pick the tier that matches
   the hardware-support category (`ek_ra8d2/hw_validated/hil/`,
   `ek_ra8d2/hw_validated/hil/`, `ek_ra8d2/hw_validated/manual/`,
   `ek_ra8d2/hw_pending/`, or `_unsupported/`). Every app is fully
   self-contained -- its own boot files, linker script, CMakeLists.
2. Copy the five per-app boot files from a sibling app
   (`vector_table.c`, `system_init.c`, `secure_exception.c`,
   `trustzone_init.{c,h}`).
3. Copy `linker_script.ld`, `CMakeLists.txt`, and `Makefile`. Update
   `RA8_APP_NAME` / `APP` to the new app name.
4. Add a host-side integration test under `tests/test_app_<newapp>.c`
   exercising any new logic (see `tests/test_app_blink_hal.c` for the
   minimal shape).
5. Re-run `make` from the repo root -- the top-level CMake
   auto-discovers the new directory; no edit to the root `CMakeLists.txt`
   is required.

## 6. Adding a test

* Place new host tests under `tests/test_*.c` (compiled with the
  pinned Unity-minimal harness shipped in `tests/mocks/`).
* **Every compound boolean decision** added to first-party code under
  `libs/`, `src/`, or `port/` must have a paired test that demonstrates
  MC/DC. Document the vector pattern in a `@par MC/DC:` block on the
  test function. Example:

  ```c
  /**
   * @test ra8_isr_register_validates_inputs
   *
   * @par MC/DC:
   * Decision: if (handler == NULL || priority > k_ra8_isr_prio_max)
   *  - V1: handler=valid, priority=0   -> false
   *  - V2: handler=NULL,  priority=0   -> true (varies handler)
   *  - V3: handler=valid, priority=255 -> true (varies priority)
   */
  ```

* See [`docs/MCDC.md`](docs/MCDC.md) for the coverage measurement workflow
  (`make mcdc`) and the running gap list at
  [`docs/MCDC_GAPS.md`](docs/MCDC_GAPS.md).
* Do not introduce dynamic allocation in test scaffolding -- the same
  NASA Power-of-10 Rule 3 budget applies.

## 7. Pre-commit gate inventory

`scripts/git/pre-commit` runs the following gates on every commit. Each
has its own policy document; click through before disagreeing with a
finding:

* clang-format (`scripts/format_code.sh`).
* clang-tidy (`scripts/clang_tidy.sh`) -- enforces NASA Power-of-10
  Rule 4 (function length), naming conventions, magic-number bans, and
  the C23 typed-enum requirement. See [`docs/STATIC_ANALYSIS.md`](docs/STATIC_ANALYSIS.md).
* ASCII character check -- non-ASCII bytes in source files are
  rejected (`scripts/utils/fix-encoding.py --check`).
* Doxygen audit (`scripts/utils/doxy_audit.py`) -- every function in
  `libs/`, `src/`, `port/` must carry the full required tag set
  documented in `CLAUDE.md`.
* Citation check (`scripts/utils/cite_check.py`) -- register-level
  changes must cite the Hardware User's Manual section. See
  [`docs/CITATION_POLICY.md`](docs/CITATION_POLICY.md).
* Ring + World tag check (`scripts/utils/check_world_tags.py`) --
  every public header must declare its architectural ring and
  TrustZone world. See [`docs/RING_AND_WORLD.md`](docs/RING_AND_WORLD.md).
* Roadmap stats refresh (`scripts/utils/roadmap_stats.py`).
* Obsolete-standards check
  (`scripts/utils/check_obsolete_standards.py`) -- references to
  superseded safety standards are rejected; use the current
  DO-178C / IEC 61508 / ISO 26262 spelling instead.
* MC/DC block check -- new compound decisions need the matching test
  block.
* Stack-usage soft warning -- any function over 2 KB or with dynamic
  stack use is reported (currently warning-only for third-party SOUP).
* AI-attribution ban (`scripts/utils/check_no_ai_attribution.py`) --
  rejects `Co-Authored-By: Claude`, "Generated with Claude Code", and <!-- AI-OK: quoting the banned footer -->
  similar footers anywhere in the tree. Re-run independently in CI by
  `.github/workflows/no-ai-attribution.yml`. See
  [`docs/AI_ATTRIBUTION_POLICY.md`](docs/AI_ATTRIBUTION_POLICY.md).

## 8. PR conventions

* **Conventional commit subjects.** Examples: `fuzz: add ra8_tls
  harness`, `hal: ra8_gpt sets PMR before PFS`, `tests: cover the
  ra8_net_arp duplicate-IP path`. Keep the subject under 70 characters.
* **No AI attribution.** Do not add `Co-Authored-By: Claude`, <!-- AI-OK: quoting the banned footer -->
  "Generated with Claude Code", or similar footers. Treat every commit <!-- AI-OK: quoting the banned footer -->
  as if a human wrote it. This is a hard rule. The pre-commit gate
  `scripts/utils/check_no_ai_attribution.py` extends the rule to in-tree
  files; see `docs/AI_ATTRIBUTION_POLICY.md`.
* **No destructive git ops without an explicit ask.** Never push
  `--force` to `main`, never run `git reset --hard` on someone else's
  branch, never use `--no-verify` to skip the pre-commit hook.
* **One logical change per commit.** Fuzz harnesses, documentation
  updates, and HAL refactors should land as separate commits even when
  authored in the same session.
* **Run the full test suite locally** (`bash scripts/test-docker.sh`)
  before opening a PR. CI runs the same image, so a green local run is
  a strong signal.
* **No backward-compatibility shims.** Update every call site in the
  same commit; do not leave deprecated aliases or wrappers.

## 9. Filing an issue

Issues are the project's long-term memory. Write every issue so a
reader who is not you -- or you, six months from now -- can reproduce
the problem from scratch without guessing. An issue that says "TX is
broken, I used tcpdump" is hearsay; an issue that pastes the exact
commands and their raw output is evidence. Assume the bench will not
be in the same state later and the person debugging will not remember
how it was done.

### Always include

* **Firmware identity.** The exact commit hash (`git rev-parse HEAD`)
  and branch, which example app, the build command, and the resulting
  artifact path. "Latest main" is not an identity -- commits move.
* **Reproduction steps.** Every command, verbatim and in order, so
  they can be copy-pasted. No paraphrasing.
* **Raw output.** Paste the actual console output in fenced code
  blocks -- full dumps, not a summary. Length is fine; truncation is
  not.
* **Expected vs. actual.**
* **What is already ruled out**, and the evidence behind each ruling.

### Extra for hardware / HIL / register-level bugs

* **Bench topology.** The board, the debug probe (J-Link serial
  number), the host driving it, and any network wiring -- interface
  name, IP addresses, MAC addresses. Mark which details are specific
  to your bench versus general to any EK-RA8D2.
* **Register evidence.** When you read MCU registers, give the J-Link
  commander script verbatim, the absolute addresses with their HUM
  chapter/section, and the register semantics that matter (for
  example many RMAC statistics counters are clear-on-read, so a
  baseline read followed by a post-event read yields the exact
  delta). Paste both reads.
* **Bench scripts.** The exact `scripts/hil_*.sh` invocations used.

### Rule out environment error first (required)

A bug report is only credible once you have proven the toolchain, the
build, and -- for a hardware bug -- the bench are themselves sound.
Otherwise the "bug" may be your setup. **Before filing, run the full
verification baseline and paste its results into the issue:**

* **Local unit-test suite** -- `make test`. Expect `100% tests passed`.
* **CI gate suite** -- the jobs in `.github/workflows/firmware.yml`,
  runnable locally: cross-build every app
  (`bash scripts/build_all_examples.sh`), clang-tidy
  (`bash scripts/clang_tidy.sh --check`), clang-format
  (`bash scripts/format_code.sh --check`), and the citation / ASCII
  checks under `scripts/utils/`.
* **HIL suite** (hardware bugs) -- `bash scripts/hil_all.sh`, which
  flashes and verifies every app under
  `examples/ek_ra8d2/hw_validated/hil/` on the board. A green HIL run
  proves the J-Link, the flash path, the UART/USB plumbing, and the
  bench wiring all work -- so a remaining failure is the firmware, not
  the rig.

If every baseline step is green and the bug still reproduces, it is a
genuine defect: state that explicitly in the issue. If a baseline step
is red, fix it first or explain in the issue why it is unrelated.

### Template

```markdown
## Summary
One paragraph: what is wrong and how it manifests.

## Affected firmware
- Commit / branch: <hash> (<branch>)
- App: examples/.../<app>
- Build: <command>
- Artifact: <path to .hex/.elf>

## Bench setup
Board / debug probe / host / wiring, with every ID spelled out.

## Reproduction
Numbered, copy-pasteable commands.

## Observed data
Raw output, in full, in code blocks.

## Analysis
What the data proves; what is ruled out and why.

## Next step
```

GitHub issue #1 ("Ethernet: large-frame TX corrupted post-MAC") is
the worked example -- match that level of detail.

## 10. Where to ask questions

This is a personal project, not an open community, but issues and
discussions on the GitHub repository are welcome. For deeper context
on the architectural ring + TrustZone world tagging system see
[`docs/RING_AND_WORLD.md`](docs/RING_AND_WORLD.md); for the qualification
roadmap see [`docs/QUALIFICATION_ROADMAP.md`](docs/QUALIFICATION_ROADMAP.md);
for the running list of vendored third-party blobs (mbedTLS, ThreadX,
NetX Duo, FileX, USBX, NimBLE) see
[`docs/SOUP/`](docs/SOUP/).
