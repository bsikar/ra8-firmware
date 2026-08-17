/**
 * @file mdl_export_internal.h
 * @brief Module-private contracts shared by chapter export writers.
 *
 * @details Defines the bounded page table, miniz workspace adapter, canonical
 * external-cover descriptor, and the cross-translation-unit writer seams. None
 * of these declarations is part of the exporter-facing API.
 *
 * [Ring 4 / Domain] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mdl_export.h"
#include "mdl_export_io_internal.h"
#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_err.h"

/** @brief Fixed bounds on the per-chapter page list. */
typedef enum : uint16_t {
  k_max_pages = 2048, /**< Maximum page entries per chapter. */
  k_name_max  = 256,  /**< Maximum entry-name bytes.         */
} mdl_export_limits_t;

/** @brief Fixed storage used to describe a validated external cover. */
typedef enum : uint8_t {
  k_cover_ext_bytes   = 8U,  /**< Canonical image extension buffer. */
  k_cover_mime_bytes  = 32U, /**< Canonical image MIME buffer.      */
  k_cover_entry_bytes = 32U, /**< Canonical archive member path.    */
} mdl_cover_bounds_t;

/** @brief Validated external cover details retained without owning storage. */
typedef struct {
  const char* source;                     /**< Trusted caller-owned source path.  */
  char        entry[k_cover_entry_bytes]; /**< Canonical relative member path.    */
  char        mime[k_cover_mime_bytes];   /**< MIME derived from magic bytes.     */
  bool        external;                   /**< True when a separate cover exists. */
} mdl_external_cover_t;

/** @brief Metadata stored immediately before each bounded miniz allocation. */
typedef struct {
  max_align_t alignment; /**< Forces following payload to maximum alignment.   */
  size_t      span;      /**< Aligned header-plus-payload bytes in this block. */
  size_t      bytes;     /**< Requested payload bytes currently preserved.     */
  bool        free;      /**< Whether this block can satisfy a later request.  */
} mdl_zip_block_t;

/** @brief Per-writer adapter from miniz callbacks to one caller workspace. */
typedef struct {
  mdl_export_workspace_t* ws;        /**< Exclusive caller-owned workspace.  */
  mdl_zip_block_t*        first;     /**< First miniz block, or NULL.        */
  size_t                  floor;     /**< Cursor restored after writer end.  */
  bool                    exhausted; /**< A callback exceeded bounded space. */
} mdl_zip_allocator_t;

/**
 * @brief Validate an optional bounded source-attribution URL.
 * @details Accepts empty fields or complete absolute HTTP(S) URLs and rejects
 * whitespace, control bytes, invalid authority, and unterminated storage.
 * @param[in] url Fixed-capacity source URL field.
 * @return Source field validation status.
 * @retval k_ra8_ok The field is empty or valid.
 * @retval k_ra8_err_invalid_size The field is not NUL-terminated.
 * @retval k_ra8_err_invalid_arg The nonempty URL is invalid.
 * @pre @p url addresses ::k_mdl_meta_url_max readable bytes.
 * @pre The caller treats every non-ok result as a metadata failure.
 * @post The URL is unchanged.
 * @post No filesystem or network access occurs.
 * @note Thread-safe and allocation-free.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_export_validate_source_url(const char* url);

/**
 * @brief Report whether an snprintf result fit its destination.
 * @details Rejects encoding errors and values that consumed the NUL position.
 * @param[in] written Return value from snprintf.
 * @param[in] cap Destination capacity passed to snprintf.
 * @return Whether formatting completed without truncation.
 * @retval true The complete result fit.
 * @retval false Formatting failed or truncated.
 * @pre @p cap matches the formatted destination.
 * @pre @p written is the unmodified formatting result.
 * @post No state is modified.
 * @post A true result permits use of the formatted string.
 * @note Thread-safe and side-effect free.
 * @since 0.1.0
 */
RA8_PRIV bool priv_mdl_export_snprintf_fit(int written, size_t cap);

