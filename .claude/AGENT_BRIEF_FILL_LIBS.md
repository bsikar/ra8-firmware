# Agent Brief: Fill Missing RA8D2 Peripheral Drivers

**You are working in /Users/bsikar/Documents/github/ra8d2-firmware** as part of a parallel
swarm of agents, each owning a different RA8D2 peripheral. Your peripheral is
specified in your individual prompt. This brief is the shared spec.

## Files YOU create (NEW files only, exact paths)

1. `libs/ra_hal/inc/ra8d2_<short>_regs.h` -- register layout. **CHECK FIRST**: this
   may already exist (e.g. `ra8d2_lpm_regs.h`, `ra8d2_lvd_regs.h`,
   `ra8d2_wdt_regs.h`, `ra8d2_tsn_regs.h`, `ra8d2_ospi_regs.h`). If so, **extend**
   it with whatever extra offsets/enums you need rather than recreating.
2. `libs/ra_hal/inc/ra_<short>.h` -- public API
3. `libs/ra_hal/src/ra_<short>.c` -- implementation
4. `tests/test_ra_<short>.c` -- unit test

## Files you MUST NOT touch

- Any `CMakeLists.txt` (auto-globbed via `CONFIGURE_DEPENDS` -- new sources/tests
  are picked up automatically; you do NOT need to wire anything)
- `MEMORY.md`, `CHAPTER_MAP.md`, `docs/ARCHITECTURE.md`, `docs/ROADMAP.md`
- Any existing driver files
- Any other agent's files (each agent owns a unique `<short>` name; do not stray)

## Reference materials (READ -- NEVER copy verbatim)

CLAUDE.md is explicit: "No Renesas FSP code in this tree." Use FSP and the HUM
**only** to understand register sequences -- every line you write is hand-authored
in this project's style with proper HUM citations.

