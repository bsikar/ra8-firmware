# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Synthetic fixtures proving each finding fires -- and that none over-fires.

Both directions, deliberately.  A doc-attachment gate that cries wolf gets
switched off, and a switched-off gate is indistinguishable from a clean tree,
so every defect class here has a must-fire fixture AND the tricky-but-legal
form that must stay silent beside it.

Split out of the checker (#359): the fixture table is 600-odd lines of C, and
keeping it inline meant the rules it proves were unreachable by eye.
"""

from __future__ import annotations

import os
import sys
import tempfile
from pathlib import Path

from docattach_ast import (
    _check_declarations,
    _require_libclang,
    check_forward_decl_blocks,
)
from docattach_lex import check_banned_boilerplate, check_consecutive_blocks
from docattach_model import Finding

# ---------------------------------------------------------------------------
# Selftest
# ---------------------------------------------------------------------------
#: (name, source, expected finding codes).  Every defect class must be proven
#: to FIRE, and every tricky-but-legal form proven NOT to.  A gate that cries
#: wolf gets disabled, which is worse than no gate.
SELFTEST_CASES: list[tuple[str, str, set[str]]] = [
    # ---------------- must FIRE ----------------
    (
        "param name pasted from another function",
        """
/**
 * @brief Add two numbers.
 * @param[in] reader Open reader.
 * @param[in] tile   Tile index.
 * @return Sum.
 */
int add_values(int lhs, int rhs) { return lhs + rhs; }
""",
        {"DOC001", "DOC002"},
    ),
    (
        "defect behind an RA8_* annotation macro is still seen",
        """
#define RA8_INTERNAL
/**
 * @brief Emit one char-decl pair.
 * @param[in] conn_handle Connection handle.
 * @param[in] pdu         Raw PDU.
 * @return Nothing.
 */
RA8_INTERNAL
static void internal_emit_pair(unsigned char* resp, int pos) { (void)resp; (void)pos; }
""",
        {"DOC001", "DOC002"},
    ),
    (
        "correct block behind an RA8_* annotation macro does not fire",
        """
#define RA8_INTERNAL
/**
 * @brief Emit one char-decl pair.
 * @param[out] resp Response buffer.
 * @param[in]  pos  Cursor into resp.
 * @return Nothing.
 */
RA8_INTERNAL
static void internal_emit_pair(unsigned char* resp, int pos) { (void)resp; (void)pos; }
""",
        set(),
    ),
    (
        "param documented that does not exist",
        """
/**
 * @brief Add two numbers.
 * @param[in] lhs Left.
 * @param[in] rhs Right.
 * @param[in] carry Nonexistent.
 * @return Sum.
 */
int add_values(int lhs, int rhs) { return lhs + rhs; }
""",
        {"DOC001"},
    ),
    (
        "partially documented signature (1 of 3)",
        """
/**
 * @brief Download one chapter.
 * @param[in] url Source URL.
 * @return Status.
 */
int download_chapter(const char* url, const char* dest, int retries) { return 0; }
""",
        {"DOC002"},
    ),
    (
        "retval on a void function",
        """
/**
 * @brief Reset the widget.
 * @param[in] id Widget id.
 * @retval 0 Success.
 */
void widget_reset(int id) { (void)id; }
""",
        {"DOC003"},
    ),
    (
        "@return promising a value on a void function",
        """
/**
 * @brief Reset the widget.
 * @param[in] id Widget id.
 * @return The number of registers cleared.
 */
void widget_reset(int id) { (void)id; }
""",
        {"DOC003"},
    ),
    (
        "two doc blocks in a row (the ra8_viewer main/log_sink shape)",
        """
/**
 * @brief Program entry point.
 * @return 0 on success.
 */

/**
 * @brief Log sink.
 * @param[in] byte Byte to emit.
 */
static void log_sink(unsigned char byte) { (void)byte; }
""",
        {"DOC004"},
    ),
    (
        "identical block pasted twice",
        """
/**
 * @brief Compute the checksum.
 * @param[in] len Length.
 * @return Checksum.
 */
/**
 * @brief Compute the checksum.
 * @param[in] len Length.
 * @return Checksum.
 */
int checksum(int len) { return len; }
""",
        {"DOC004"},
    ),
    (
        "@fn naming a different function",
        """
/**
 * @fn viewer_open_comic
 * @brief Open an RTA1 atlas.
 * @param[in] r Reader.
 * @return Status.
 */
int viewer_open_rta1(int r) { return r; }
""",
        {"DOC005"},
    ),
    (
        "@struct naming a different struct",
        """
/**
 * @struct lcd_config_t
 * @brief Panel timing.
 */
struct panel_timing_t { int hsync; };
""",
        {"DOC005"},
    ),
    (
        "@enum naming a different enum",
        """
/**
 * @enum lcd_state_t
 * @brief Reader states.
 */
enum reader_state_t { k_idle = 0 };
""",
        {"DOC005"},
    ),
    (
        "definition-site 'Implementation of `X()`' naming a different function",
        """
/** @brief Implementation of `ra8_err_to_str()` -- linear-scan lookup. */
int ra8_err_to_code(int c) { return c; }
""",
        {"DOC005"},
    ),
    (
        "block on a forward declaration separated from its definition by other code",
        """
/**
 * @brief Probe and cache every tile's native size.
 * @param[in] r Reader.
 * @return Status.
 */
static int compute_tiles(int r);

/**
 * @brief Unrelated helper standing between the declaration and the body.
 * @param[in] v Value.
 * @return Value.
 */
static int passthrough(int v) { return v; }

static int compute_tiles(int r) { return r; }
""",
        {"DOC006"},
    ),
    (
        "-Wmissing-prototypes idiom: local prototype directly above its definition",
        """
/**
 * @brief Non-maskable interrupt handler.
 * @return Nothing.
 */
void NMI_Handler(void);
void NMI_Handler(void) { }
""",
        set(),
    ),
    (
        "banned pointer-only boilerplate",
        """
/** @brief Implementation of ra8_foo (see header for full contract). */
int ra8_foo(void) { return 0; }
""",
        {"DOC007"},
    ),
    (
        "two adjacent blocks are still caught when the gap is only whitespace",
        """
/**
 * Enable a build option that nothing below actually declares.
 */

/**
 * @brief Widget identifier width.
 */
#define WIDGET_ID_BITS 8
""",
        {"DOC004"},
    ),
    (
        "a real comment between two blocks does not license a duplicate",
        """
/**
 * Enable a build option that nothing below actually declares.
 */
/* an ordinary comment, not a commented-out directive */

/**
 * @brief Widget identifier width.
 */
#define WIDGET_ID_BITS 8
""",
        {"DOC004"},
    ),
    # ---------------- must NOT fire ----------------
    (
        "untagged block documenting a commented-out config option",
        """
/**
 * Enable the verified implementations of ECDH primitives from Project Everest.
 *
 * The Everest code is Apache-2.0 only, so enabling this is incompatible with
 * taking the library under GPL-2.0-or-later.
 */
//#define MBEDTLS_ECDH_VARIANT_EVEREST_ENABLED

/**
 * \\def MBEDTLS_GCM_LARGE_TABLE
 *
 * Use a larger GCM table to speed up AES-GCM.
 */
//#define MBEDTLS_GCM_LARGE_TABLE
""",
        set(),
    ),
    (
        "commented-out #undef also counts as the documented subject",
        """
/**
 * Disable the built-in entropy sources.
 */
// #undef MBEDTLS_ENTROPY_C

/**
 * @brief Widget identifier width.
 */
#define WIDGET_ID_BITS 8
""",
        set(),
    ),
    (
        "correct function block",
        """
/**
 * @brief Add two numbers.
 * @param[in] lhs Left operand.
 * @param[in] rhs Right operand.
 * @return The sum.
 * @retval 0 Both operands were zero.
 */
int add_values(int lhs, int rhs) { return lhs + rhs; }
""",
        set(),
    ),
    (
        "correct void function (no @return/@retval)",
        """
/**
 * @brief Reset the widget.
 * @param[in] id Widget id.
 */
void widget_reset(int id) { (void)id; }
""",
        set(),
    ),
    (
        "@file block directly above a symbol block",
        """
/**
 * @file demo.c
 * @brief Demo translation unit.
 */
/**
 * @brief Add two numbers.
 * @param[in] lhs Left.
 * @param[in] rhs Right.
 * @return Sum.
 */
int add_values(int lhs, int rhs) { return lhs + rhs; }
""",
        set(),
    ),
    (
        "@defgroup and @{ grouping markers between blocks",
        """
/**
 * @defgroup lcd LCD driver
 * @{
 */
/**
 * @brief Clear the framebuffer.
 * @return Status.
 */
int lcd_clear(void);
/** @} */
""",
        set(),
    ),
    (
        "@copydoc block with another symbol's parameter names",
        """
/** @copydoc ra8_gpio_output_init */
int ra8_gpio_output_init_impl(int port, int pin) { return port + pin; }
""",
        set(),
    ),
    (
        "sanctioned definition-site single-line form, correct name",
        """
/** @brief Implementation of `ra8_err_to_str()` -- linear-scan lookup. */
int ra8_err_to_str(int code) { return code; }
""",
        set(),
    ),
    (
        "undocumented parameters with no @param at all (doxy_audit's job, not ours)",
        """
/**
 * @brief Add two numbers.
 * @return Sum.
 */
int add_values(int lhs, int rhs) { return lhs + rhs; }
""",
        set(),
    ),
    (
        "@param inside a @code example naming other symbols",
        """
/**
 * @brief Register a handler.
 * @param[in] handler Callback.
 * @return Status.
 * @code
 * // @param[in] port Port identifier
 * ra8_isr_register(handler);
 * @endcode
 */
int ra8_isr_register(int handler) { return handler; }
""",
        set(),
    ),
    (
        "variadic function documenting only its named parameters",
        """
/**
 * @brief Formatted log.
 * @param[in] fmt Format string.
 * @return Bytes written.
 */
int ra8_logf(const char* fmt, ...) { (void)fmt; return 0; }
""",
        set(),
    ),
    (
        "forward declaration bare, definition documented (the correct shape)",
        """
static int compute_tiles(int r);

/**
 * @brief Probe and cache every tile's native size.
 * @param[in] r Reader.
 * @return Status.
 */
static int compute_tiles(int r) { return r; }
""",
        set(),
    ),
    (
        "header declaration documented, definition bare (CLAUDE.md's prescribed split)",
        """
/**
 * @brief Add two numbers.
 * @param[in] lhs Left.
 * @param[in] rhs Right.
 * @return Sum.
 */
int add_values(int lhs, int rhs);
""",
        set(),
    ),
    (
        "namesake statics in different files must not merge (keyed per file)",
        """
/**
 * @brief Zero a buffer.
 * @param[in] len Length.
 */
static void internal_zero_bytes(int len) { (void)len; }
""",
        set(),
    ),
    (
        "pointer-back note WITH a real implementation note is allowed",
        """
/** @brief Implementation of `ra8_foo()` -- O(1) table lookup, see HUM Ch 5.2. */
int ra8_foo(void) { return 0; }
""",
        set(),
    ),
    (
        "\\def block documenting a deliberately commented-out config option",
        """
/**
 * \\def MBEDTLS_AES_ROM_TABLES
 *
 * Use precomputed AES tables stored in ROM.
 */
//#define MBEDTLS_AES_ROM_TABLES

/**
 * \\def MBEDTLS_AES_FEWER_TABLES
 *
 * Use less ROM/RAM for AES tables.
 */
//#define MBEDTLS_AES_FEWER_TABLES
""",
        set(),
    ),
    (
        "@var block stranded above another symbol's block, real variable left bare",
        """
/**
 * @var g_release_err
 * @brief Captured release code.
 */
/** @brief Sentinel for the release code. */
typedef enum : unsigned {
  k_err_none = 0U, /**< None yet. */
} err_sentinel_t;

volatile unsigned g_release_err = 0U;
""",
        {"DOC004"},
    ),
    (
        "'@return This function never returns.' on a [[noreturn]] void handler",
        """
/**
 * @brief Park the core forever.
 * @return This function never returns.
 */
[[noreturn]] void park_forever(void) { for (;;) { } }
""",
        set(),
    ),
    (
        "@retval on a [[noreturn]] void handler is still a contradiction",
        """
/**
 * @brief Park the core forever.
 * @return This function never returns.
 * @retval (none) The core spins in place.
 */
[[noreturn]] void park_forever(void) { for (;;) { } }
""",
        {"DOC003"},
    ),
    (
        "house-style '@return Nothing.' on a void function is not a contradiction",
        """
/**
 * @brief Reset the widget.
 * @param[in] id Widget id.
 * @return Nothing.
 */
void widget_reset(int id) { (void)id; }
""",
        set(),
    ),
    (
        "typedef'd anonymous struct named by its @struct tag (the C23 house shape)",
        """
/**
 * @struct sim_args_t
 * @brief Parsed command line.
 */
typedef struct {
  int verbose; /**< Verbosity level. */
} sim_args_t;
""",
        set(),
    ),
    (
        "typedef'd anonymous enum named by its @enum tag",
        """
/**
 * @enum lcd_state_t
 * @brief Panel states.
 */
typedef enum : unsigned char {
  k_lcd_state_idle = 0, /**< Idle. */
} lcd_state_t;
""",
        set(),
    ),
    (
        "struct with correctly-named @struct tag and documented members",
        """
/**
 * @struct panel_timing_t
 * @brief Panel timing.
 */
struct panel_timing_t {
  int hsync; /**< Horizontal sync width. */
  int vsync; /**< Vertical sync width. */
};
""",
        set(),
    ),
]


def _findings_for(path: Path, cindex, args: list[str]) -> list[Finding]:
    """Every finding for one fixture, through the same code the gate runs.

    Deliberately the production helpers rather than a walk of its own. This
    used to re-implement ``check_file``'s cursor loop inline, which meant the
    suite could keep passing while the code the gate actually runs drifted
    away from it -- proving the fixtures against a second implementation
    nobody ships.
    """
    rel = str(path)
    text = path.read_text(encoding="ascii")
    own = os.path.realpath(str(path))
    tu = cindex.Index.create().parse(str(path), args=args)
    return [
        *check_consecutive_blocks(rel, text),
        *check_banned_boilerplate(rel, text),
        *_check_declarations(tu, cindex, rel, own, text),
        *check_forward_decl_blocks(tu, cindex, rel, own, text),
    ]


def selftest() -> int:
    """Run the synthetic fixtures in both directions."""
    cindex = _require_libclang()
    args = ["-std=c23", "-x", "c", "-DRA8_HOST_BUILD=1"]
    failures = 0
    with tempfile.TemporaryDirectory() as td:
        for idx, (name, src, expected) in enumerate(SELFTEST_CASES):
            path = Path(td) / f"case_{idx:02d}.c"
            path.write_text(src, encoding="ascii")
            got = _findings_for(path, cindex, args)

            codes = {f.code for f in got}
            if codes != expected:
                failures += 1
                sys.stderr.write(
                    f"  FAIL [{idx:02d}] {name}\n"
                    f"       expected {sorted(expected) or '<clean>'}\n"
                    f"       got      {sorted(codes) or '<clean>'}\n"
                )
                for f in got:
                    sys.stderr.write(f"         {f.code} {f.symbol}: {f.detail}\n")

    if failures:
        sys.stderr.write(
            f"check_doc_attachment.py: selftest FAILED ({failures}/{len(SELFTEST_CASES)} cases).\n"
        )
        return 2
    fires = sum(1 for _, _, e in SELFTEST_CASES if e)
    clean = len(SELFTEST_CASES) - fires
    print(
        f"check_doc_attachment.py: selftest passed "
        f"({len(SELFTEST_CASES)} cases: {fires} must-fire, {clean} must-not-fire)."
    )
    return 0