/**
 * @brief Bind a miniz archive to caller-owned bounded storage.
 * @details Installs the module allocator callbacks and records the workspace
 * cursor that writer teardown must restore.
 * @param[out] zip Zeroed archive descriptor receiving callbacks.
 * @param[out] alloc Writer-lifetime allocator adapter.
 * @param[in,out] ws Exclusive initialized exporter workspace.
 * @pre Every pointer is non-NULL.
 * @pre Workspace bytes remain live until release.
 * @post No default miniz allocator remains installed.
 * @post @p alloc records the initial workspace cursor.
 * @note Not thread-safe for a shared workspace.
 * @since 0.1.0
 */
RA8_PRIV void priv_mdl_zip_workspace_bind(mz_zip_archive*         zip,
                                          mdl_zip_allocator_t*    alloc,
                                          mdl_export_workspace_t* ws);

/**
 * @brief Restore a workspace after a miniz writer ends.
 * @details Releases every transient archive block in one cursor operation while
 * retaining the measured high-water mark.
 * @param[in,out] alloc Active adapter, or NULL.
 * @pre @p alloc is NULL or its writer has ended.
 * @pre Its workspace is not concurrently accessed.
 * @post A valid workspace cursor equals the saved floor.
 * @post High-water accounting is preserved.
 * @note Not thread-safe for a shared workspace.
 * @since 0.1.0
 */
RA8_PRIV void priv_mdl_zip_workspace_release(mdl_zip_allocator_t* alloc);

