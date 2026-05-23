# CLAUDE.md <!-- AI-OK: filename self-reference -->

This file provides guidance to Claude Code when working with code in this repository. <!-- AI-OK: self-reference to Claude Code -->

> **For human readers:** the authoritative style guide is
> [`docs/STYLE_GUIDE.md`](docs/STYLE_GUIDE.md) and the architectural-ring +
> TrustZone-world tagging system is documented in
> [`docs/RING_AND_WORLD.md`](docs/RING_AND_WORLD.md). This file restates the
> most-violated rules in a form an AI assistant can act on, but the
> human-facing docs are the source of truth.

---

## Target Hardware

| Item | Value |
|------|-------|
| **MCU** | Renesas R7KA8D2KFLCAC (RA8D2 group) |
| **Primary Core** | Arm Cortex-M85 @ 1 GHz (with Helium / MVE) |
| **Secondary Core** | Arm Cortex-M33 @ 250 MHz |
| **Code Memory** | 1 MB MRAM (non-volatile) |
| **System RAM** | 2 MB SRAM with ECC |
| **Package** | 289-pin BGA (12 mm x 12 mm, 0.65 mm pitch) |
| **Board** | EK-RA8D2 with 7.0-inch 1024x600 parallel TFT, OV5640 5 MP camera |
| **External Memory** | 64 MB Octo-SPI flash, 64 MB SDRAM |
| **Debugger** | On-board SEGGER J-Link OB (SWD/JTAG) |
| **Toolchain** | ARM GNU Toolchain (arm-none-eabi-gcc) + CMake |
| **RTOS** | None (bare-metal + custom HAL). A hand-written RTOS may be introduced later. |

## Development Approach

- **Bare-metal** with a hand-written HAL layered on top of the chip's register map, the same way `star-rx72n-firmware` was built for the RX72N.
- **No Renesas FSP code in this tree.** FSP headers and app notes may be used as **reference material** when writing the HAL, but every source file in `src/` and `libs/` is hand-written under the rules below.
- **Zero vendor IDE artifacts.** This repo is built from the command line with CMake + arm-none-eabi-gcc. e2 studio, IAR, Keil project files are NOT checked in.
- **Reference material lives in `docs/reference/`**: the RA8D2 datasheet, Hardware User's Manual, technical brief, and high-temperature-operation app note are committed so they are always at hand.

### Useful External Resources

These are **reference-only** -- do not copy code from them into this repo without rewriting under this project's style rules.

- Renesas FSP source (BSP, register headers, drivers): https://github.com/renesas/fsp
- Renesas FSP BSP MCU documentation: https://renesas.github.io/fsp/group___b_s_p___m_c_u.html
- FSP product page: https://www.renesas.com/en/software-tool/ra-flexible-software-package-fsp
- RA8D2 product page: https://www.renesas.com/en/products/ra8d2
- Keil CMSIS DFP for RA: https://www.keil.arm.com/packs/ra_dfp-renesas/versions/

---

## Quick Reference Commands

- **Build default app**: `make` (blink)
- **Build specific app**: `make <app>` (e.g., `make blink_hal`)
- **List discovered apps**: `make apps`
- **Run host unit tests**: `make test`
- **Check MC/DC coverage**: `make mcdc`
- **Code formatter**: `make format` (apply) or `make check` (dry run)
- **Run linter (clang-tidy)**: `make tidy`
- **Generate Doxygen docs**: `make docs`
- **Pre-commit validation**: `make ascii` (encoding check), `make version` (check @since tags)

---

## Subagents & Swarms

This repository utilizes specialized custom project subagents under `.claude/agents/` <!-- AI-OK: reference to .claude directory --> to perform focused, token-efficient audits. These reviewers are configured for specific compliance checking, allowing the main agent to delegate verification tasks:

- **Code Style Compliance (`@style-reviewer`)**:
  - **Purpose**: Audits C23 syntax rules, Doxygen tag completeness, non-inclusive terminology replacements, and header guards.
  - **When to Trigger**: Whenever a new or modified C source file (`.c`), header (`.h`), or style documentation is written. Always invoke prior to submitting code to prevent CI check failures.
  - **Scope**: Focused strictly on code structure and syntax layout. Uses the fast and token-efficient `haiku` model with read-only tools.
- **Safety & MC/DC Compliance (`@safety-reviewer`)**:
  - **Purpose**: Audits safety compliance (DO-178C Level B), compound boolean decision MC/DC test vector coverage, SOLID design principles, and NASA Power of 10 rules.
  - **When to Trigger**: On any modification to core logic, state machines, control flow, or host unit tests under `tests/`.
  - **Scope**: Audits logic structures, loop bounds, return value validation, and test adequacy. Uses the powerful `sonnet` model and is equipped with the `Bash` tool to run tests and coverage checks via `make test` or `make mcdc`.
- **HUM Citations Validation (`@citation-reviewer`)**:
  - **Purpose**: Meticulously audits direct register accesses to verify that each is immediately preceded by a valid Hardware User's Manual (HUM) citation, and strictly bans in-tree line-number citations.
  - **When to Trigger**: On any modification to register structures, inline register accessors, or HAL drivers interacting with MMIO (e.g. under `libs/ra_hal/`).
  - **Scope**: Checks for properly formatted `/* HUM Ch ... */` comments. Uses the `haiku` model and has `Bash` access to run the global verification script (`python3 scripts/utils/cite_check.py --strict`).

### Agent Collaboration Protocol

When acting as the main agent, you should collaborate with the subagent swarm using the following guidelines:

1. **Selective Delegation**: When a logical block is written or updated, invoke the corresponding subagent (e.g., using the `invoke_subagent` tool or prompting the user) pointing to the specific file paths.
2. **Actionable Feedback**: Request the subagents to detail specific violations with file names, line numbers, and complete drop-in diffs.
3. **Iterative Remediation**: Refine the code iteratively based on the subagent reviews. Do not consider a task complete until all relevant subagents confirm 100% compliance.
4. **Ascii & No-AI Constraints**: Maintain pure 7-bit ASCII encoding and zero AI attribution across all agent prompts, comments, and workspace additions.

---

## Terminology Standard

**IMPORTANT:** This project uses inclusive terminology following OSHWA standards:

- **Controller/Peripheral** - NOT master/slave (I2C, SPI, 1-Wire)
- **COPI/CIPO** - NOT MOSI/MISO (Controller Out Peripheral In / Controller In Peripheral Out)
- **CS (Chip Select)** - NOT SS (Slave Select). Use "CS" for SPI chip select signals and "Chip Select" in documentation.
- **Primary/Main** - NOT master (for configuration structures)

Note: External APIs and Renesas reference documents may still use legacy terminology. Map these to our terminology in comments when integrating.

---

## Backward Compatibility Policy

**IMPORTANT:** This is a personal project with **ZERO backward compatibility requirements**. There will never be public releases or versioned APIs.

### Core Principles

1. **Breaking changes are ENCOURAGED** - If it improves code quality, refactor immediately
2. **No compatibility layers** - Delete old code, update all call sites in the same change
3. **Main branch must work** - The only requirement is that main remains in a working state
4. **No gradual transitions** - No deprecation warnings, no compatibility shims, no aliases

### What This Means in Practice

**FORBIDDEN (will be rejected in code review):**
- Function aliases: `#define old_name new_name`
- Deprecation macros: `__attribute__((deprecated))`
- Wrapper functions for "compatibility"
- Comments like `// TODO: Remove old API after migration`
- Keeping unused code "just in case"

**REQUIRED:**
- Update ALL call sites in the same commit when changing APIs
- Delete old code immediately - no staged rollouts
- Rename types, functions, fields freely to improve clarity
- Ensure main branch builds successfully

### Examples

