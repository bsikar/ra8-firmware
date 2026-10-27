/**
 * @file ra8_fmt_smoke.c
 * @brief Cross-target smoke TU for the header-only ra8_fmt contract.
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * ra8_fmt owns no code yet, so the only thing a second target can prove
 * about it is that the contract header itself compiles where the format
 * engines will run, and that the four shapes a backend binds stay the
 * objects the engines expect. This translation unit does exactly that and
 * nothing more: it includes inc/ra8_fmt_io.h, which pulls in the
 * ra8_err_t contract from ra8_core, and static-asserts the shapes.
 *
 * It is built by build.zig's default step for whatever target the build
 * was configured with, so tools/check_portability.py --image-builds
 * ra8_fmt exercises it on riscv32-freestanding-none and
 * thumb-freestanding-eabi the same way it exercises the other portable
 * libraries' real builds.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_fmt_io.h"

/* The four contract shapes must stay concrete objects a backend can
   allocate and bind. Size floors, not exact sizes: the ABI the engines
   bind is the shape and its field names, not a padding decision the
   target is free to make differently. */
_Static_assert(sizeof(ra8_fmt_source_t) >= sizeof(ra8_fmt_pread_fn) + sizeof(uint64_t),
    "source shape lost its positioned read or its size");
_Static_assert(sizeof(ra8_fmt_sink_t) >= sizeof(ra8_fmt_sink_write_fn),
    "sink shape lost its append callback");
_Static_assert(sizeof(ra8_fmt_spool_t) >= sizeof(ra8_fmt_pread_fn),
    "spool shape lost its post-seal reader");
_Static_assert(sizeof(ra8_fmt_transaction_t) >= sizeof(void*),
    "transaction shape lost its ops pointer");

/* The positioned-read shape is the contract the JOF engines bind through
   ra8_fmt_source_t::read_at; it must stay a plain freestanding function
   pointer over the five documented arguments. */
_Static_assert((ra8_err_t (*)(void*, uint64_t, uint8_t*, size_t, size_t*))0
    == (ra8_fmt_pread_fn)0,
    "ra8_fmt_pread_fn drifted from the documented read-at shape");
