<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 Brighton Sikarskie -->

# ADR-0012: Where the C23 capability and ABI contract is asserted

* **Status** -- Proposed. The decision below is the owner's to confirm; this
  record frames it and names the constraint that narrows it.
* **Date** -- 2026-09-17
* **Issue** -- #787 (Platform-arch (h): portable C23 compiler / ABI /
  data-model contract), parent epic #692.

## Context

#787 asks for a contract with one central property: *required language and
library features are verified with compile probes, not inferred only from a
compiler name or version.* Before choosing a shape for that contract, this
record establishes what the tree asserts today and where.

Everything below was read on `dev` at commit
`7a324e21d2140f88db1f4ed20ce13bdbfeaad3d6`. The scope of the reading is stated
honestly in "What was not checked" at the end: five files, no tree-wide sweep,
so nothing here claims that a construct or an assertion is absent from the tree
as a whole.

### 1. The language level is set, and set permissively

The top-level `CMakeLists.txt` sets `CMAKE_C_STANDARD 23`,
`CMAKE_C_STANDARD_REQUIRED ON` and `CMAKE_C_EXTENSIONS ON`. GNU extensions are
therefore enabled for every cross target configured from that listfile, and
`tests/cmake/host_config.cmake` repeats the same three settings for the host
test build. #787 item 2 wants neutral code held to strict ISO C23 by default,
with extensions confined to named platform surfaces. Neither of those two
configuration files offers a strict-ISO configuration to hold anything to, so
the extension policy has no build path behind it yet.

### 2. Capability is inferred from the compiler version, deliberately