**WRONG - Don't do this:**
```c
// WRONG - No backward compatibility shims!
#define lcd_clear lcd_framebuffer_clear  // Just update call sites
ra_err_t lcd_clear(void) __attribute__((deprecated));  // Delete it

// WRONG - Don't keep old implementations
ra_err_t old_uart_send(const uint8_t* data) {  // Delete and update callers
    return new_uart_send(data);
}
```

**CORRECT - Do this:**
```c
// CORRECT - Just rename the function and update all call sites
ra_err_t lcd_framebuffer_clear(void);

// CORRECT - If renaming a type, update all references
typedef struct {
    uint32_t pixel_clock_hz;  // Renamed from 'clock'
} lcd_config_t;
```

---

## Character Encoding Policy

**MANDATORY:** ALL source files in this project must use **pure 7-bit ASCII only** (Unicode code points U+0000-U+007F). This applies to every `.c`, `.h`, `.cpp`, `.hpp`, `.cmake`, `.md`, `.yml`, `.sh`, `.py` file -- including comments, documentation, and string literals.

**Rationale:** Multi-byte UTF-8 characters break downstream toolchains (static analyzers, MISRA checkers, code coverage tools, Windows IDEs, and the embedded debugger console).

### Enforcement

A pre-commit hook at `scripts/git/pre-commit` rejects any commit containing non-ASCII characters in source files. CI will also run the check.

---

## Git Commits and Pull Requests

**Do not add AI attribution to commits or PRs.** Write natural commit messages and PR descriptions without any footers suggesting automated generation. Keep messages clean and professional.

---

## AI Attribution Policy

**Zero AI attribution anywhere in the codebase.** No file in `libs/`, `src/`, `tests/`, `examples/`, `port/`, `scripts/`, or `docs/` may reference any AI-tool attribution as the author, reviewer, or contributor of code, tests, docs, or any artifact. <!-- AI-OK: policy description -->

Forbidden patterns include:
- Comments citing an assistant as a reviewer
- Co-Authored-By or generated-by footers in code comments <!-- AI-OK: policy description -->
- Author lines naming an AI assistant

The pre-commit gate `scripts/utils/check_no_ai_attribution.py` enforces this strictly. See `docs/AI_ATTRIBUTION_POLICY.md` for the full rules.

---

## Summary Documents

**Do not create summary documents, integration summaries, or completion reports unless explicitly requested by the user.** This includes files like `INTEGRATION_SUMMARY.md`, `COMPLETION_REPORT.md`, test scripts, or similar documentation. Only create these if the user specifically asks for them.

---

## Coding Rules & C23 Standards

- **C23 Syntax**: Use `bool`, `true`, and `false` directly. Do NOT include `<stdbool.h>`. Use `static_assert` directly without `_Static_assert` or `<assert.h>`. Zero-initialize structs/arrays with `= {}` (never `= {0}`).
- **C23 Typed Enums**: Every enum MUST specify an explicit underlying type (`typedef enum : uint8_t { ... } name_t;`). Select the smallest fitting type. Use `uintptr_t` for register base addresses. NO macros for integer constants.
- **Header Guards**: Use `#pragma once` at the top of headers. DO NOT use traditional include guards.
- **Function Validation**: Minimum 2 validation checks (preconditions and postconditions) per function (NASA Power of 10 Rule 5). Use `RA_CHECK_NULL_PTR` from `ra_check.h` for null guards.

### Constants and Macros (RA8D2 C Firmware)

**Strict preference hierarchy:**

