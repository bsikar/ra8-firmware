# Agent Brief: Full Driver Coverage (Round 3)

**Goal**: Bring every existing RA8D2 HAL driver up to **STAR-pattern
completeness** -- every documented register exposed, every operating mode
reachable, every IRQ source hooked, full DMA coupling. No deferred features.

**You are working in /Users/bsikar/Documents/github/ra8d2-firmware** as part of
a parallel swarm. Your peripheral is in your individual prompt.

## What "complete" means

The reference is the STAR project at
`/Users/bsikar/Documents/github/STAR/star-rx72n-firmware/libs/rx_hal/src/`.
Sample driver sizes there:

  - `rx_eccram.c` -- 468 LOC (ECC controller, all modes)
  - `rspi.c` -- 1706 LOC (full SPI master/slave with DMA)
  - `rx_iwdt.c` -- 1661 LOC (full IWDT with all timeout/window modes)
  - `uart.c` -- 2257 LOC (full UART with hardware flow control)
  - `riic.c` -- 3134 LOC (full I2C master/slave with DMA)

Your driver, after this round, should:

1. **Cover every register field** documented in your HUM chapter -- no register
   should exist in the HUM chapter without a corresponding driver code path.
2. **Expose every operating mode** -- if the HUM lists 4 modes, all 4 must be
   reachable through the public API and have an enum value.
3. **Hook every IRQ source** -- every documented interrupt has a callback +
   dispatch path. Status registers expose every flag.
4. **Implement DMA coupling** where the peripheral supports it -- read FSP for
   the DMA hookup pattern, then write your own.
5. **Implement full lifecycle**: init, deinit, start, stop, reset, enter_stop,
   exit_stop, error recovery.
6. **Implement timeout/error paths** -- bounded waits with `k_ra_err_hw_timeout`
   returns, retry helpers where the HUM mentions them.
7. **Document every state transition** in `@par State Machine` doxygen sections
   with `@startuml` if the peripheral is stateful.

## What stays out of scope (NOT deferred -- genuinely not HAL)

These belong in PAL/protocol layers, not the HAL:

- **Full TCP/IP stack** -- `ra_net_pal` wraps lwIP for that
- **USB descriptor tables / class drivers (CDC/MSC/HID)** -- `ra_usb_pal` wraps
  CherryUSB; the HAL exposes pipes + EP0 setup, the PAL builds descriptors
- **File format parsers** (BSDL for boundary scan, ELF/HEX for flash) -- these
  are tool concerns
- **DSP filter design** (FIR coefficient computation) -- the HAL exposes the
  coefficient register write; the application computes the coefficients

If you find yourself writing a TCP three-way handshake or a USB descriptor
parser, you are out of bounds -- defer to the appropriate `*_pal` and document
the boundary.

## Files you may touch (this round only)

