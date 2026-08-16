/**
 * @file ra8_fmt_portable_main_internal.h
 * @brief Caller-workspace CLI composition for every supported tool verb.
 * @details Exposes bounded JOF paths and strict streamed RBKC inspection.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_fmt_stream.h"

/** @brief Explicit composition-root RAM budget for image-to-JOF conversion. */
typedef enum : uint32_t {
  k_ra8_fmt_cli_convert_arena_bytes = 8U * 1024U * 1024U, /**< Convert prefix (8 MiB).      */
  k_ra8_fmt_cli_record_cap          = 65536U,             /**< All legal JOF audit records. */
  k_ra8_fmt_cli_tile_cap            = 4194304U,           /**< Decoded-tile bytes (4 MiB).  */
  k_ra8_fmt_cli_scratch_cap         = 4718848U,           /**< Stored-tile bound bytes.     */
  k_ra8_fmt_cli_workspace_bytes     = (sizeof(ra8_jof_audit_record_t) * k_ra8_fmt_cli_record_cap) +
                                      k_ra8_fmt_cli_tile_cap +
                                      k_ra8_fmt_cli_scratch_cap, /**< Existing shared high-water. */
  k_ra8_fmt_cli_rbkc_table_cap      = 65537U,   /**< At most 65,536 chunks plus end. */
  k_ra8_fmt_cli_rbkc_compressed_cap = 4194304U, /**< One stored chunk.               */
  k_ra8_fmt_cli_rbkc_chunk_cap      = 4194304U, /**< One inflated chunk.             */
  k_ra8_fmt_cli_rbkc_scratch_cap =
    k_ra8_fmt_cli_workspace_bytes - (k_ra8_fmt_cli_rbkc_table_cap * sizeof(uint64_t)) -
    k_ra8_fmt_cli_rbkc_compressed_cap - k_ra8_fmt_cli_rbkc_chunk_cap, /**< Strict book work. */
} ra8_fmt_cli_limit_t;

/**
 * @struct ra8_fmt_cli_workspace_t
 * @brief One explicit, shared composition-root workspace for portable verbs.
 * @details Convert uses the first 8 MiB. Inspect binds a record table followed
 * by its tile and stored-byte buffers; because CLI verbs are mutually
 * exclusive, one named BSS object covers the larger exact high-water instead of
 * summing per-verb singletons.
 * @since 0.1.0
 */
typedef struct {
  alignas(max_align_t) uint8_t bytes[k_ra8_fmt_cli_workspace_bytes]; /**< Shared named storage. */
} ra8_fmt_cli_workspace_t;

/**
 * @brief Try the caller-workspace JOF-convert command path.
 * @param[in]     argc Process argument count.
 * @param[in]     argv Process argument vector.
 * @param[in,out] arena Explicit caller-owned producer workspace.
 * @param[in]     arena_cap Exact bytes supplied at @p arena.
 * @param[out]    handled Set when this function owns the exit status.
 * @return Process exit status when handled; unspecified otherwise.
 * @pre @p arena is aligned for `max_align_t` and spans @p arena_cap bytes.
 * @post An over-budget input fails before any output transaction begins.
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] int priv_fmt_try_portable_convert(int      argc,
                                                         char**   argv,
                                                         uint8_t* arena,
                                                         size_t   arena_cap,
                                                         bool*    handled);

/**
 * @brief Try strict streamed JOF or RBKC inspection.
 * @param[in]  argc    Process argument count.
 * @param[in] argv Process argument vector.
 * @param[in,out] workspace Explicit shared composition-root arena.
 * @param[out] handled Set when this function owns the exit status.
 * @return Process exit status when handled; unspecified otherwise.
 */
RA8_PRIV [[nodiscard]] int priv_fmt_try_portable_inspect(int                      argc,
                                                         char**                   argv,
                                                         ra8_fmt_cli_workspace_t* workspace,
                                                         bool*                    handled);

/**
 * @brief Try the bounded two-spool JOF-verification command path.
 * @param[in] argc Process argument count.
 * @param[in] argv Process argument vector.
 * @param[in,out] workspace Explicit shared composition-root arena.
 * @param[out] handled Set when this function owns the exit status.
 * @return Process exit status when handled; unspecified otherwise.
 * @pre @p workspace is aligned for every producer and decode carve.
 * @post Every anonymous spool and source descriptor is closed.
 * @post Optional PPM publication occurs only after full comparison and
 * validation.
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] int priv_fmt_try_portable_verify(int                      argc,
                                                        char**                   argv,
                                                        ra8_fmt_cli_workspace_t* workspace,
                                                        bool*                    handled);