1. **Enums** - ALWAYS use for ALL integer constants
   ```c
   // CORRECT: C23 typed enums with explicit underlying type (MANDATORY)
   typedef enum : uint8_t {
     k_lcd_state_idle      = 0,
     k_lcd_state_refreshing = 1,
     k_lcd_state_error      = 2,
   } lcd_state_t;

   typedef enum : uint16_t {
     k_timeout_ms  = 1000,    // Integer constant -> enum
     k_max_retries = 3,       // Integer constant -> enum
   } lcd_config_t;

   // WRONG: Untyped enum (no underlying type specified)
   typedef enum {
     k_lcd_state_idle = 0,  // Missing `: uint8_t`
   } lcd_state_t;

   // WRONG: Never use macros for integer constants
   #define TIMEOUT_MS (1000)  // Should be enum!
   ```

   **C23 Typed Enum Requirements (MANDATORY for this firmware):**
   - ALL enums MUST specify an explicit underlying type using C23 syntax
   - Syntax: `typedef enum : <type> { ... } name_t;`
   - Choose the smallest type that fits all values:
     - `uint8_t` - Values 0-255 (most common: states, indices, small constants)
     - `uint16_t` - Values 256-65535 (timeouts in ms, medium constants)
     - `uint32_t` - Values > 65535 (large constants, bit masks -- NOT addresses)
     - `uintptr_t` - Hardware register base addresses (MANDATORY for all address enums)
     - `int8_t`, `int16_t`, `int32_t` - For signed values
   - Use `uintptr_t` for any enum whose values are hardware memory-mapped addresses. On the 32-bit RA8D2 target `uintptr_t` == `uint32_t`, but on the 64-bit x86_64 unit-test host `uintptr_t` == `uint64_t`. Using `uint32_t` for addresses silently truncates on the test host and produces wrong pointer casts.
   - This ensures predictable size, ABI stability, and debugger compatibility

2. **const variables** - ONLY for floating-point (enum limitation)
   ```c
   // CORRECT: Floating-point must use const (can't use enum)
   static const float s_pixel_clock_mhz = 51.2F;
   static const float s_pid_kp = 1.0F;

   // WRONG: Never use macros for floats
   #define PIXEL_CLOCK_MHZ (51.2F)  // Should be const!
   ```

3. **Macros** - ONLY for these 3 specific cases:
   ```c
   // ALLOWED: Reducing duplicated code
   #define RA_RETURN_ON_ERROR(err, tag, msg) \
       do { \
           ra_err_t _err = (err); \
           if (_err != k_ra_ok) { \
               ra_log_error((tag), (msg)); \
               return _err; \
           } \
       } while (0)

   // ALLOWED: Conditional compilation (optimization)
   #if LOG_LEVEL >= k_log_error
   #define ra_log_error(tag, msg) internal_ra_log_error((tag), (msg))
   #else
   #define ra_log_error(tag, msg) ((void)0)
   #endif

   // ALLOWED: Build configuration flags
   #ifdef __ARM_ARCH_8_1M_MAIN__
   #define RA_HAS_MVE
   #endif

   // FORBIDDEN: Hardware register addresses (use inline accessors)
   #define IOPORT_BASE ((ra_ioport_regs_t*)0x40080000)  // Wrong!
   #define IOPORT      (*IOPORT_BASE)                    // Wrong!

   // FORBIDDEN: Backward compatibility
   #define old_function new_function  // Wrong! Update call sites instead
   ```

4. **Hardware Register Access** - Use inline accessor functions:
   ```c
   // CORRECT: Inline accessor with typed enum address
   typedef enum : uintptr_t {
       k_ioport_base_addr = 0x40080000,
       k_sci0_base_addr   = 0x40118000,
   } hw_addresses_t;

   typedef enum : uint8_t {
       k_bit_led = 7,
   } gpio_bits_t;

   static inline volatile ra_ioport_regs_t* ioport(void) {
       return (volatile ra_ioport_regs_t*)k_ioport_base_addr;
   }

   static inline volatile ra_sci_regs_t* sci0(void) {
       return (volatile ra_sci_regs_t*)k_sci0_base_addr;
   }

   // Usage: Same syntax as macro approach
   sci0()->TDR = 0x42;
   ioport()->PODR[1] |= (1U << k_bit_led);
   ```

### No Magic Numbers (RA8D2 C Firmware)

**ZERO TOLERANCE for magic numbers.** ALL numeric literals must be named typed enums, including:

