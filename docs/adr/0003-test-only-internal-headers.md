# ADR-0003: Test-only access to internal symbols via `<module>_internal.h`

## Status

Accepted -- 2026-03-04.

## Context

ADR-0001 commits to **MC/DC for every compound boolean decision**.
Many compound decisions in this firmware sit inside
`static`-qualified helper functions (parameter validators, packed
guard predicates, byte-order conversions) that are deliberately not
part of the public API. Examples:

* `internal_validate_arp_frame()` in `libs/ra8_net/src/ra8_net_arp.c`
* `priv_check_pwm_invariant()` in `libs/ra8_hal/src/ra8_pwm.c`
* `internal_pll_config_legal()` in `libs/ra8_hal/src/ra8_cgc.c`

These helpers are exactly where the most condition-dense `if (a && b
&& c)` lines live, because the public API typically delegates
input-shape checking to a handful of `internal_*` routines. To prove
MC/DC on these, the host test suite has to *call them directly* with
hand-built vectors -- but `static` linkage prevents that.

The candidates considered:

* **Promote the helper to public API.** Rejected: pollutes the
  surface area, leaks implementation details, and creates ABI
  obligations the project doesn't want.
* **Use `#ifdef RA8_TEST_BUILD` to drop `static`.** Rejected: the
  production binary's symbol table now depends on a build flag,
  which makes it harder to argue "tests exercise the same code
  the device runs". Also conflicts with link-time optimisation
  decisions inside the production build.
* **`#include` the .c file from the test.** Rejected: forces the
  test to inherit every file-scope `static` variable from the
  module under test, defeats clang-coverage's per-TU bookkeeping,
  and breaks down the moment two tests want to include the same
  module.
* **Friend declarations / weak symbols.** C has neither in a
  portable form.
* **Per-module `<module>_internal.h` header that exposes selected
  internals via non-`static` linkage.** Chosen.

The convention "expose just the bits the tests need, in a header
named to make the intent obvious" is widely used in projects with
similar coverage requirements (Linux kernel `<module>_internal.h`,
musl's `__*` namespace, some CMSIS driver test suites).

## Decision

* **Each module that needs MC/DC of internal helpers ships a
  companion header `<module>_internal.h`** under the module's
  `inc/` directory (or, for very large modules, a dedicated
  `inc/internal/` subdir).
* The header is **declared but not installed**. Production code
  must not `#include` it. The pre-commit hook's
  `check_world_tags.py` is the gate: the internal header is
  tagged as such and only `tests/` and the module's own
  `src/` are allowed to include it.
* **Symbols exposed via the internal header are renamed** from
  `static internal_x()` to `priv_<module>_x()` (or
  `ra8_<module>_priv_x()` for already-public-prefixed modules).
  The `priv_` prefix is established in `CLAUDE.md` and signals
  "test-only, do not call from production".
* **MC/DC tests cite the source file:line** of the compound
  decision in their `@par MC/DC:` block. The static gate
  `scripts/checks/check_new_compound_has_mcdc.py` matches
  newly-added `&&`/`||` against test functions whose
  `@par MC/DC:` block names the same source location, so a
  helper exposed via internal header still satisfies the
  per-commit gate.

## Consequences

### Positive

* MC/DC vectors can drive the helpers that actually contain the
  compound decisions, instead of relying on the public API to
  thread inputs all the way down (which is brittle and often
  impossible to do at N+1 vector minimality).
* Production binary is unchanged -- the symbol still exists with
  external linkage on the host build, but the production cross
  build never includes the internal header into anything that
  would call it.
* The intent ("this is a test seam") is encoded in the *name*
  (`<module>_internal.h`, `priv_*` prefix) rather than buried in
  conditional compilation, which makes review easier.

### Negative

* Internal helpers that previously had `static` linkage now have
  external linkage in the host test build. This costs a small
  amount of symbol-table noise but does not change call-graph
  inlining decisions in the cross build (where the header is
  not included).
* Two places now declare the same helper (the `.c` file's
  forward declaration, plus `<module>_internal.h`). Drift is
  caught by the compiler -- mismatched signatures fail to link.

### Neutral

* The choice is reversible per-module: a module that grows a
  new public API can fold its `priv_*` helpers back to `static`
  and delete the internal header without breaking the
  qualification posture, as long as the MC/DC vectors are
  re-routed through the new public surface.

## References

* `CLAUDE.md` -- "Naming Conventions" (`internal_` vs `priv_`
  prefix definition).
* `docs/RING_AND_WORLD.md` -- header tagging system that gates
  who can include what.
* `scripts/checks/check_world_tags.py` -- the include-policy
  enforcer.
* `scripts/checks/check_new_compound_has_mcdc.py` -- the static
  gate that ties new compound decisions to test vectors via
  source citation.
* `docs/MCDC.md` -- how MC/DC vectors cite the helpers they
  exercise.