`cmake/toolchain-ra8d2.cmake` pins the cross compiler by version: it runs
`-dumpfullversion`, matches `major.minor` against `RA8_PINNED_ARM_GCC_VERSION`
("13.3"), and raises `FATAL_ERROR` on a mismatch unless
`RA8_STRICT_TOOLCHAIN` is turned off. The justification recorded in the file is
codegen reproducibility for the vendored miniz inflater under strict aliasing
(#178), which is a good reason to pin a version and not a statement about
language features.

The same file sets `CMAKE_C_COMPILER_WORKS 1` and `CMAKE_CXX_COMPILER_WORKS 1`,
because the cross compiler cannot produce host executables. That is correct,
and it is also the constraint that decides this ADR: with those variables
forced, CMake runs no compiler test of its own during a cross configure, so the
cross build performs **no** configure-time probe of anything. Capability is
inferred entirely from the pinned version string, which is exactly the
substitution #787 rules out.

### 3. One float-ABI capability claim is already marked unverified in-tree

`cmake/toolchain-ra8d2.cmake` compiles with
`-mcpu=cortex-m85 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16`, the
single-precision FPU variant, and its comment records why the
double-precision variant would be wrong for the RA8D2.
`cmake/toolchain-ra8p1.cmake` includes that file verbatim and appends
`-mfpu=fpv5-d16` so the last `-mfpu` wins for both compile and link, carrying a
bracketed note in the file that the RA8P1 double-precision FPU presence is "to
be re-verified against the RA8P1 CMSIS core header and HUM R01UH1064EJ once
silicon/datasheet access is available".

So a flag that changes floating-point ABI and codegen for a whole chip is set
from an unconfirmed premise, and the only record of the doubt is a comment.
A probe cannot settle this one either: it is a silicon fact, not a compiler
fact. It belongs in the target/toolchain capability matrix #787 asks for, as an
unconfirmed row.

### 4. The freestanding contract is stated twice, and the two statements differ

`cmake/toolchain-ra8d2.cmake` defines `RA8_FREESTANDING` for every cross target,
links with `-nostdlib`, and its comment says "no standard libc startup or
default runtime libraries (newlib/libnosys forbidden)".
`cmake/toolchain-ra8p1.cmake` describes the inherited link flags as "the
newlib-nano linker flags" and explains that "the link step selects the
newlib-nano multilib from the effective `-mfpu`", which is why the override has
to reach the linker flags too.

Both statements are about the same link, and they do not agree on whether
newlib-nano is in the picture. #787 item 5 wants the freestanding contract
enumerated; today it is a define plus two divergent comments.

### 5. Two C implementations already build the same first-party sources

The host test configuration in `tests/cmake/host_config.cmake` builds every
translation unit with `RA8_OFF_TARGET` and `UNIT_TEST` defined, and states the
ABI divergence in a comment where it drops the target's stack-usage bound:
"the host compiler ABI pushes wider arguments than Cortex-M85". The same file
pins the host image with `-no-pie -Wl,-Ttext-segment=0x70000000` on Linux so
static data cannot land inside the fake peripheral windows.

That is the data-model question #787 poses, already live: one set of sources,
two implementations (freestanding `arm-none-eabi` and the hosted 64-bit host
compiler), with the difference recorded in prose rather than asserted anywhere
that can fail.

### 6. The probe idiom #787 wants already exists, one directory away

`tests/cmake/host_config.cmake` uses `CheckCCompilerFlag` to probe
`-fcoverage-mcdc` and `-fcondition-coverage`, and it carries the lesson a
language probe would otherwise have to learn again (#346): it clears the cached
result first, because the answer is a property of the compiler rather than of
the build directory, and a stale cache once let a gate report MC/DC support
from a compiler that had none. It also fails closed when the requested
capability is missing rather than degrading quietly.

So the shape, the cache discipline and the fail-closed default are settled
practice in this tree. What is missing is a probe of the *language*, and a home
for it on the cross side where no probe runs at all.

## Decision

Deferred to the owner. Four options, with what each costs.

**A. Configure-time compile probes for the cross build.** A probe module
included by both toolchain files, testing each required C23 construct with
`check_c_source_compiles`. Matches #787's wording directly and reuses the idiom
in section 6. Cost: with `CMAKE_C_COMPILER_WORKS` forced and `-nostdlib` in the
link flags, a probe must be compile-only, so the module has to set
`CMAKE_TRY_COMPILE_TARGET_TYPE` to `STATIC_LIBRARY` for every check. That is a
real constraint on the option rather than a preference: a probe that tries to
link will fail for reasons unrelated to the feature it is testing.

**B. In-source assertions on one platform header.** `static_assert` on the
data-model invariants (`CHAR_BIT`, exact-width integer availability, pointer
and size width, enum ABI) plus `#error` on a missing feature, in one header
every first-party translation unit already includes. Cost: it fires at compile
time rather than configure time, so a failure surfaces as a wall of errors
across the tree instead of one actionable configure diagnostic; and it covers
the data model well but language *features* poorly.

**C. Keep the version pin as the only gate, and say so on purpose.** Record in
the qualification documentation that capability is asserted by pinning Arm GNU
Toolchain 13.3 and that the pin is load-bearing. Cost: it closes #787 by
narrowing it, and the first genuinely dissimilar target breaks the claim.

**D. Both A and B, split by kind.** Probes for language and library features at
configure time; assertions for data-model and ABI invariants at compile time;
the silicon facts from section 3 in a declared capability matrix that carries
unconfirmed rows as unconfirmed. Most work, and the only option that covers all
three kinds of claim in the issue.

The recommendation is **D**, sequenced so the probe module lands first and the
matrix is declared before a second dissimilar target is called portable. The
portability tiers #787 names (`portable-core`, `portable-octet`,
`platform-specific`) are deliberately **not** proposed here: no file read for
this record names a tier, both implementations in the tree today have an 8-bit
byte, and the C28x counterexample that motivates the split is hypothetical
until a toolchain for it exists. Declaring a tier vocabulary before a second
data model is real would be structure ahead of evidence.

## Consequences

* The version pin stays either way. Nothing in this record argues for relaxing
  it: it protects codegen, and it is only its use as a *capability* claim that
  #787 objects to.
* Under A or D, every cross configure gains a probe step. The probes must be
  compile-only for the reason in option A, and a failure has to name the
  construct and the tier, not just the flag.
* Under B or D, one header becomes the single home for the data-model
  invariants, and the assertions currently carried by product code (for
  example the IEC 60559 guards #747 records in the media-download state
  header) move there rather than being duplicated.
* The RA8P1 double-precision FPU premise in section 3 stays unconfirmed until
  silicon or the RA8P1 hardware manual is in hand, whichever option is chosen.
  It should be visible as an unconfirmed row rather than as a comment in a
  toolchain file.
* The disagreement in section 4 is worth fixing on its own, whichever option
  wins: one of the two comments is wrong about the link, and a reader cannot
  tell which from the files alone.

## What was not checked

* **No tree-wide sweep was run this fire.** The claims above are scoped to five
  files read at `dev` commit `7a324e21d2140f88db1f4ed20ce13bdbfeaad3d6`: the
  top-level `CMakeLists.txt`, the `cmake/` directory listing,
  `cmake/toolchain-ra8d2.cmake`, `cmake/toolchain-ra8p1.cmake` and
  `tests/cmake/host_config.cmake`. No count is derived from a search, and no
  statement here asserts that a construct, a `static_assert` or a probe is
  absent from the tree as a whole.
* The C23 construct inventory #787 asks for (which constructs the tree actually
  uses, and which of them the analyzer applies semantic rules to) is **not**
  attempted here. It needs a sweep and an analyzer run, and it is the natural
  next slice.
* Nothing here is a build. This record touches no build input.

## References

* #787 -- Platform-arch (h): portable C23 compiler / ABI / data-model contract.
* #692 -- EPIC: platform architecture.
* #788 -- Platform-arch (i): C23 / MISRA / analyzer coverage and deviation
  audit, which owns the analyzer half of the same question.
* #178 -- the codegen reason the cross compiler version is pinned.
* #346 -- the stale-probe-cache failure the host configuration learned from.
* #747 -- IEC 60559 guards asserted in a product header today.
* `docs/adr/0002-cppcheck-only-misra-enforcement.md` -- the analyzer budget
  decision that bounds what any language policy can be enforced with.