```c
// CORRECT: Array indices as typed enums
typedef enum : uint8_t {
    k_idx_high_byte = 0,
    k_idx_low_byte  = 1,
} be16_byte_idx_t;

buf[k_idx_high_byte] = (val >> k_shift_byte);

// CORRECT: Bit shifts as typed enums
typedef enum : uint8_t {
    k_shift_byte   = 8,
    k_shift_enable = 7,
} bit_shifts_t;

// CORRECT: Protocol offsets as typed enums
typedef enum : uint8_t {
    k_offset_sync    = 0,
    k_offset_payload = 4,
} frame_offsets_t;

// CORRECT: Bit masks as typed enums (use uint32_t for masks)
typedef enum : uint32_t {
    k_mask_byte   = 0xFF,
    k_mask_enable = 0x80,
} bit_masks_t;

// WRONG: Magic numbers
buf[0] = (val >> 8);              // What is 0? What is 8?
frame[4] = payload;               // What's at index 4?
REG = (1 << 7) | (3 << 3);        // Which bits? Why?
```

- Always use braces for control statements.
- Use `assert()` for programming errors only, not runtime errors.
- Avoid inline ASM; if required, use `volatile` and document why.

---

## Doxygen Documentation Requirements

Every file, function (public or static), struct, enum, and macro must be documented with comprehensive Doxygen comments:

### Documentation Policy

This project enforces **MAXIMUM documentation coverage** with zero tolerance for undocumented code:

1. **Every file** must have complete file-level documentation
2. **Every function** must use ALL applicable Doxygen tags
3. **Every struct/enum** must document ALL members
4. **Every variable** (global, static, member) must be documented
5. **Every typedef** must have full documentation
6. **Every macro** must be documented with usage examples

### Required Tags by Code Element

**Functions - Minimum Required Tags:**
- `@brief` - One-line summary
- `@details` - Multi-paragraph explanation with algorithm description
- `@param[in/out/in,out]` - ALL parameters with direction, valid range, units, constraints
- `@return` - Return value description
- `@retval` - EVERY possible return value documented individually
- `@pre` - Preconditions (minimum 2 per NASA Rule 5)
- `@post` - Postconditions (minimum 2 per NASA Rule 5)
- `@note` - Thread safety statement
- `@code` - Usage example (if non-trivial)
- `@see` - Cross-references to related functions
- `@since` - Version introduced

**Structs/Enums - Minimum Required Tags:**
- `@struct/@enum` - Structure/enumeration tag
- `@brief` - One-line summary
- `@details` - Detailed explanation
- `/**<` - Inline comment for EVERY member/value with full explanation
- `@invariant` - Constraints on fields
- `@code` - Usage example
- `@see` - Related types

**Variables - Minimum Required Tags:**
- `@var/@def` - Variable/macro tag
- `@brief` - One-line summary
- `@details` - Purpose and usage
- `@note` - Access restrictions
- `@warning` - Direct modification warnings (for static variables)
- `@since` - Version introduced

**State Machines - Required Documentation:**
- `@startuml` state diagram showing all transitions
- Each state documented with entry/exit actions
- Transition conditions and guards
- State transition table in `@par` section

### Example: Complete Function Documentation

```c
/**
 * @brief Configure an RA8D2 PORT pin as a digital output
 *
 * @details
 * Writes to the IOPORT PFS (Pin Function Select) register to put the pin in
 * "general-purpose output" mode and clears the output latch. Must be called
 * from a single-threaded context during system init or with IRQs masked.
 *
 * @param[in] port Port identifier (k_ra_port_0 .. k_ra_port_11)
 * @param[in] pin  Pin index within the port (0..15)
 * @param[in] init_level Initial output level (k_ra_level_low / k_ra_level_high)
 *
 * @return ra_err_t Error code
 * @retval k_ra_ok Success, pin configured and driven to init_level
 * @retval k_ra_err_invalid_arg port or pin out of range
 * @retval k_ra_err_pin_conflict pin already owned by another peripheral
 *
 * @pre Power to the IOPORT module is on (MSTPCRB cleared for IOPORT)
 * @pre Caller holds the pin validator lock (single-threaded init context OK)
 * @post Pin is driven to init_level
 * @post PFS register locked after write
 *
 * @invariant IOPORT.PWPR.B0WI remains 1 outside the critical section
 *
 * @note Not thread-safe, caller must provide synchronization
 * @warning PFS writes without unlocking PWPR are silently dropped
 *
 * @par Example:
 * @code
 * ra_gpio_output_init(k_ra_port_1, 7, k_ra_level_high); // LED on
 * @endcode
 *
 * @see ra_gpio_write()  Drive an already-configured output
 * @see ra_gpio_input_init()  Configure as input instead
 *
 * @since Version 1.0.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 5: 3 preconditions, 2 postconditions
 */
ra_err_t ra_gpio_output_init(ra_port_t port, uint8_t pin, ra_level_t init_level);
```

