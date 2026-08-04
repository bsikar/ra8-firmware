/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_pending/ereader_comic/cm_tiled_check.h
 * @brief Boot-time self-check that opens an oversized comic page via the JOF
 *        tile path and reports a deterministic digest (#344).
 *
 * @par Tag
 * [Ring 7 / App] {World: NS}
 *
 * @details
 * The viewable reader in main.c pages small baked CBZ pages with the whole-decode
 * pipeline (`ra8_img_decode_blit`). A page whose decoded size exceeds the decode
 * arena cannot be opened that way -- the #344 cap. This self-check proves the fix
 * on device: it opens a baked oversized single-page CBZ (::k_comic_large_cbz),
 * routes the page through ::ra8_comic_tiles (import-time JOF transcode +
 * `ra8_tile_cache` paging), decodes every tile in bounded RAM, and FNV-hashes the
 * concatenated tile payloads. The integer tile pipeline makes the digest
 * identical on host, ra8_emulator, and silicon, so the boot banner can pin it.
 *
 *
 * @since 0.1.0
 */
#pragma once

#include <stdint.h>

/**
 * @struct cm_tiled_result_t
 * @brief Outcome of the oversized-page tile self-check.
 * @details Populated by ::cm_comic_tiled_selfcheck; @p ok is true only when the
 *          oversized page was over budget for the whole-decode arena AND opened
 *          + tiled + decoded cleanly through the tile path.
 * @invariant When @p ok is false every other field is 0.
 * @since 0.1.0
 */
typedef struct {
  uint32_t w;     /**< Decoded page width, pixels.                 */
  uint32_t h;     /**< Decoded page height, pixels.                */
  uint32_t tiles; /**< Tile count in the produced JOF atlas.       */
  uint32_t crc;   /**< FNV-1a-32 over the concatenated tile bytes. */
  bool     ok;    /**< True when the oversized page tiled cleanly. */
} cm_tiled_result_t;

/**
 * @brief Open the baked oversized page through the tile path and digest it.
 *
 * @details Opens ::k_comic_large_cbz with `ra8_comic`, confirms page 0's decoded
 *          footprint exceeds the whole-decode budget, transcodes it to a JOF
 *          atlas via `ra8_comic_tiles_import`, and decodes every tile through the
 *          owned tile cache, FNV-hashing the decoded pixels. All storage is
 *          file-static SDRAM (no heap); the panel framebuffer is never touched,
 *          so the reader's page-1 render is undisturbed.
 *
 * @return cm_tiled_result_t The page geometry, tile count, digest, and status.
 * @retval {.ok=true,...}  The oversized page opened + tiled + decoded cleanly.
 * @retval {.ok=false,...} The archive, footprint, import, or a tile decode failed.
 *
 * @pre `ra8_cgc_init` / SDRAM bring-up have run (the SDRAM arenas are usable).
 * @pre The baked ::k_comic_large_cbz fixture is linked in.
 * @post No panel framebuffer or ra8_gfx binding is modified.
 * @post On success @p ok is true and the digest is deterministic.
 *
 * @note Not thread-safe; single-threaded boot context.
 * @since 0.1.0
 */
cm_tiled_result_t cm_comic_tiled_selfcheck(void);