/**
 * @brief Classify a miniz writer failure.
 * @details Distinguishes caller-arena exhaustion from other ZIP failures.
 * @param[in] alloc Active allocator adapter, or NULL.
 * @return Export status matching the allocator state.
 * @retval k_ra8_err_invalid_size Bounded storage was exhausted.
 * @retval k_ra8_fail No exhaustion was recorded.
 * @pre @p alloc is NULL or remains live.
 * @pre A writer failure occurred before this call.
 * @post Allocator state is unchanged.
 * @post Equal state produces an equal result.
 * @note Thread-safe across distinct immutable adapters.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_zip_workspace_error(const mdl_zip_allocator_t* alloc);

/**
 * @brief Validate and canonicalize an external cover.
 * @details Leaves in-chapter covers alone and maps a validated external image
 * to a type-derived canonical member path.
 * @param[in,out] storage Injected portable file reader.
 * @param[in] meta Metadata carrying the cover path, or NULL.
 * @param[in] names Sorted chapter page names.
 * @param[in] count Readable page-name rows.
 * @param[out] cover Canonical cover descriptor.
 * @return Cover classification status.
 * @retval k_ra8_ok No external cover is needed or one is valid.
 * @retval k_ra8_err_invalid_arg The fixed path is unterminated.
 * @retval k_ra8_err_invalid_size The canonical path does not fit.
 * @retval k_ra8_err_validation_failed The source is not a valid image.
 * @pre @p names and @p cover are valid for the declared bounds.
 * @pre @p meta is NULL or stable for the call.
 * @post Success initializes @p cover completely.
 * @post Host paths never become archive member names.
 * @note Not thread-safe against cover replacement.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_export_prepare_cover(mdl_storage_t*           storage,
                                                 const mdl_export_meta_t* meta,
                                                 const char               names[][k_name_max],
                                                 size_t                   count,
                                                 mdl_external_cover_t*    cover);

/**
 * @brief Write a CBZ into a caller-owned staged output.
 * @details Stores page and optional cover members and appends ComicInfo before
 * finalizing the bounded ZIP writer.
 * @param[in,out] storage Injected portable file reader.
 * @param[in] dir Chapter directory.
 * @param[in] names Sorted page-name rows.
 * @param[in] count Page count.
 * @param[in,out] output Active validated-publication output.
 * @param[in] meta Resolved metadata.
 * @param[in,out] ws Exclusive exporter workspace.
 * @return Writer status.
 * @retval k_ra8_ok A finalized CBZ was written.
 * @retval k_ra8_err_invalid_size A bound or workspace was exceeded.
 * @retval k_ra8_err_validation_failed An external cover was invalid.
 * @retval k_ra8_fail ZIP or file I/O failed.
 * @pre Paths and name rows are valid and stable.
 * @pre @p output owns an active transaction and remains valid through finalization.
 * @post Success leaves one finalized CBZ.
 * @post Failure leaves transaction abort or publication to the caller.
 * @note Not thread-safe for shared output or workspace.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_export_cbz(mdl_storage_t*           storage,
                                       const char*              dir,
                                       char                     names[][k_name_max],
                                       size_t                   count,
                                       mdl_export_output_t*     output,
                                       const mdl_export_meta_t* meta,
                                       mdl_export_workspace_t*  ws);

/**
 * @brief Write an uncompressed CBT tar archive.
 * @details Streams page members and ComicInfo through deterministic ustar
 * records without retaining the whole archive.
 * @param[in,out] storage Injected portable file reader.
 * @param[in] dir Chapter directory.
 * @param[in] names Sorted page-name rows.
 * @param[in] count Page count.
 * @param[in,out] output Active validated-publication output.
 * @param[in] meta Resolved metadata.
 * @return Writer status.
 * @retval k_ra8_ok A complete CBT was written.
 * @retval k_ra8_err_invalid_size A ustar or metadata bound was exceeded.
 * @retval k_ra8_fail File I/O failed.
 * @pre Paths and rows are valid and stable.
 * @pre @p output owns an active transaction.
 * @post Every opened source stream is closed.
 * @post Success includes the complete ustar trailer.
 * @note Not thread-safe for the same output.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_export_tar(mdl_storage_t*           storage,
                                       const char*              dir,
                                       char                     names[][k_name_max],
                                       size_t                   count,
                                       mdl_export_output_t*     output,
                                       const mdl_export_meta_t* meta);

/**
 * @brief Write a gzip-wrapped CBT archive.
 * @details Streams deterministic ustar records directly through caller-arena
 * DEFLATE into the active output; no intermediate archive exists.
 * @param[in,out] storage Injected portable file reader.
 * @param[in] dir Chapter directory.
 * @param[in] names Sorted page-name rows.
 * @param[in] count Page count.
 * @param[in,out] output Active validated-publication output.
 * @param[in] meta Resolved metadata.
 * @param[in,out] ws Exclusive exporter workspace.
 * @return Writer status.
 * @retval k_ra8_ok A complete gzip stream was written.
 * @retval k_ra8_err_invalid_size A bound or workspace was exceeded.
 * @retval k_ra8_fail File or compression I/O failed.
 * @pre Paths and rows are valid and stable.
 * @pre Workspace bytes remain live throughout the call.
 * @post No intermediate file is created.
 * @post Success includes the gzip checksum trailer.
 * @note Not thread-safe for shared output or workspace.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_export_tar_gzip(mdl_storage_t*           storage,
                                            const char*              dir,
                                            char                     names[][k_name_max],
                                            size_t                   count,
                                            mdl_export_output_t*     output,
                                            const mdl_export_meta_t* meta,
                                            mdl_export_workspace_t*  ws);

/**
 * @brief Write a fixed-layout EPUB3 archive.
 * @details Emits OCF roots, page XHTML, images, metadata, cover information,
 * navigation, and the final ZIP directory from caller storage.
 * @param[in,out] storage Injected portable file reader.
 * @param[in] dir Chapter directory.
 * @param[in] names Sorted page-name rows.
 * @param[in] count Page count.
 * @param[in,out] output Active validated-publication output.
 * @param[in] meta Resolved metadata.
 * @param[in,out] ws Exclusive exporter workspace.
 * @return Writer status.
 * @retval k_ra8_ok A finalized EPUB was written.
 * @retval k_ra8_err_invalid_size A text or workspace bound was exceeded.
 * @retval k_ra8_err_validation_failed An external cover was invalid.
 * @retval k_ra8_fail ZIP, formatting, or file I/O failed.
 * @pre Paths and rows are valid and stable.
 * @pre Workspace bytes and @p output remain live through writer teardown.
 * @post The ZIP writer is ended on every initialized path.
 * @post Success includes OPF and navigation members.
 * @note Not thread-safe for shared output or workspace.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_export_epub(mdl_storage_t*           storage,
                                        const char*              dir,
                                        char                     names[][k_name_max],
                                        size_t                   count,
                                        mdl_export_output_t*     output,
                                        const mdl_export_meta_t* meta,
                                        mdl_export_workspace_t*  ws);

/**
 * @brief Compile one page directory into a strict chunked RABOOK artifact.
 * @details Produces a private fixed-layout EPUB with the production writer,
 *          compiles it through the firmware RABOOK1 pipeline, wraps the flat
 *          blob as independent RBKC chunks, and validates the complete staged
 *          artifact before atomic publication.
 * @param[in,out] storage Injected portable storage and transaction provider.
 * @param[in] directory Canonical chapter directory.
 * @param[in] names Sorted page-name rows.
 * @param[in] count Page count within the fixed RABOOK profile.
 * @param[in] destination Canonical final `.rabook` path.
 * @param[in] meta Resolved metadata to embed.
 * @param[in,out] workspace Exclusive caller-owned export/compiler workspace.
 * @return Writer, compiler, validator, or publication status.
 * @retval k_ra8_ok One strict RBKC artifact was published.
 * @retval k_ra8_err_invalid_size A page/profile/workspace bound was exceeded.
 * @retval k_ra8_err_validation_failed An intermediate or final artifact failed validation.
 * @pre All pointers and page rows are valid and stable for the complete call.
 * @pre @p ws is exclusively mutable and no competing writer owns the destination.
 * @post Success publishes exactly one reader-openable RABOOK artifact.
 * @post Failure removes private intermediates and preserves any prior destination.
 * @note Not thread-safe for shared storage, destination, or workspace.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_export_rabook(mdl_storage_t*           storage,
                                          const char*              directory,
                                          char                     names[][k_name_max],
                                          size_t                   count,
                                          const char*              destination,
                                          const mdl_export_meta_t* meta,
                                          mdl_export_workspace_t*  workspace);

/**
 * @brief Convert every page in a chapter directory to a sibling `.jof` atlas.
 *
 * @details
 * Transcodes each listed page in place: `page.jpg` becomes `page.jof` beside
 * it, leaving the source file untouched. Each page is produced through the
 * firmware's own `ra8_jof` producer, so a container this tool writes is
 * byte-identical to one the board would produce from the same source, and the
 * full-width band geometry is the shape `ra8_longstrip` scrolls.
 *
 * Stops at the first page that fails and reports that page's error, so a
 * partially converted directory is always explained by a non-ok return rather
 * than discovered later; pages already written before the failure remain.
 *
 * @param[in,out] storage Injected portable storage and transaction provider.
 * @param[in] dir   Chapter directory holding the page files.
 * @param[in] names Page file names, `count` entries of at most ::k_name_max
 *                  bytes each, in the order they should be converted.
 * @param[in] count Number of valid entries in @p names.
 * @param[in,out] ws Exclusive caller-owned exporter workspace.
 *
 * @return Result code.
 * @retval k_ra8_ok                Every page was transcoded.
 * @retval k_ra8_err_not_supported A page is in no format the producer decodes.
 * @retval k_ra8_err_invalid_size  A page's geometry admits no work arena.
 * @retval k_ra8_fail              A page could not be read, or its output
 *                                 could not be opened or written.
 *
 * @pre @p storage is initialized and exclusive to this export.
 * @pre @p dir names an existing, readable directory.
 * @pre @p names holds @p count readable entries.
 * @post On ::k_ra8_ok a `.jof` sibling exists for every entry in @p names.
 * @post On failure, earlier committed pages remain visible; the failed page
 *       and every later page preserve their prior destinations.
 *
 * @note Not thread-safe for a shared workspace or destination directory.
 *
 * @see mdl_export_chapter_ws()
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_export_jof(mdl_storage_t*          storage,
                                       const char*             dir,
                                       const char              names[][k_name_max],
                                       size_t                  count,
                                       mdl_export_workspace_t* ws);
