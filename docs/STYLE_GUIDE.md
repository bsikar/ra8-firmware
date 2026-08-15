# ra8-firmware style guide

Authoritative reference for source-code conventions in this repository.
Code review and the pre-commit hook expect everything below; CLAUDE.md
restates the most-violated rules but **this file is the source of truth**.

## Table of contents

1. [File-header Doxygen block](#file-header-doxygen-block)
2. [Function documentation](#function-documentation)
3. [Comment formatting](#comment-formatting)
4. [Naming](#naming)
5. [Types and constants (C23)](#types-and-constants-c23)
6. [Header guards](#header-guards)
7. [Hardware register access](#hardware-register-access)
8. [HUM citations](#hum-citations)
9. [SOLID principles for C](#solid-principles-for-c)
10. [NASA Power of 10](#nasa-power-of-10)
11. [Backward compatibility (there is none)](#backward-compatibility-there-is-none)
12. [Character encoding](#character-encoding)
13. [Ring and World tagging](#ring-and-world-tagging)

## File-header Doxygen block

Every `.c` and `.h` opens with the same Doxygen comment block, in the
order below. The order itself is a readability convention: no tool parses
it, and nothing breaks if you swap two tags. (This paragraph used to say
"order matters -- the cite_check / world_tag scripts grep on it". Neither
ever has. `cite_check.py` greps `HUM Ch`, `check_world_tags.py` greps the
`[Ring N / X]` / `{World: X}` pair, and neither has ever read `@file`,
`@brief` or `@details`. What the tags themselves are held to is in the
table below, one enforcer named per row.)

```c
/**
 * @file ra8_glcdc.c
 * @brief Graphics LCD Controller driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * One paragraph summary of what the file does, what hardware it
 * touches, and what other files it depends on. Reference the
 * relevant HUM chapter so readers know where to look.
 *
 * @par Register protection sequencing
 * (Optional named @par sections for tricky details that don't fit
 * inline as comments. STAR uses these heavily for PRCR sequencing.)
 *
 * @author Brighton Sikarskie
 * @date 2026-04-21
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
```

Each row names the check that actually fails a build when the rule is
broken. A row whose enforcer is `--` rests on review, and says so rather
than implying a guarantee that does not exist. The counts are a dated
measurement (2026-07-28, 2121 first-party C files), not a live invariant --
they are here so the Required column can be audited rather than believed.

| Tag | Required | Enforced by | Notes |
|---|---|---|---|
| `@file` | Yes | `doxy_audit.py --style` | Must be present and must name **this** file: the bare basename (1648 files), the repo-relative path (456), or no argument at all (17, doxygen then infers it). A `@file` left naming the old location after a `git mv` fails here rather than becoming a doxygen warning nobody reads. |
| `@brief` | Yes | `doxy_audit.py --style` | One sentence, ends without a period. |
| `@par Tag` | Yes for Ring 3+ | `check_world_tags.py` | See [Ring and World tagging](#ring-and-world-tagging). |
| `@details` | Yes | `doxy_audit.py --style`, **ratcheted** | Multi-paragraph explanation. 130 files predate enforcement and are frozen in `.github/doxy-details-baseline.txt`; that list may only shrink, and a file outside it fails on sight. |
| Named `@par <Name>` | Optional | -- | Use for PRCR sequencing, IRQ wiring, state machines, anything subtle. |
| `@author` | Optional | -- | Review convention, not a rule: 104 of 2121 first-party C files carry one, and `@copyright` below already names the author. Keep it where it exists; a new file does not need it. This row said "Yes" for the life of the tree while 95% of it disagreed and nothing checked. |
| `@date` | Optional | -- | ISO 8601 (YYYY-MM-DD) where present; 103 files carry one. |
| `@copyright` + SPDX | Yes | `check-copyright.py` | `Copyright (c) YEAR <author>` then `SPDX-License-Identifier: MIT`. The check greps the SPDX line and the copyright holder's name anywhere in the file -- it does not require the `@copyright` tag spelling, which is why 63 files (54 in `tools/`, 9 in `port/`) satisfy it with a plain `/* SPDX... */` pair outside the header block. |
| `@since` | Yes on public declarations | `check-since-version.py` | Semantic version the file first appeared at. **Presence** is gated only for `ra8_*` declarations in `libs/ra8_*/inc/`; the **value** of every `@since` anywhere in the tree must equal the top-level `VERSION`. |

## Function documentation

Every function -- public or static -- gets a full Doxygen block. The
required tags are:

```c
/**
 * @brief Configure an RA8D2 PORT pin as a digital output
 *
 * @details
 * Multi-paragraph explanation. Include the algorithm steps if the
 * function is non-trivial. Mention thread safety, ISR safety, and
 * any external invariants the caller has to maintain.
 *
 * @param[in] port Port identifier (k_ra8_port_0 .. k_ra8_port_11)
 * @param[in] pin  Pin index within the port (0..15)
 * @param[in] init_level Initial output level
 *
 * @return ra8_err_t Error code
 * @retval k_ra8_ok Success, pin configured and driven to init_level
 * @retval k_ra8_err_invalid_arg port or pin out of range
 *
 * @pre Power to the IOPORT module is on (MSTPCRB cleared for IOPORT)
 * @pre Caller holds the pin validator lock
 *
 * @post Pin is driven to init_level
 * @post PFS register locked after write
 *
 * @note Not thread-safe; caller must provide synchronization
 * @warning PFS writes without unlocking PWPR are silently dropped
 *
 * @par Example
 * @code
 *   ra8_gpio_output_init(k_ra8_port_1, 7, k_ra8_level_high);
 * @endcode
 *
 * @see ra8_gpio_write
 * @since 0.1.0
 */
ra8_err_t ra8_gpio_output_init(ra8_port_t port, uint8_t pin, ra8_level_t init_level);
```

Gated by `doxy_audit.py --check` (the `pre-commit-checks` gate and the
pre-commit hook), over every function -- including statics -- in `libs/`,
`src/` and `port/`:

- `@brief`, `@details`, `@param` for every parameter, `@return`
- At least 2 `@pre` and 2 `@post` (NASA Power of 10 Rule 5)
- `@retval` for every distinct return value, on any non-`void` function
- `@note` mentioning thread safety
- `@since` semantic version

`@see` is a **review convention, not a gate**. It is worth writing where a
reader would genuinely want the pointer, and nothing checks it: measured
2026-07-28, 3014 of the 3162 documented function blocks in the tree have
none, and a rule that demanded one everywhere would be closed with filler
cross-references rather than useful ones. The same applies to `@warning`
(3142 without) and `@par MC/DC:` (3099 without) -- write them where they say
something.

`@param` direction tags are mandatory: `@param[in]`, `@param[out]`,
`@param[in,out]`. Plain `@param` without the direction is rejected by
`doxy_audit.py --style`, which reads every Doxygen comment in first-party C
rather than only function blocks. That scope is the point: of the 55
directionless tags the rule found when it was first enforced, **none** sat on
a function declaration -- 53 documented function-like macros and 2 documented
a callback typedef, and the function gate looks at neither. Any other bracket
text (`@param[inout]`, `@param[i]`) is rejected too; a typo that silently
means nothing to doxygen is not an improvement on a missing tag.

## Comment formatting

Spacing inside single-line block comments is enforced by
`scripts/checks/check_comment_format.py`, which runs as part of `make format`
(applies) and `make check` (verifies), and so gates pre-commit and CI through
`scripts/checks/format_code.sh`. The rules:

- **One space after the opener.** `/*text` -> `/* text`; the Doxygen member
  form gets `/**< text` (never `/**<text`).
- **One space before the closer.** `text.*/` -> `text. */` (never `text.*/`).
- **Aligned `*/`.** Across a run of consecutive trailing comments that
  clang-format put in the same start column, the closing `*/` are padded so
  they line up under the longest comment in the run (the longest gets one
  space):

  ```c
  uint16_t clock_stop_time;       /**< CLSTPTSETR.CLKSTPT[11:2].  */
  uint8_t  clock_beforehand_time; /**< CLSTPTSETR.CLKBFHT[23:16]. */
  uint8_t  clock_keep_time;       /**< CLSTPTSETR.CLKKPT[31:24].  */
  ```

- **One block, one alignment.** A run of trailing comments must align as a
  single unit: one `/**<` column and one `*/` column. clang-format aligns a run
  to its widest code plus one space, but when that column would push the
  longest comment past column 100 it abandons the run and starts a fresh group
  mid-block, leaving one struct with two of each:

  ```c
  /* WRONG -- one struct, two alignment groups */
  ra8_fs_mount_t* mount;         /**< Owning mount point.                       */
  uint32_t        cur_cluster;   /**< Cluster the offset currently points into. */
  uint32_t walk_cache_idx;     /**< Read accelerator: chain index whose cluster is cached below. */
  uint32_t size_bytes;         /**< File size (DIR_FileSize).                                    */
  ```

  That is canonical clang-format output, so the formatter will not object; the
  pass reports it instead. It cannot repair it -- shortening prose is the
  author's call -- so the remedy is yours: **tighten the long comment**, or
  **move it to its own `/** ... */` block above the line it documents**, which
  ends the run there and stops one over-long declaration dragging the whole
  block right. A run ends at a blank line, a code-only line, a standalone
  comment, or an inline mid-code comment.

Division of labour: **clang-format owns the comment _start_ column**
(`AlignTrailingComments: true` aligns each `/**<` to the widest code in the
block + one space) and never touches the comment interior
(`ReflowComments: false`); **the pass owns the interior + the `*/` column**.
Because the two never overlap, they reach a stable result together (a comment
the pass tightens can free column budget that lets clang-format re-align the
start, so `make format` repeats both to a fixed point).

Left untouched: multi-line `/** ... */` blocks, `//` line comments, decorative
banners (`/**** ... ****/`), and inline mid-code comments (`f(/*tag=*/x)`) --
clang-format owns the spacing around those.

## Naming

| Identifier kind | Convention | Example |
|---|---|---|
| Functions | `snake_case` | `ra8_gpio_output_init` |
| Public types | `snake_case_t` | `ra8_port_pin_t` |
| Private types | `snake_case_t` | `ra8_drv_state_t` |
| Macros / `#define` | `SCREAMING_SNAKE` | `RA8_RETURN_ON_ERROR` |
| Enum values | `k_<scope>_<name>` | `k_ra8_ok`, `k_ra8_pin_led1` |
| File-local (`static`) functions | `internal_<verb>` | `internal_validate_freq` |
| Cross-TU module-private functions | `priv_<verb>` | `priv_unlock_pwpr` |
| File-scope static data / constants | `s_<name>` | `s_tag` |
| Global variables (avoid) | `g_<name>` | `g_ra8_vector_table_start` |
| Linker symbols | `g_ra8_ls_<name>` | `g_ra8_ls_stack_top` |

The `g_ra8_ls_` prefix on linker symbols is mandatory: it keeps them
out of the leading-underscore reserved namespace that ISO C and
`cert-dcl37-c` reject.

The three private prefixes describe linkage; they are not interchangeable
abbreviations. A file-local helper is declared `RA8_INTERNAL static` and uses
`internal_`. A helper intentionally shared by multiple translation units in
one `libs/<module>` or `tools/<tool>` is non-`static`, uses `RA8_PRIV` and the
`priv_` prefix, and is declared in that module's `*_internal.h`. The `s_`
prefix is data-only: it is never a function name and never names automatic or
externally-linked data. Public functions use their published API name and no
private linkage annotation; `RA8_TEST_HELPER` is the explicit test-only
external-linkage exception. `scripts/checks/check_annotations.py` enforces
these pairings against the AST rather than inferring linkage from spelling.

## Types and constants (C23)

The project targets **C23 with GNU extensions** (`-std=gnu23`). Use
the modern features:

```c
// Always: typed enums with explicit underlying type.
typedef enum : uint8_t {
  k_ra8_state_idle      = 0,
  k_ra8_state_busy      = 1,
  k_ra8_state_error     = 2,
} ra8_state_t;

// Always: static_assert (C23 keyword), not _Static_assert.
static_assert(sizeof(ra8_state_t) == 1, "tightly-packed enum");

// Always: zero-init with empty braces.
ra8_drv_state_t s = {};

// Never: stdbool.h. `bool`, `true`, `false` are C23 keywords.
// Never: _Static_assert. Use static_assert.
// Never: = {0} zero-init. Use = {}.
// Never: untyped enums. Always pick the underlying type.
```

Underlying-type choice:

- `uint8_t` -- values 0..255 (states, indices, small constants)
- `uint16_t` -- 256..65535 (timeouts in ms, medium constants)
- `uint32_t` -- > 65535 (large constants, bit masks)
- `uintptr_t` -- hardware register base addresses (mandatory)
- `int8_t`/`int16_t`/`int32_t`/`int64_t` -- signed values

`uintptr_t` for register bases is non-negotiable: on the 32-bit
RA8D2 target it's `uint32_t`, on the 64-bit x86_64 unit-test host
it's `uint64_t`. Using `uint32_t` for an address silently truncates
on the test host and produces wrong pointer casts.

Magic numbers are forbidden -- every literal becomes a typed enum:

```c
// CORRECT
typedef enum : uint8_t {
  k_idx_high_byte = 0,
  k_shift_byte    = 8,
  k_mask_byte     = 0xFF,
} byte_layout_t;

buf[k_idx_high_byte] = (uint8_t)((val >> k_shift_byte) & k_mask_byte);

// WRONG
buf[0] = (uint8_t)((val >> 8) & 0xFF);  // What is 0? 8? 0xFF?
```

## Header guards

Use `#pragma once`. Traditional `#ifndef`/`#define`/`#endif` guards
are rejected by code review.

```c
/* file header doxygen block */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* declarations */

#ifdef __cplusplus
}
#endif
```

## Hardware register access

Use **inline accessor functions**, never macro-style register pointers:

```c
// CORRECT
typedef enum : uintptr_t {
  k_ra8_glcdc_base_addr = 0x40340000UL,
} ra8_glcdc_addr_t;

static inline volatile r_glcdc_regs_t* ra8_glcdc(void)
{
  return (volatile r_glcdc_regs_t*)k_ra8_glcdc_base_addr;
}

// Usage
ra8_glcdc()->BG_PERI = 0x12345678U;

// WRONG
#define GLCDC_BASE ((volatile r_glcdc_regs_t*)0x40340000UL)
GLCDC_BASE->BG_PERI = 0x12345678U;
```

The inline-accessor approach lets the host `RA8_OFF_TARGET` build
intercept register writes by linking a different `ra8_glcdc()` body.
Macros foreclose that.

## HUM citations

Every register read or write -- whether through an accessor or a raw
volatile pointer -- carries a comment immediately above citing the
RA8D2 Hardware User's Manual:

```c
/* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
*ra8_mstp_reg32(k_ra8_mstpcrc_off) &= ~(1U << k_ra8_mstpc_glcdc_bit);
```

The format is:

```
/* HUM Ch X.Y "Section name" p NNNN */
/* HUM Ch X.Y "Section name", p NNNN */          (comma OK)
/* HUM Ch X.Y "Section name" p NNNN-MMMM */      (page range)
```

`scripts/checks/cite_check.py` walks every `.c` / `.h` and verifies
that each cite's chapter exists in `docs/reference/CHAPTER_MAP.md`
and the page falls within the chapter's range.

## SOLID principles for C

The same SOLID principles that apply to OO carry over to procedural C
with small adaptations. Each point also references the
analogous structure in the STAR project for cross-pollination.

### Single Responsibility (S)

- **One module = one purpose.** `ra8_pid` would handle ONLY PID
  arithmetic -- no motor control, no hardware. STAR's `rx_pid`
  follows the same rule.
- **One function = one action.** `ra8_pid_compute` does math;
  `ra8_pid_reset` clears state. They do not share an entry point.
- **Separation of concerns.** Configuration (`ra8_pid_config_t`) is a
  separate type from runtime state (`ra8_pid_handle_t`). The handle
  carries the cfg by value at init time so subsequent edits to the
  cfg struct don't ghost-update running PIDs.

### Open/Closed (O)

- **Extensible without modification.** Drivers configure via a
  `_config_t` struct passed to `_init`. Adding a new feature gates
  on a new field in the struct, not a new entry point.
- **Runtime tuning.** `_set_*` setters allow updates without
  recompilation. STAR's `rx_pid_set_gains` is the canonical example.
- **Avoid hardcoded values.** All limits live in the config:
  `output_min`, `output_max`, `integral_min`, `integral_max`.

### Liskov Substitution (L)

- **Implementations are interchangeable.** A bus manager accepts any
  bus type (I2C/SPI/1-Wire) through the same vtable shape.
- **Mocks substitute real implementations.** Tests use
  `mock_ra8_bus_iic` in place of real hardware -- the HAL's mock vs.
  prod selection is at link time, not source time.
- **Consistent error handling.** All drivers return `ra8_err_t` with
  the same semantics. A caller can write a generic error handler
  that handles every driver uniformly.

### Interface Segregation (I)

- **Small, focused interfaces.** A bus interface splits into
  `read()`, `write()`, `configure()` -- not a single fat
  `do_anything(verb, args)` entry point.
- **Separate read / write paths.** Half-duplex consumers don't pay
  for full-duplex code.

### Dependency Inversion (D)

- **High-level modules depend on abstractions, not implementations.**
  ```c
  typedef struct {
    ra8_err_t (*read)(void* ctx, uint8_t* data, uint32_t len);
    ra8_err_t (*write)(void* ctx, const uint8_t* data, uint32_t len);
    void* ctx;
  } bus_interface_t;
  ```
- **Function pointers for late binding.** This is the project's
  intentional deviation from NASA Power of 10 Rule 9 -- the
  testability win is worth the relaxed pointer-discipline rule.
- **Inject mocks via the same interface.** Test code links a mock
  vtable; production code links the real one.

## NASA Power of 10

Safety-critical embedded conventions, per JPL "The Power of 10:
Rules for Developing Safety-Critical Code". The project follows
all 10 with a single intentional deviation:

| # | Rule | This project |
|---:|---|---|
| 1 | Simplify control flow -- no `goto`, `setjmp`, recursion. | Compliant. |
| 2 | All loops have fixed upper bounds. | Compliant; main loops are `while(1)` with watchdog refresh, exempt by convention. |
| 3 | No dynamic memory after init. | Compliant. Zero `malloc`/`free` in firmware. `_sbrk` traps any accidental use. |
| 4 | Functions ~60 lines max. | Compliant. clang-tidy `LineThreshold = 60` enforces. NOLINT only for legitimately linear HUM-spec init paths. |
| 5 | Two assertions per function. | Compliant. Use `RA8_CHECK_NULL_PTR` for preconditions, output bounds checks for postconditions. |
| 6 | Smallest scope. | Compliant. File-scope vars are `static`; loop counters live in the `for`-statement. |
| 7 | Check all return values. | Compliant. `RA8_RETURN_ON_ERROR` macro propagates; `(void)` casts mark explicit ignores -- but never at a TrustZone boot boundary. A C23 `(void)` cast suppresses `[[nodiscard]]` by ISO rule, so `-Werror` cannot police it; `scripts/checks/check_tz_boundary_discard.py` therefore bans `(void)`-cast discards of `ra8_tz_secure_boot_*()` calls everywhere and of any `ra8_*()` call inside a boot TU (a `.c` defining `SystemInit` / `ra8_trustzone_init`). |
| 8 | Limit preprocessor use. | Compliant. C23 typed enums replace `#define` for constants; macros only for duplicated code, conditional compilation, build flags. |
| 9 | Restrict pointer use. | **Intentional deviation.** Function pointers are allowed for Dependency Inversion. |
| 10 | Compile clean with max warnings. | Compliant. `-Wall -Wextra -Werror -fshort-enums`; CI fails on any warning. |

## Backward compatibility (there is none)

This is a personal project with **zero backward-compatibility
requirements**. There will never be public releases or versioned APIs.

**Forbidden** (rejected in code review):

- Function aliases: `#define old_name new_name`
- Deprecation macros: `__attribute__((deprecated))`
- Wrapper functions for "compatibility"
- Comments like `// TODO: remove old API after migration`
- Keeping unused code "just in case"

**Required**:

- Update ALL call sites in the same commit when changing APIs.
- Delete old code immediately. No staged rollouts.
- Rename types, functions, fields freely to improve clarity.
- Main branch must build successfully.

## Character encoding

All source files **must be pure 7-bit ASCII** (Unicode 0x00..0x7F).
Applies to every `.c`, `.h`, `.cpp`, `.hpp`, `.cmake`, `.md`, `.yml`,
`.sh`, `.py` file -- including comments, documentation, and string
literals.

Rationale: multi-byte UTF-8 breaks downstream toolchains -- static
analysers, MISRA checkers, code-coverage tools, Windows IDEs, and
the embedded debugger console.

`scripts/git/pre-commit` rejects any commit containing non-ASCII in
source files.

## Ring and World tagging

See [`docs/RING_AND_WORLD.md`](RING_AND_WORLD.md) for the full
explanation. Short version: every Ring 3+ file gets a tag that
declares its architectural ring and which TrustZone world it expects
to run in:

```c
/**
 * @file ra8_glcdc.c
 * @brief Graphics LCD Controller driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 * ...
 */
```

`scripts/checks/check_world_tags.py` enforces it at commit time.
