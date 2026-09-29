# ADR-0011: Chip-level boot code duplicated across the two board libraries

## Status

Proposed -- 2026-09-17. The decision below is the repository
owner's to confirm; this ADR records the options, the constraint
that narrows them, and the consequences of each. Nothing is
adopted by merging it.

## Context

Issue #696 ("Platform-arch (d): board-description + arch/chip/board
build-selection model") proposes an `RA8_ARCH` / `RA8_CHIP` /
`RA8_BOARD` selection triple, a single toolchain dispatcher under `cmake/`,
a board-interface header with a gate behind it, and a split of
`libs/ra8_core/inc/ra8_device.h` so that external-memory geometry
moves to the board half. The issue is marked DESIGN / PLANNING
ONLY and is sequenced behind the filesystem work in #611.

Three of the issue's premises have moved since it was filed, and a
fourth condition it did not anticipate is now the load-bearing one.
All statements below were re-derived against `dev` at `1ae56f4`.

### The selection surface is smaller than the issue assumes

* The issue asks for a sweep of "the 227 Makefiles". The tree
  tracks **13** files named `Makefile`, and **none** of them names
  `cmake/toolchain-ra8d2.cmake` or `cmake/toolchain-ra8p1.cmake`.
  Selection runs through `CMakePresets.json` (lines 10 and 22) and
  the top-level `CMakeLists.txt` (lines 50, 80, 160). The Makefile
  sweep is not work that exists.
* No `RA8_ARCH`, `RA8_CHIP` or `RA8_BOARD` cache variable exists
  anywhere in the tree. The only device switch today is the
  preprocessor define `RA8_DEVICE_RA8P1`, consumed by
  `libs/ra8_core/inc/ra8_device.h`.
* Exactly two toolchain files exist. Their filenames are named
  from 20 sites, so a dispatcher that replaces them is a
  20-site edit, not a 2-file edit: the top-level `CMakeLists.txt`
  (50, 80, 160), `cmake/ccache.cmake` (24),
  `cmake/ra8_add_app.cmake` (237), `cmake/ra8_app/sources.cmake`
  (557), `cmake/shared_libs/CMakeLists.txt` (19),
  `scripts/builders/all_examples.sh` (280, 302),
  `scripts/builders/build_cross_compile_db.py` (306),
  `scripts/checks/check_freestanding_runtime.py` (717-721),
  `scripts/checks/check_nsc_cmse.sh` (68),
  `scripts/checks/tidy/passes.sh` (206),
  `scripts/ci/gates/build.sh` (243), `scripts/ci/unicorn_pin.sh`
  (31), `scripts/dev/ra8_apps.py` (106, 138), plus
  `docs/TOOLCHAIN.md` and the qualification set (PSAC, SCMP, SDP,
  SRS, SVP, TOOL_QUALIFICATION) and
  `docs/reference/ra8p1_vs_ra8d2.md`.

### The board layer the issue proposes already exists, undeclared

Two board libraries are already in the tree, each with its own
linker script and its own boot-source directory:
`libs/ra8_board_ek_ra8d2` and `libs/ra8_board_ra8p1`. What is
missing is not the layer but its contract: there is no
board-interface header and no gate that holds one board library to
the shape of the other.

`libs/ra8_core/inc/ra8_device.h` is referenced by 27 tracked
files, including every RA8P1 foundation example, the whole of
`libs/ra8_board_ra8p1`, the NPU and Ethos-U surface in
`libs/ra8_hal`, and `tests/cmake/unit_tests.cmake`. Whether the
memory-map enumerations *specifically* have consumers is the
question asked by #1048, which is not this issue's and is not
settled here.

### The load-bearing condition: the copies have already drifted

Both board libraries carry the same five chip-boot translation
units. Line counts, `libs/ra8_board_ek_ra8d2/src/boot/` against
`libs/ra8_board_ra8p1/src/boot/`:

| Translation unit        | EK-RA8D2 | RA8P1 | Diff lines |
|-------------------------|----------|-------|------------|
| `nmi_exception.c`       | 116      | 122   | 22         |
| `secure_exception.c`    | 79       | 84    | 7          |
| `system_init.c`         | 566      | 552   | 60         |
| `trustzone_init.c`      | 207      | 212   | 7          |
| `vector_table.c`        | 633      | 615   | 62         |