---

## Architectural Annotations

The `RA_*` annotation macros in `libs/ra_core/inc/ra_attributes.h` record architectural and safety contracts on individual functions. They expand to `__attribute__((annotate("...")))` under clang and to a comment-only no-op under other toolchains.

| Macro | One-line purpose |
|-------|------------------|
| `RA_TEST_HELPER` | Externally-linked symbol callable only from `tests/`. |
| `RA_INTERNAL` | Marker that a function is intended to be `static`. |
| `RA_PRIV` | Module-private helper shared across TUs in one library. |
| `RA_DI_SLOT(role)` | Explicit Dependency Injection seam (mock required). |
| `RA_NSC_VENEER` | TrustZone S/NS entry-point veneer in `libs/ra_nsc/`. |
| `RA_HW_REGISTER_ACCESS` | Inline MMIO accessor returning a `volatile` pointer. |
| `RA_NASA_RULE_3_OK` | Documented exception to NASA P10 Rule 3 (dynamic alloc). |
| `RA_MCDC_DEACTIVATED(reason)` | MC/DC deactivation; reason text gated by citation policy. |
| `RA_MAX_STACK(bytes)` | Per-function stack-frame budget (cross-checked via `.su`). |
| `RA_ISR_SAFE` | Function is callable from interrupt context. |
| `RA_EXPECTS_LOCK(name)` | Caller must hold the named lock on entry. |
| `RA_HOST_FRIENDLY` | Safe under `RA_SIMULATOR_MODE` (no unmocked MMIO). |
| `RA_LATENCY_BUDGET_NS(n)` | Real-time WCET deadline in nanoseconds. |
| `RA_NO_RECURSION` | NASA P10 Rule 1: no direct or indirect self-call. |
| `RA_BOUNDED_LOOP(symbol)` | NASA P10 Rule 2: every loop bounded by `symbol`. |
| `RA_VALIDATES(n)` | NASA P10 Rule 5: at least `n` `RA_CHECK_*` calls. |
| `RA_OWNS_RESOURCE(kind)` | RAII-style ownership; release on every return path. |
| `RA_REVIEWED_BY(name)` | Safety-critical reviewer sign-off (rolled into SVR). |
| `RA_REGISTER_BANK(peripheral)` | Group MMIO accessors by parent peripheral. |

---

## External HUM Citations Policy

- **Register Citations**: Every single register read/write or access MUST have an external Hardware User's Manual (HUM) citation comment immediately above it:
  `/* HUM Ch X.Y "section name" p NNNN */` (or `p NNNN-MMMM`)
- **In-tree Citations Banned**: Never cite files in this repository by line number (e.g., `libs/foo.c:123` is forbidden). Reference the function or symbol name instead. <!-- CITES-OK: literal example of the banned pattern documenting the rule itself -->

### Manual review checklist

Pre-commit hooks catch most citation drift, but not all of it. Before pushing, scan your own diff for the following and fix them locally:
- `git diff | grep -E '\.[ch]:[0-9]+'` -- catches `file:line` anchors in source comments and docs.
- Stale doc cross-references: search for any heading slug, function name, or filename you renamed in this change and confirm every Markdown link still resolves.
- TODO / FIXME / `WARN_ONLY_MODE` flags whose work you just completed -- delete the marker.

