# Contributing to ra8-firmware

Welcome. This is a personal exploration of the Renesas RA8D2 MCU
(EK-RA8D2 evaluation kit) targeting the same safety-integrity bar that
shipping safety-critical embedded code is held to: **IEC 61508 SIL 3**
(industry-agnostic) and the matching **DO-178C Level B** /
**ISO 26262 ASIL C** derivatives. The repository is FOSS under the MIT
licence (see `LICENSE.txt`) and is developed entirely from the command
line -- no vendor IDE artifacts, no e2 studio.

The authoritative style guide is [`docs/STYLE_GUIDE.md`](docs/STYLE_GUIDE.md),
and the repository-wide working rules live in `CLAUDE.md` at the repository
root. `AGENTS.md` is the compatibility entry point to those same rules. Read
the style guide and `CLAUDE.md` before contributing non-trivial code. Editor
setup is documented in [`docs/IDE.md`](docs/IDE.md).

## 1. Project goal

* Hand-written bare-metal firmware targeting the Renesas RA8D2
  (Cortex-M85 @ 1 GHz primary core, Cortex-M33 @ 250 MHz secondary, 1 MB
  MRAM, 1.6 MB ECC SRAM).
* Build the entire HAL and PAL surface area without copying Renesas FSP
  code. FSP headers are reference material only; every first-party line in
  `libs/` and `examples/`, excluding declared `third_party/` SOUP roots, is
  hand-written under this project's style
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
  [Hardware User's Manual](docs/reference/ra8d2-hardware-user-manual.pdf) is the primary
  citation for any register-level change.

## 3. First-time setup

```bash
git clone https://github.com/<your-fork>/ra8-firmware.git
cd ra8-firmware
just setup
```

The only bootstrap prerequisites are Git, `just`, a stdlib Python 3.11 through
3.14, trusted CA certificates/network access for the first fetch, and a
working container runtime (`podman`, `docker`, or `nerdctl`; macOS Docker uses
Colima). `just setup` creates the ignored repository-local `.venv`, installs
the exact Python lock and Galaxy collections, configures the tracked hooks,
and refreshes the pinned `ra8-ci` image. It never installs into the system
Python. Re-run `just setup-python` when only Python or hooks need refreshing.
Windows development uses WSL2. See `docs/PYTHON_ENVIRONMENTS.md` for lock
maintenance and managed environment ownership.

You need two toolchains:

* **Host clang/gcc** for the unit-test build. The supported macOS path is the
  project's Linux dev container, invoked through `just ci` or the focused
  `just quality::devcontainer::*` recipes. On macOS you need Colima, Docker
  Desktop, or another compatible container runtime running because the host
  test fake uses `mmap(MAP_FIXED, ...)`
  at MCU peripheral addresses, which macOS arm64 refuses below 4 GiB.
* **arm-none-eabi-gcc** (ARM GNU Toolchain) for the cross-compiled
  firmware build. Use the repository-pinned 13.3.rel1 toolchain shipped in
  the dev container; compiler output is version-sensitive and other releases
  are not supported verification environments.

CMake, the C/C++ compilers, the Arm cross-compiler, and non-Python analysis
tools are provided by the dev container. Run
`just quality::devcontainer` to see the focused commands.

## 4. Daily workflow

```text
edit -> just quality::devcontainer::format -> just checks::devcontainer -> git commit
```

* `just quality::devcontainer::format` applies the pinned formatter to every
  first-party C/H file. The pre-commit hook rejects formatting drift.
* `just checks::devcontainer` runs the focused format, tidy, and unit-test
  checks inside the pinned image. Before pushing, run the full `just ci` suite.
* `git commit` triggers `scripts/git/pre-commit` (formatting,
  clang-tidy, ASCII check, doxygen audit, citation check, world-tag
  check, MC/DC block check, ...). Do **not** bypass with `--no-verify`
  -- if a hook fails, fix the underlying issue and re-stage.

For cross-compiled firmware iteration:

```bash
just apps::build blink                 # build one firmware example
just apps::example::list               # list every discovered firmware example
just apps::hardware::flash blink       # flash a board attached to this host
just apps::hardware::debug blink       # launch GDB on a locally attached board
```

## 5. Adding an application

The full procedure lives in `CLAUDE.md` under "Adding a new application".
The short version:

1. Create `examples/ek_ra8d2/<tier>/.../<newapp>/` -- pick the tier that matches
   the hardware-support category, from validated-on-silicon down to
   needs-hardware-nobody-has.
2. Give it an
   `examples/ek_ra8d2/<tier>/.../<newapp>/src/main.c` and a root
   `CMakeLists.txt` that calls
   `ra8_add_app()`. That is usually the whole app: the board layer under
   `libs/ra8_board_<board>/` supplies the vector table, `SystemInit`,
   the exception handlers and the linker script.
3. Only if the app must diverge from the board defaults, drop a
   same-named implementation under the app's `src/` directory. Put any
   app-local header under `inc/`. See `docs/ARCHITECTURE.md`.
4. Add a host-side integration test under the appropriate `tests/<unit>/src/`
   directory, exercising any new logic (see `test_app_blink_hal.c` for the
   minimal shape).
5. Run `just apps::build <newapp>` from the repository root. Discovery is
   automatic; no edit to the root `CMakeLists.txt` is required.

## 6. Adding a test

* Place new host tests under the relevant `tests/<unit>/src/` directory (compiled with the
  pinned Unity-minimal harness shipped in `tests/mocks/`).
* **Every compound boolean decision** added to first-party code under
  `libs/` or `port/` must have a paired test that demonstrates
  MC/DC. Document the vector pattern in a `@par MC/DC:` block on the
  test function. Example:

  ```c
  /**
   * @test ra8_isr_register_validates_inputs
   *
   * @par MC/DC:
   * Decision: if (handler == nullptr || priority > k_ra8_isr_prio_max)
   *  - V1: handler=valid, priority=0   -> false
   *  - V2: handler=nullptr, priority=0  -> true (varies handler)
   *  - V3: handler=valid, priority=255 -> true (varies priority)
   */
  ```

* See [`docs/MCDC.md`](docs/MCDC.md) for the coverage measurement workflow
  (`just quality::local::mcdc`) and the running gap list at
  [`docs/MCDC_GAPS.md`](docs/MCDC_GAPS.md).
* Do not introduce dynamic allocation in test scaffolding -- the same
  NASA Power-of-10 Rule 3 budget applies.

## 7. What the pre-commit gate checks

`scripts/git/pre-commit` is the authority -- it names each gate as it
runs it. They fall into a few families, and each family has a policy
document; click through before disagreeing with a finding.

* **Shape** -- clang-format, and clang-tidy for NASA Power-of-10 Rule 4
  (function length), naming, the magic-number ban and the C23
  typed-enum requirement. See
  [`docs/STATIC_ANALYSIS.md`](docs/STATIC_ANALYSIS.md).
* **Encoding** -- non-ASCII bytes in source files are rejected outright.
* **Documentation** -- every function in `libs/` and `port/` must carry
  the full Doxygen tag set documented in `CLAUDE.md`, on a block that
  actually describes the symbol it is attached to.
* **Citations** -- a register-level change must cite the Hardware
  User's Manual, and must not cite an in-tree file by line number. See
  [`docs/CITATION_POLICY.md`](docs/CITATION_POLICY.md).
* **Architecture tags** -- every public header declares its
  architectural ring and TrustZone world. See
  [`docs/RING_AND_WORLD.md`](docs/RING_AND_WORLD.md).
* **Safety** -- a new compound boolean decision needs its MC/DC block
  ([`docs/MCDC.md`](docs/MCDC.md)), and stack frames are bounded
  ([`docs/STACK_USAGE.md`](docs/STACK_USAGE.md); soft findings in
  third-party SOUP are reported, not fatal).
* **Provenance** -- the AI-attribution ban, re-run independently in CI.
  See [`docs/AI_ATTRIBUTION_POLICY.md`](docs/AI_ATTRIBUTION_POLICY.md).

## 8. PR conventions

* **Conventional commit subjects.** Examples: `fuzz: add ra8_tls
  harness`, `hal: ra8_gpt sets PMR before PFS`, `tests: cover the
  ra8_net_arp duplicate-IP path`. Keep the subject under 70 characters.
* **No AI attribution.** Do not add `Co-Authored-By: Claude`, <!-- AI-OK: quoting the banned footer -->
  "Generated with Claude Code", or similar footers. Treat every commit <!-- AI-OK: quoting the banned footer -->
  as if a human wrote it. This is a hard rule. The pre-commit gate
  `scripts/checks/check_no_ai_attribution.py` extends the rule to in-tree
  files; see `docs/AI_ATTRIBUTION_POLICY.md`.
* **No destructive git ops without an explicit ask.** Never push
  `--force` to `main`, never run `git reset --hard` on someone else's
  branch, never use `--no-verify` to skip the pre-commit hook.
* **One logical change per commit.** Fuzz harnesses, documentation
  updates, and HAL refactors should land as separate commits even when
  authored in the same session.
* **Run the full gate suite locally** (`just ci`) before opening a PR. CI uses
  the same gate registry and pinned environment.
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
* **Bench scripts.** The exact `scripts/hil/*.sh` invocations used.

### Rule out environment error first (required)

A bug report is only credible once you have proven the toolchain, the
build, and -- for a hardware bug -- the bench are themselves sound.
Otherwise the "bug" may be your setup. **Before filing, run the full
verification baseline and paste its results into the issue:**

* **Local unit-test suite** -- `just quality::local::test`. Expect `100% tests passed`.
* **CI gate suite** -- `just ci`, the same complete gate registry that the
  workflows invoke. Do not reproduce the registry with a hand-written list.
* **HIL suite** (hardware bugs) -- `just hil::run`, which builds locally,
  stages the artifacts, and verifies every app under
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
for the vendored third-party components and their qualification
justifications see [`docs/SOUP/`](docs/SOUP/).