Three of the five RA8P1 copies open with a note reading "this
chip-boot TU is byte-identical to the EK-RA8D2 copy", attributed to
issue #226. For three of the five files that claim is false today:

* `libs/ra8_board_ra8p1/src/boot/secure_exception.c` and
  `libs/ra8_board_ra8p1/src/boot/trustzone_init.c` are byte-identical
  to their EK-RA8D2 counterparts apart from the `@file` path and
  the note itself. The claim holds.
* `libs/ra8_board_ra8p1/src/boot/nmi_exception.c` renames
  `internal_nmi_report` to `internal_ra8_board_nmi_report`,
  including the branch target inside the naked-asm handler. A
  rename, not a behaviour change.
* `libs/ra8_board_ra8p1/src/boot/system_init.c` hard-codes the
  shared-RAM MPU region (`k_ra8_mpu_shram_base` 0x22100000,
  `k_ra8_mpu_shram_limit` 0x2219FFE0) where the EK-RA8D2 copy
  derives base and extent from
  `libs/ra8_board_ek_ra8d2/inc/ra8_board_ek_ra8d2_dualcore.h`, drops
  `k_ra8_mpu_region_quantum`, swaps that include for
  `libs/ra8_core/inc/ra8_attributes.h`, marks two file-local helpers
  `RA8_INTERNAL`, and does not carry the `RA8_BOOT_CACHE_VIA_HAL`
  opt-in that routes L1 cache enable through `ra8_cache_icache_enable`
  for issue #577.
* `libs/ra8_board_ra8p1/src/boot/vector_table.c` is the more serious
  one. The EK-RA8D2 copy forwards every peripheral IRQ vector to
  `ra8_isr_dispatch` so that a `(handler, ctx)` pair armed through
  `ra8_isr_register` runs in NVIC handler-mode. Its own comment
  records why that replaced the previous shape: the older form
  weak-aliased every `IRQn_Handler` straight to `Default_Handler`,
  `Default_Handler` contains `bkpt #0`, and on debug-disabled
  silicon a stray `bkpt` escalates to HardFault, so the first
  SCI / SPI / USB interrupt a driver armed would halt the chip. The
  RA8P1 copy is still the older weak-alias-to-`Default_Handler`
  form, and does not include the `ra8_isr` substrate at all.

The RA8P1 vector-table copy is latent rather than live: its own
note records that every application under
`examples/ra8p1_foundation` overrides it with an app-local vector
table, so no shipping build reaches the stale path today. It
becomes live the moment an RA8P1 application is added that does
not override it, which is exactly what the canonical copy exists
for.

Nothing in the tree detects any of this. The only statement of
intent is prose in a file header, and prose cannot fail a build.

### Geometry stated in four places

The same SRAM window is written out independently in the device
header's memory-map enumerations
(`libs/ra8_core/inc/ra8_device.h`: base 0x22000000, size
0x001A0000), in `scripts/checks/check_linker_scripts.py` as the
hard-coded `SRAM_WINDOW_BASE` / `SRAM_WINDOW_SIZE` constants with
a comment at line 277 naming the header as the source it is copied
from, in the MPU enumerations of
`libs/ra8_board_ek_ra8d2/src/boot/system_init.c` and
`libs/ra8_board_ra8p1/src/boot/system_init.c`, and in the `MEMORY` blocks of the linker
scripts (139 tracked `.ld` files, 74 of which declare an `SRAM`
region). This is the "configuration in three places" shape the
platform-architecture epic exists to remove, and it is prior to
the selection model: a chip/board split of the header cannot be
judged correct while three other copies of the same numbers are
not derived from it.

## Decision

Four options, one constraint that narrows them.

**Option A -- promote the chip-boot TUs into a shared chip layer.**
Move the five translation units into a chip-level library that both
board libraries compile, and keep in each board library only the
boot code that is genuinely board-specific (the dual-core memory
map, the panel and connector surface, the linker script). This is
the target shape and the only one that makes the duplication
structurally impossible.

**Option B -- keep the copies, gate the drift.** Leave both trees
in place and add a check that compares the translation units the
RA8P1 headers declare identical against their EK-RA8D2
counterparts, failing when they diverge without a declared
deviation, in the two-direction style proposed for the RTOS
symbol-isolation check under #695 (a new divergence fails, and a
declared deviation that no longer exists also fails).
Cheap, behaviour-neutral, and it converts a prose claim into an
enforced one.