---

## NASA Power of 10 Rules

The project follows NASA/JPL Power of 10 rules for safety-critical embedded code with one intentional deviation for testability.

- **Rule 1 (Control Flow)**: No `goto`, `setjmp`/`longjmp`, or recursion.
- **Rule 2 (Loop Bounds)**: All loops have statically provable bounds.
- **Rule 3 (Memory)**: Zero dynamic memory after initialization (zero malloc/free in firmware).
- **Rule 4 (Length)**: Keep functions short (~60 lines). Enforced by clang-tidy.
- **Rule 5 (Validation)**: Minimum 2 validation checks per function (preconditions and postconditions).
- **Rule 6 (Data Scope)**: Declare data at the smallest possible scope.
- **Rule 7 (Return Values)**: Check all return values of non-void functions.
- **Rule 8 (Preprocessor)**: Limit preprocessor use (C23 enums for constants).
- **Rule 9 (Pointers - Deviation)**: Function pointers are **ALLOWED** to enable Dependency Inversion Principle (DIP) and mock-injection for host unit-testing.
- **Rule 10 (Warnings)**: Compile with maximum warnings (`-Wall -Wextra -Werror`).

---

## Safety / MC/DC (DO-178C Level B)

- **Target Certification**: Targets **IEC 61508 SIL 3** and **DO-178C Level B** safety bar.
- **MC/DC Coverage**: All compound boolean decisions must have MC/DC vectors in the unit tests.
- **Independent Influence**: Demonstrate that each condition independently affects the outcome (N+1 test cases).
- **Documentation**: State the MC/DC vector pattern in the test's Doxygen `@par MC/DC:` block.
- **Exempt Code**: `libs/third_party/` (SOUP components) is exempt from MC/DC re-test in this repo. Component justifications live under `docs/SOUP/`.

### Example MC/DC Test Block

```c
/**
 * @test ra_isr_register_validates_inputs
 *
 * @par MC/DC:
 * Decision: `if (handler == nullptr || priority > k_ra_isr_prio_max)` (2 conditions)
 * - Vector 1: handler=valid, priority=0       -> false (control: both conditions false)
 * - Vector 2: handler=NULL,  priority=0       -> true  (varies handler only)
 * - Vector 3: handler=valid, priority=255     -> true  (varies priority only)
 * Vectors 1+2 prove handler independently affects outcome; 1+3 prove the
 * same for priority. N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 */
TEST(ra_isr, register_validates_inputs) { ... }
```

---

## SOLID Principles for C

- **Single Responsibility (S)**: One module = one purpose; one function = one action.
- **Open/Closed (O)**: Modules configured via configuration structures passed to `*_init()` functions.
- **Liskov Substitution (L)**: Interface implementations (e.g. buses) are completely interchangeable.
- **Interface Segregation (I)**: Small, focused interfaces instead of fat interfaces.
- **Dependency Inversion (D)**: High-level modules do not depend on low-level details. Use function pointer structures for interfaces.

### Test access to internal symbols (MC/DC scope)

Mock injection (DIP) is preferred, but some validation paths can only be reached by calling internal symbols.
- A `static` helper that needs MC/DC test access should be promoted to TU-external linkage (drop `static`) and forward-declared in the module's `_internal.h`.
- The `_internal.h` declaration must carry a `@par MC/DC:` Doxygen note.
- Production callers must keep using the public API; the only consumers of the promoted symbol outside the defining TU are tests under `tests/`.

---

## Repository Layout

