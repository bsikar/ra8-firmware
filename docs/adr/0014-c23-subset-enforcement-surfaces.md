# ADR-0014: Where the approved C23 subset is enforced, and what an
# analyzer-coverage claim may rest on

* **Status:** Proposed -- the decision is the repository owner's to
  confirm. This record establishes what the tree enforces today and
  names the constraint that orders the options. It adopts nothing.
* **Date:** 2026-09-17
* **Context issue:** Platform-arch (i): C23 / MISRA / analyzer
  coverage and deviation audit, parent epic RA8FW-298.
* **Related:** the Platform-arch (h) issue and ADR-0010 (the
  compiler / ABI / data-model contract), the MISRA edition question,
  and ADR-0002 (`docs/adr/0002-cppcheck-only-misra-enforcement.md`).

## Context

The issue asks for an inventory of every C23-only construct the project
permits, mapped to its MISRA treatment, its compiler support, its
analyzer support, its permitted scope, and any deviation it needs. The
issue is written as though that record does not exist yet.

Read against `dev` at `8e70a3da68c81f11675a9396a9d6deb0be27bd74`, that
is not the position. The approved subset is already enforced, in four
places, each owning one or two constructs:

1. `scripts/checks/check_c23_headers.py` -- every `enum` and
   `typedef enum` in first-party code must carry an explicit
   underlying type (`enum : <type>`), and headers use `#pragma once`.
   So C23 fixed-underlying-type enumerations are not merely permitted,
   they are mandatory, which is a stronger position than the issue's scope
   list assumes.
2. `scripts/checks/check_no_null.py` -- the `nullptr`-only rule. Its
   own docstring records that scope is derived from `git ls-files`
   so that `tools/` is held to the same bar as `libs/`.
3. `scripts/checks/check_c23_patterns.py` -- the canonical spellings:
   C23's empty initializer `{}` as the one first-party zero form,
   typed integer literal suffixes, and digit separators. Its docstring
   names its source of authority as the `CLAUDE.md` sections "C23
   Syntax" and "Constants and Macros".
4. `scripts/checks/patches/cppcheck-2.13/misra_9-c23-empty-initializer.patch`
   -- a local patch to the upstream MISRA add-on so that an empty
   aggregate initializer (C23 6.7.10) stops being reported as a Rule
   9.x structure violation, while non-aggregate and excess brace
   levels keep failing.

Item 4 is the one worth stating plainly: the MISRA baseline in this tree
is not stock MISRA C:2012. It is MISRA C:2012 plus a locally carried C23
amendment, applied as a patch to a checker add-on. The MISRA edition
question and ADR-0002 hold that decision; this record only observes that
the C23 gap that question describes is already being closed by hand, one
rule at a time, in a patch series.

## The constraint that orders the options

`scripts/checks/cppcheck_c23_compat.h`, read in full on `dev` at
`8e70a3da68c81f11675a9396a9d6deb0be27bd74`, is a cppcheck-only shim
passed to the gate with `--include=`. Its whole body is:

```c
#define nullptr ((void*)0)
```

Its header explains why: the pinned analyser (cppcheck 2.13, held by
`require_tool_versions`) does not recognise the C23 `nullptr` keyword
as a null pointer constant, so a guard such as
`if (fp == nullptr) { return err; }` fails to convince its value-flow
engine that `fp` is null on that path, and it reports a false-positive
`resourceLeak` / `memleak` on every guarded `fopen` or `malloc`. The
file also records why the `#define` is unconditional rather than
guarded behind `__CPPCHECK__` (a guard would make cppcheck explore
both configurations and re-surface the false positives) and why it is
not passed as `-Dnullptr=NULL` on the command line (that would disable
the automatic configuration exploration the `#ifdef`-selected build
variants rely on). The shim is correct, well argued, and firmware
never trips the underlying pattern, because NASA Power-of-10 Rule 3
forbids the heap and there is no `FILE*` on bare metal.

The consequence for the issue is the constraint this record exists to
name. **The analyser is deliberately shown a different language than the
compiler is.** For at least one construct in the approved subset, a
clean cppcheck run is evidence about `((void*)0)`, not about `nullptr`.
So the "analyzer support" column the issue asks for cannot be derived
from the gate passing: derived that way it would be self-certifying,
recording the shim's dialect and calling it C23 coverage. That is
exactly the failure mode the issue opens with -- parser acceptance is
not proof that the intended semantic rules were applied -- and the tree
contains a worked example of it rather than a hypothetical one.

Two things follow, and they are constraints rather than preferences:

* Each construct in the matrix needs a positive and a negative fixture
  proving what the checker actually diagnoses. The issue already asks
  for this; the shim is the reason it cannot be traded for "the gate is
  green".
* Every row must declare whether a compatibility shim or an add-on
  patch is in effect for that construct, because the row is otherwise
  a claim about a dialect nobody ships.