- **HUM page PDFs**: `docs/reference/ra8d2-hardware-user-manual/page-NNNN.pdf`.
  Read individual pages with the Read tool (PDF supported; for chapter sweeps,
  read 5-10 pages around the register layout sections rather than the whole
  chapter -- you don't need every page).
- **FSP source** (reference only): `/Users/bsikar/Documents/github/fsp/ra/fsp/src/<r_xxx>/`
  and `/Users/bsikar/Documents/github/fsp/ra/fsp/inc/`. Your individual prompt
  names the directory.
- **Canonical templates -- copy STYLE from**:
  - `libs/ra_hal/src/ra_xspi.c` (large driver: lifecycle, IRQ, power, citations)
  - `libs/ra_hal/src/ra_glcdc.c` (smaller display driver -- closer to your target)
  - `libs/ra_hal/inc/ra8d2_lpm_regs.h` (regs header pattern -- offset enums)
  - `libs/ra_hal/src/ra_acmphs.c` + `tests/test_ra_acmphs.c` (driver+test pair)
- **CLAUDE.md** at repo root is the absolute spec -- read it.

## Hard rules from CLAUDE.md (do not violate)

- C23 typed enums for ALL constants: `typedef enum : uint8_t { k_xxx = N } name_t;`
  - `uint8_t` for small values, `uint32_t` for masks, `uintptr_t` for hardware addresses
- `#pragma once` for headers (NEVER traditional include guards)
- snake_case names; `k_` prefix for enum values; `s_` for static vars;
  `internal_` for static functions
- ZERO magic numbers -- every literal becomes a typed enum (including bit shifts,
  buffer indices, thresholds)
- 7-bit ASCII only (no UTF-8 -- pre-commit hook rejects it)
- No `#include <stdbool.h>` (`bool` is a C23 keyword)
- No `_Static_assert` (use C23 `static_assert`)
- No `= {0}` zero-init (use C23 `= {}`)
- Min 2 preconditions + 2 postconditions per function (NASA Power of 10 Rule 5)
  -- use `RA_CHECK_NULL_PTR` from `ra_check.h` for null guards
- Every function: full Doxygen with `@brief @details @param @return @retval
  @pre @post @note @see @since` (see CLAUDE.md for the full required tag list)
- Every register access has a citation comment immediately above:
  `/* HUM Ch X.Y "section name" p NNNN */`
  (also accepted: `/* HUM Ch X.Y "section name", p NNNN */` or
  `/* HUM Ch X.Y "section name" p NNNN-MMMM */`)
- Citations validated by `scripts/utils/cite_check.py` against page ranges in
  `docs/reference/CHAPTER_MAP.md` -- the page MUST fall in the chapter's range

## Driver API surface (minimal viable -- match ra_glcdc.h shape)

Keep it small. Aim for:

- `ra_<short>_init(const ra_<short>_config_t* cfg)` -- power on + configure
- `ra_<short>_deinit(void)` -- power off
- `ra_<short>_get_status(uint32_t* out)` / `ra_<short>_clear_status(uint32_t mask)`
- `ra_<short>_attach_handler(callback)` if peripheral has IRQ
- `ra_<short>_enter_stop()` / `ra_<short>_exit_stop()` for power transitions
- One or two peripheral-specific operations (e.g. start/stop, write/read)

Defer advanced features in the file header doxygen with a "Not yet implemented"
section. DO NOT try to cover the entire HUM chapter. The bar is "init +
lifecycle + one operation works in a unit test".

## Required power-up pattern (every driver does this)

Every peripheral needs its module-stop bit cleared in `init()`. Use
`ra_mstp_enable(k_ra_mstp_<peripheral>)` from `libs/ra_hal/inc/ra_mstp.h`.
If your peripheral does NOT have an `k_ra_mstp_<peripheral>` enum value yet,
DO NOT add one (that touches a shared file) -- instead, in `init()` perform a
direct register access to MSTPCRA/B/C/D/E and add a TODO comment. Cite HUM
Ch 11.2.x for the MSTPCR write.

## Test

Use `tests/test_ra_acmphs.c` as the template. Tests MUST:

- include `"unity_minimal.h"` and `"ra_sim_mmap.h"`
- call `ra_sim_mmap_reset()` at the start of each test
- assert `k_ra_ok` returns and verify register state via the sim mmap
- have a `main()` returning the test outcome (call `TEST_REPORT()` at end)
- cover at least 3 cases: happy init, null-arg rejection, one operation

The host test build expects: peripheral register accesses go through the
register accessor inline functions (not raw `(*(volatile uint32_t*)addr)`) so
the sim mmap can intercept them. Look at `ra_glcdc_reg32()` in `ra_glcdc.c` and
`ra_acmphs()` in `ra_acmphs.c` for the accessor pattern.

## Verification at end (do BOTH, do not skip)

1. **Citation check** (run from repo root):
   ```
   python3 scripts/utils/cite_check.py --strict \
     libs/ra_hal/src/ra_<short>.c \
     libs/ra_hal/inc/ra_<short>.h \
     libs/ra_hal/inc/ra8d2_<short>_regs.h \
     tests/test_ra_<short>.c
   ```
   Must exit 0. Fix any issues (check the cited page actually falls in the
   chapter's range from `CHAPTER_MAP.md`).

2. **Host test build** (best effort):
   ```
   cmake -S tests -B build/host_<short> -DCMAKE_BUILD_TYPE=Debug 2>&1 | tail -10
   cmake --build build/host_<short> --target test_ra_<short> 2>&1 | tail -30
   ```
   If the test compiles, also run the binary: `./build/host_<short>/test_ra_<short>`.
   If the build infra rejects your test, fix the test (NOT the build infra).
   If cmake itself fails to configure for unrelated reasons, document it and move on.

## Reporting

End your final message with a 3-line summary:
- Files created (full paths)
- `cite_check.py --strict` exit code
- Host test build/run result

Then call `TaskUpdate` to mark your task `completed`.

## A worked example -- what your prompt might look like

```
Implement RA8D2 ACMPLP (low-power analog comparator) -- HUM Ch 56, p 3508-3517.
FSP reference: /Users/bsikar/Documents/github/fsp/ra/fsp/src/r_acmplp/
Files:
  libs/ra_hal/inc/ra8d2_acmplp_regs.h
  libs/ra_hal/inc/ra_acmplp.h
  libs/ra_hal/src/ra_acmplp.c
  tests/test_ra_acmplp.c
Task ID: #N
```

You'd then read CLAUDE.md, ra_acmphs.c, ra_acmphs.h, ra8d2_acmphs_regs.h
(closest existing analog comparator driver), then HUM pages 3508-3517, then the
FSP r_acmplp.c, then write your four files following the pattern.