```
ra8d2-firmware/
  CMakeLists.txt               Top-level CMake -- auto-discovers examples/<tier>/.../<app>/ dirs
  Makefile                     Top-level shorthand: `make <app>` / `make apps`
  cmake/
    toolchain-ra8d2.cmake      arm-none-eabi cross-compile settings
  examples/
    ek_ra8d2/                  Stock EK-RA8D2 evaluation kit (no extra HW)
      hw_validated/            Apps confirmed working on a stock EVM
        smoke/                 No-UART smoke tests (e.g. blink, blink_hal)
        uart/                  Apps that print over SCI UART
        manual/                Apps needing manual jumper / button steps
      hw_pending/              Apps written but not yet HW-validated
    _unsupported/              Apps needing external hardware (motor, audio CODEC, ...)
    <tier>/.../<app>/          Each app dir contains:
      main.c                   Application entry
      vector_table.c           Per-app vector table + Reset_Handler
      system_init.c            Per-app SystemInit
      secure_exception.c       Per-app SecureFault handler
      trustzone_init.{c,h}     Per-app SAU bring-up
      linker_script.ld         Per-app memory map (may diverge from sibling apps)
      CMakeLists.txt           Per-app cmake target
      Makefile                 Per-app `make` (configure + build via cmake)
      README.md
  src/                         Shared internals (no boot code, no main)
    inc/                       Internal headers shared between TUs
    secure_app/                Ring 5 secure-side substrate (key vault)
  libs/                        Hand-written libraries
    ra_core/                   ra_err, ra_check, ra_log, ra_assert, ...
    ra_hal/                    Peripheral drivers + register header files
    ra_nsc/                    TrustZone NSC veneers
    ra_net_pal/, ra_usb_pal/   Platform abstraction layers
  tests/                       Host-side unit tests (standard gcc/clang, not cross-compiled)
  scripts/
    flash.sh                   Takes a .hex path argument; per-app Makefiles call it
    ozone.sh                   Takes an .elf path argument
    debug.sh                   Takes an .elf path argument
    format_code.sh             clang-format wrapper (auto-discovers app dirs)
    clang_tidy.sh              clang-tidy wrapper (auto-discovers app dirs)
    git/
      pre-commit               Pre-commit hook (ASCII, format, tidy, C23 patterns)
    utils/                     check-since-version, cite_check, check_world_tags, ...
  docs/
    reference/                 Committed datasheets and manuals (PDFs)
  .github/workflows/           CI
  .clang-format                Formatter config (copied verbatim from STAR rx72n)
  .clang-tidy                  Linter config (naming, NASA Rule 4 thresholds)
  .clangd                      Editor integration (strips ARM flags clangd can't parse)
  .cppcheck-suppressions       MISRA deviation justifications
  .gitignore
  .gitattributes
  .editorconfig
  LICENSE.txt                  MIT, Copyright (c) 2026 Brighton Sikarskie
  CLAUDE.md                    This file <!-- AI-OK: reference to CLAUDE.md -->
  README.md
```

### Adding a new application

Create a new directory `examples/<tier>/.../<newapp>/` containing:
1. `main.c` -- the application entry.
2. The five per-app boot files copied from a sibling app (`vector_table.c`, `system_init.c`, `secure_exception.c`, `trustzone_init.c`, `trustzone_init.h`). Update each `@file` to the new path.
3. `linker_script.ld` (also copied; may diverge later).
4. `CMakeLists.txt` and `Makefile` (copy from a sibling and update the `RA_APP_NAME` / `APP` variable).

The next `make` from the repo root re-disovers it -- no changes needed to the top-level `CMakeLists.txt` or top-level `Makefile`.

---

## Key Reference Documents

Committed under `docs/reference/`:

- `ra8d2-datasheet.pdf` (R01DS0493EJ) - electrical specs, pin lists, feature summary
- `ra8d2-hardware-user-manual.pdf` (R01UH1065EJ) - **primary** register reference; use this when writing HAL code
- `ra8d2-technical-brief.pdf` (R01TB0104EJ) - high-level overview
- `ra8d2-high-temperature-operation.pdf` (R01AN8060EJ) - application note

**IMPORTANT:** Always reference the **Hardware User's Manual** (`r01uh1065ej0130-ra8d2.pdf`) when writing register-level code. Page numbers and section references in commit messages should cite this manual.