- `libs/ra_hal/src/ra_<short>.c` -- your driver (extend, don't rewrite)
- `libs/ra_hal/inc/ra_<short>.h` -- public API (add entry points)
- `libs/ra_hal/inc/ra8d2_<short>_regs.h` -- register layout (add anything missing)
- `tests/test_ra_<short>.c` -- tests (add coverage for every new entry point)

## Files you must NOT touch

- Other agents' drivers (each agent owns a single peripheral)
- `CMakeLists.txt` (auto-globbed)
- `MEMORY.md`, `CHAPTER_MAP.md`, any `docs/*.md`
- `libs/ra_core/` (shared utilities)
- `libs/ra_nsc/`, `libs/ra_net_pal/`, `libs/ra_usb_pal/` (PAL layers)

## Reference materials (READ -- never copy verbatim)

CLAUDE.md is explicit: "No Renesas FSP code in this tree." Use FSP and the HUM
**only** to understand register sequences -- every line you write is hand-authored
in this project's style with proper HUM citations.

- **STAR pattern** at `/Users/bsikar/Documents/github/STAR/star-rx72n-firmware/libs/rx_hal/src/`
  -- look at the larger files (`riic.c`, `rspi.c`, `rx_iwdt.c`, `uart.c`) for
  the level of detail expected.
- **HUM page PDFs** at `docs/reference/ra8d2-hardware-user-manual/page-NNNN.pdf`
  -- read every page in your chapter range.
- **FSP source** at `/Users/bsikar/Documents/github/fsp/ra/fsp/src/<r_xxx>/`
  for register-sequence reference.
- **Existing model files**:
  - `libs/ra_hal/src/ra_xspi.c` (613 LOC, well-developed)
  - `libs/ra_hal/src/ra_usb.c` (663 LOC, well-developed)
  - `libs/ra_hal/src/ra_sci.c` (543 LOC, well-developed)

## Hard rules from CLAUDE.md (do not violate)

- C23 typed enums for ALL constants (no magic numbers, ever)
- `#pragma once` (no traditional include guards)
- snake_case names; `k_` prefix for enum values; `s_` for static vars;
  `internal_` for static functions
- 7-bit ASCII only
- No `#include <stdbool.h>` (`bool` is C23 keyword)
- No `_Static_assert` (use C23 `static_assert`)
- No `= {0}` (use C23 `= {}`)
- Every function: full Doxygen with `@brief @details @param @return @retval
  @pre @post @note @see @since` + `@par State Machine` for stateful APIs
- Every register access: `/* HUM Ch X.Y "section name" p NNNN */` cite
  immediately above
- Min 2 preconditions + 2 postconditions per function (NASA Rule 5)
- Citations validated by `scripts/utils/cite_check.py --strict`

## Tests

Match the new completeness:
- One test per public entry point (happy + bad-arg + edge case)
- Tests for every operating mode (e.g. if the chip supports 4 SSIE formats,
  test all 4)
- Tests for every status flag and IRQ dispatch path
- Use `tests/test_ra_acmphs.c` as the baseline shape; bigger drivers can grow
  to 30-50 tests.

## Verification (do BOTH at the end)

1. **Citation check**:
   ```
   python3 scripts/utils/cite_check.py --strict \
     libs/ra_hal/src/ra_<short>.c \
     libs/ra_hal/inc/ra_<short>.h \
     libs/ra_hal/inc/ra8d2_<short>_regs.h \
     tests/test_ra_<short>.c
   ```
   Must exit 0.

2. **Standalone compile**:
   ```
   cc -std=gnu23 -DRA_SIMULATOR_MODE -DUNIT_TEST -Wall -Wextra -fsyntax-only \
     -Ilibs/ra_core/inc -Ilibs/ra_hal/inc -Ilibs/ra_net_pal/inc \
     -Ilibs/ra_usb_pal/inc -Ilibs/ra_nsc/inc -Itests/mocks -Isrc/secure_app \
     libs/ra_hal/src/ra_<short>.c tests/test_ra_<short>.c
   ```
   Must exit 0 (no warnings).

## Reporting

End with a 5-line summary:
- New entry points added (count + list)
- New register fields exposed
- New tests added (count)
- `cite_check.py --strict` exit code
- Standalone compile result

Then mark your TaskUpdate completed.

## Driver size targets (rough)

- Tiny chapters (< 10 pages, e.g. DOTF, Boundary Scan, VREG): 400-600 LOC OK
- Small chapters (10-30 pages): 600-1000 LOC
- Medium chapters (30-100 pages): 1000-2000 LOC
- Large chapters (100+ pages, e.g. CEU 56pp, MIPI-DSI 95pp, MRAM 83pp,
  LPM 68pp, USB 92+114pp, ADC 181pp, GLCDC 77pp): 2000-3500 LOC

These are not strict -- write what the chapter requires. But if your driver
ends round 3 still under 500 LOC for a 50+ page chapter, you almost certainly
deferred something. Re-read the chapter and finish.
