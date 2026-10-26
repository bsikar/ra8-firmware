# ADR-0018: Whether the portable libraries keep the ra8_ symbol
# prefix or move to a neutral one

* **Status:** Proposed -- the decision is the repository owner's to
  confirm. This record establishes the naming facts on `dev` today,
  orders the options, and recommends one. It adopts nothing.
* **Date:** 2026-10-26
* **Context issue:** RA8FW-618 slice 4 (naming), parent epic approved
  by Brighton on 2026-10-04 ("I'm good with everything listed here").
* **Related:** ADR-0014 for the record format; the slice-1 audit table
  (published in `libs/README.md`) as the membership authority; the
  slice-2 gate (`tools/check_portability.py`) as the machine-checked
  boundary the sweep must respect.

## Context

The epic's goal: a library that does not need RA8 hardware builds for
another target (TI, ESP32-C, RX72N) with only a new set of
`if_<chip>_*` implementations, no edits to the library. Slices 1-3 are
done or on the board: the audit table is published, the drift gate
runs, and the interface gaps the audit found are landing as PRs
#3857-#3862 (fw_if rng/gpio/uart/spi/i2c/dma).

What the tree calls its libraries, read on `dev` at `e400b9299`:

- 54 libraries carry the `ra8_` prefix.
- 3 carry `if_ra8_` (`if_ra8_cgc`, `if_ra8_gpt`, `if_ra8_vfs`): these
  are the RA8 implementations behind the chip-neutral `fw_if_*`
  interfaces, so the prefix is accurate for them -- they exist only
  because RA8 exists.
- 9 first-party `fw_if_*.h` interface headers are already neutral.

The slice-1 audit's finding stands: the `ra8_` prefix is accurate for
the RA8-only set and a lie for the portable set. A portable
`ra8_core` built for the ESP32-C6 in the slice-5 proof port would read
as a contradiction to every newcomer.

## Options

1. **Keep `ra8_` everywhere (status quo).** Zero churn, zero risk.
   Cost: the portable set stays mislabeled, the slice-5 proof port
   reads confusingly, and the audit table remains the only place the
   real scope lives.
2. **Rename the portable set to a neutral prefix (`fw_`), one sweep
   per lib.** The membership is not hand-drawn: the slice-2 gate
   already names the portable rows, so the sweep's boundary is
   machine-checked before and after. Cost: real churn now (symbols,
   module names, include dirs, CMake targets, README), one PR per lib
   under the rebase-merge discipline. Benefit: the prefix then means
   "RA8 implementation", and `if_<chip>_*` names every other chip's,
   which is the scheme the interface layer already established.
3. **Alias layer: new neutral names wrapping the `ra8_` originals.**
   No churn and no honest move: two names for one thing, doubles the
   surface the gate has to police, and the lie survives underneath.

## Recommendation

Option 2, scoped to the slice-1 portable table and sequenced leaf
libs first (`ra8_core` and the other no-dependency rows before
anything that includes them), one lib per PR, commits authored as
Brighton Sikarskie with no attribution trailers, landing bottom-up so
every PR rebases cleanly. The three `if_ra8_*` libraries keep their
prefix unchanged: they are RA8 implementations and the name says so.

The decision this record needs from the repository owner: adopt
option 2 (and the `fw_` prefix specifically), or name a different
prefix -- the sweep is identical either way.