## Options

**A. One declared manifest, the existing checkers read from it.** A
single table (under `docs/`, or as data beside
`scripts/checks/check_c23_patterns.py`) lists each approved construct
with its MISRA treatment, permitted scope, shim or patch status, and
fixture pair. The four checkers above stop hard-coding their own
construct lists and read the manifest, so adding a construct is one
edit and an unlisted construct fails. Highest cost, and the only
option where the inventory cannot drift from what is enforced.

**B. Documentation-only inventory now, gates untouched.** Write the
matrix as a maintained document, citing the four surfaces, and accept
that it is a description rather than a mechanism. Cheap, honest about
being prose, and it goes stale exactly the way the naming inventory did
(see ADR-0006, which sequences the naming/linkage legacy backlog).

**C. Settle the MISRA edition first, then derive.** Rejected as a
sequencing choice, not on merit: the edition question's own analysis
puts a current edition behind a commercial checker and a budget
decision, and the empty-initializer patch shows the project already
handles C23-vs-2012 divergence per rule without waiting for an edition
change.

**D. Wait for the Platform-arch (h) contract.** ADR-0010 leaves the
portability tiers undeclared, and the issue's "permitted scope" column
(portable, platform-specific, both, neither) is written against those
tiers. The scope column is the one part of the matrix that genuinely
depends on that contract; the other four columns do not.

Recommended, subject to the owner's decision: **A for the four
constructs already enforced** (typed enums, `nullptr`, the empty
initializer, literal suffixes and digit separators), because those have
both a checker and a shim or patch status to record, and **defer the
permitted-scope column to the Platform-arch (h) contract**, since no
tier vocabulary exists to put in it yet. That splits the issue into a
part that can be closed and a part that is genuinely blocked, instead of
leaving the whole issue blocked behind the epic.

## Consequences

* The project gains a single answer to "is this construct allowed, and
  what proves the checker understands it?" Today that answer is
  assembled from four scripts, one patch and one shim.
* A shim or patch becomes a declared property of a construct rather
  than folk knowledge in a file header. `scripts/checks/cppcheck_c23_compat.h`
  is well documented in place, but nothing outside it records that one
  row of the subset is analysed in a different dialect.
* Option A adds a gate. Per the repository's gate-honesty model, it
  needs `--selftest` coverage in both directions (an unlisted
  construct fails, a listed construct whose fixture no longer
  diagnoses fails).
* If the manifest is adopted, ADR-0002 should gain a pointer to it, so
  a reader who starts at the MISRA decision finds the C23 amendments
  the tree carries.

## What was not checked

Stated plainly, because the shared build box would not admit a run for
the duration of this fire and no sweep of the tree was possible:

* **`CLAUDE.md` was not read.** `scripts/checks/check_c23_patterns.py`
  names its "C23 Syntax" and "Constants and Macros" sections as the
  source of the rules it enforces, so a normative statement of the
  subset does exist there. Nothing in this record claims what those
  sections do or do not say, and the acceptance criterion about
  `CLAUDE.md` is therefore neither confirmed nor refuted here.
* **No construct census was run.** A code search reported seven
  `constexpr` occurrences in the repository, and the first-party ones
  among them are C++ translation units
  (`libs/ra8_hal/src/ra8_ethosu_kernel.cc`,
  `apps/shared_libs/epub/tests/src/test_epub_xml_shim_cov2.cpp`)
  rather than C. That search index is not the working tree and its
  results carried an older commit, so it is a lead, not a count. No
  figure here should be quoted as an inventory of `typeof`,
  `_BitInt`, `#embed`, checked arithmetic or the C23 attributes.
* **No gate was run and no build was performed.** This change compiles
  nothing, so no `cmake --preset ra8d2-debug` and no `ninja`
  invocation is claimed. `scripts/checks/check_markdown_references.py`
  and `scripts/checks/check_final_newline.py` were not run on this
  branch; every repository path above is written out in full, since
  `scripts/checks/markdown_reference_policy.py` matches bare
  basenames, and the only bare token left is `CLAUDE.md`, which that
  policy treats as a root file token. **A re-run of the
  markdown-reference gate is owed on this branch.**

## References

* Platform-arch (i): C23 / MISRA / analyzer coverage and deviation
  audit (this record's issue), and its parent epic RA8FW-298.
* `scripts/checks/check_c23_headers.py`,
  `scripts/checks/check_no_null.py`,
  `scripts/checks/check_c23_patterns.py`.
* `scripts/checks/cppcheck_c23_compat.h`.
* `scripts/checks/patches/cppcheck-2.13/misra_9-c23-empty-initializer.patch`.
* `docs/adr/0002-cppcheck-only-misra-enforcement.md`, and the MISRA
  C:2012-vs-2025 edition question it pins.
* The Platform-arch (h) issue for the compiler, ABI and data-model
  contract this record defers the permitted-scope column to.