**Option C -- delete the RA8P1 boot copies** and have
`libs/ra8_board_ra8p1` consume the EK-RA8D2 board library's boot
sources directly. Removes the duplication with the least new
structure, at the cost of making one board library depend on
another, which is the wrong direction for a board layer and would
have to be undone by Option A later.

**Option D -- defer everything** until #611 and #226 land, per the
issue's own sequencing.

**The deciding constraint is that no RA8P1 silicon is in hand.**
#226 is open and labelled `needs-purchase`: the RA8P1 board layer
has never been brought up on hardware. Options A and C rewrite
the boot path of a board that cannot be bench-validated, and boot
code is the one place where an unvalidated refactor fails silently
until first power-on. Option B changes no compiled bytes, so it is
the only option that can land before an RA8P1 EK exists, and it
makes the drift that Option A must later fix visible and countable
rather than discovered by hand.

This ADR therefore recommends **Option B now, Option A once an
RA8P1 EK is in hand**, and records Option C as rejected on layering
grounds. It does not recommend Option D, because the drift
documented above is already in the tree and its
`vector_table.c` half reintroduces a hazard the canonical copy
documents having fixed.

Independent of which option is adopted, the three false
byte-identity claims in the RA8P1 boot headers should be corrected
to say what actually differs. That is a comment-only change and is
deliberately not bundled here: the ADR authoring rule in
`docs/adr/README.md` requires an ADR to land as its own commit.

The selection-model half of #696 (the `RA8_ARCH` / `RA8_CHIP` /
`RA8_BOARD` triple and the toolchain dispatcher) is not decided
here and stays sequenced behind #611. The premise corrections
above should be applied to the issue body before that work is
scheduled, so the estimate is not built on 227 Makefiles that do
not exist.

## Consequences

If Option B is adopted:

* A new check joins the gate registry and the duplication becomes
  a tracked number instead of a claim in a comment. The RA8P1
  divergences listed above have to be declared as deviations on
  the first run, which puts the stale vector table on the record.
* The duplication itself stays. Two copies of five boot
  translation units keep being maintained, and the coverage and
  MISRA obligations on both copies stay doubled.
* Nothing about the boot path changes, so no bench validation is
  owed.

If Option A is adopted later:

* The five translation units collapse to one copy each, and the
  RA8P1 divergences have to be resolved deliberately rather than
  inherited: the `internal_ra8_board_nmi_report` rename, the
  hard-coded shared-RAM MPU bounds, the missing `RA8_BOOT_CACHE_VIA_HAL`
  path, and the vector-table shape each become an explicit call.
* Both boards need bench validation of reset, NMI, SecureFault and
  first-interrupt paths, which is why this waits on #226.
* The board libraries gain the interface contract #696 asks for as
  a by-product, since what remains in each is by definition the
  board-specific surface.

If Option D is adopted:

* The stale RA8P1 vector table stays in the tree with a header
  asserting it is identical to a copy it is not identical to. The
  first RA8P1 application that does not ship its own vector table
  inherits the `bkpt`-to-HardFault hazard.

## References

* Issue #696 -- Platform-arch (d): board-description + arch/chip/board
  build-selection model (this ADR).
* Issue #692 -- EPIC: Platform architecture, agnostic
  multi-arch / multi-chip / multi-board structure.
* Issue #611 -- filesystem work #696 is sequenced behind.
* Issue #226 -- RA8P1 on-silicon bring-up, board layer landed,
  needs an RA8P1 EK. Source of the byte-identity notes.
* Issue #577 -- L1 cache enable through the `ra8_cache` HAL, the
  `RA8_BOOT_CACHE_VIA_HAL` opt-in present only in the EK-RA8D2 copy.
* Issue #1048 -- whether the device memory map has readers.
* Issue #220 -- RA8D2-vs-RA8P1 difference analysis and multi-chip plan.
* `libs/ra8_core/inc/ra8_device.h` -- compile-time device selection
  and the memory-map enumerations.
* `docs/reference/ra8p1_vs_ra8d2.md` -- the documented chip delta.
* ADR-0001 -- qualification target, source of the MC/DC obligation
  that duplication doubles.
