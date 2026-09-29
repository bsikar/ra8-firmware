/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */

/**
 * @file ra8_ns_rot_header.c
 * @brief Emits the Non-Secure image's ::ra8_ns_rot_header_t from C.
 *
 * @details
 * The RoT header used to be hand-assembled in the NS linker scripts as a pair
 * of ``LONG()`` directives, which meant the magic word was written out twice in
 * hex (once per script) with no compiler anywhere checking it against
 * ::k_ra8_tz_ns_rot_header_magic or against the shape of ::ra8_ns_rot_header_t.
 * A field reordered in the struct, or a digit fumbled in either script, linked
 * fine and only showed up as a Secure verifier that default-denies the BLXNS.
 *
 * Emitting the record from C closes that: ``magic`` now comes from the same
 * enum the verifier compares against, and the layout is the struct itself.
 *
 * The length cannot be: ``body_len`` is only knowable once the image is laid
 * out, so the linker script still computes it, publishes it as the absolute
 * symbol ``g_ra8_ls_ns_signed_body_len``, and this translation unit takes that
 * symbol's *address* as the value. That is the ordinary linker-defined-symbol
 * idiom, and the relocation resolves at final link.
 *
 * This file lives outside ``src/`` deliberately. ``ra8_add_app()`` globs every
 * C file under ``libs/<lib>/src`` into whatever links the library, and four
 * apps name ``ra8_tz_secure_boot`` in LIBS for their SECURE image. A
 * ``.ns_rot_header`` section in the Secure ELF is an orphan its script never
 * places, so the NS half of this library is kept where the glob cannot reach
 * it and is named explicitly by the NS link instead.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_tz_secure_boot.h"

/**
 * @var g_ra8_ls_ns_signed_body_len
 * @brief Linker-defined: signed body length in bytes, carried as an address.
 *
 * @details
 * Defined by the NS linker script (generated from ``ns_image.ld.in``, one
 * template for both the SRAM-run and execute-in-place layouts) as an
 * absolute symbol whose
 * VALUE is the byte count, so the object taken here is the length itself and
 * never something to dereference. Declared as an incomplete array so no size is
 * implied and no load is ever emitted.
 *
 * @warning Never read through this symbol. Only its address carries meaning.
 * @since 0.1.0
 */
extern const uint8_t g_ra8_ls_ns_signed_body_len[];

/**
 * @var g_ra8_ns_rot_header
 * @brief The 8-byte RoT record at ``ns_base + k_ra8_tz_ns_rot_header_offset``.
 *
 * @details
 * Placed by the NS linker script, which pins ``.ns_rot_header`` immediately
 * after the 16-slot NS vector table and ASSERTs that placement. The Secure
 * verifier reads it there to learn the signed body length without a hand-coded
 * trailer address; ``scripts/secrets/rot_sign.py`` appends the
 * ::ra8_rot_trailer_t at ``ns_base + body_len``.
 *
 * @invariant Lands at exactly ``ns_base + 0x40``; the linker script asserts it.
 * @since 0.1.0
 */
[[gnu::section(".ns_rot_header"), gnu::used]]
const ra8_ns_rot_header_t g_ra8_ns_rot_header = {
  .magic    = (uint32_t)k_ra8_tz_ns_rot_header_magic,
  .body_len = (uint32_t)(uintptr_t)g_ra8_ls_ns_signed_body_len,
};
