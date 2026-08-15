/**
 * @file mdl_export.c
 * @brief Package a chapter folder into a reader-openable container.
 *
 * @details
 * The self-contained formats are produced from vendored, in-tree code with no
 * system library or external process -- CBZ via the firmware's own miniz ZIP
 * writer, CBT via a hand-written ustar tar, and CBT.GZ via miniz DEFLATE plus
 * RFC-1952 framing. Every supported writer consumes only explicit bounded
 * caller storage. Reserved CBR, CBT.XZ, and RABOOK values deliberately have no
 * half-functional external-process implementations.
 *
 * The JOF arm lives in the sibling mdl_export_jof.c: it is the only format that
 * reaches into the firmware's `ra8_jof` decode/encode stack, so it owns
 * its own translation unit rather than widening this one's dependencies. This
 * file still dispatches to it, via mdl_export_jof() in mdl_export_internal.h.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_export.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "mdl_atomic.h"
#include "mdl_export_internal.h"
#include "mdl_sanitize.h"
#include "mdl_url_guard.h"
#include "mdl_urlname.h"
#include "miniz.h"
#include "ra8_attributes.h"

/** @brief ustar (POSIX tar) header field offsets and lengths. */
typedef enum : uint16_t {
  k_tar_block   = 512, /**< Tar block size.           */
  k_off_name    = 0,   /**< name field offset.        */
  k_len_name    = 100, /**< name field width.         */
  k_off_mode    = 100, /**< mode field offset.        */
  k_off_uid     = 108, /**< uid field offset.         */
  k_off_gid     = 116, /**< gid field offset.         */
  k_len_id      = 8,   /**< mode/uid/gid field width. */
  k_off_size    = 124, /**< size field offset.        */
  k_len_size    = 12,  /**< size field width.         */
  k_off_mtime   = 136, /**< mtime field offset.       */
  k_len_mtime   = 12,  /**< mtime field width.        */
  k_off_chksum  = 148, /**< checksum field offset.    */
  k_len_chksum  = 8,   /**< checksum field width.     */
  k_off_type    = 156, /**< typeflag field offset.    */
  k_off_magic   = 257, /**< "ustar" magic offset.     */
  k_len_magic   = 6,   /**< "ustar" magic width.      */
  k_off_version = 263, /**< version field offset.     */
} mdl_tar_layout_t;

/** @brief File mode written into tar headers and the zip STORE flag source. */
typedef enum : uint16_t {
  k_file_mode = 0644, /**< Regular-file permission bits (octal). */
} mdl_tar_mode_t;

/** @brief gzip framing constants (RFC 1952). */
typedef enum : uint8_t {
  k_gz_id1 = 0x1F, /**< gzip magic byte 1.           */
  k_gz_id2 = 0x8B, /**< gzip magic byte 2.           */
  k_gz_cm  = 0x08, /**< compression method: DEFLATE. */
  k_gz_os  = 0xFF, /**< OS: unknown.                 */
} mdl_gzip_hdr_t;

/** @brief Byte-serialisation constants. */
typedef enum : uint16_t {
  k_byte_bits    = 8,    /**< Bits per byte.            */
  k_byte_mask    = 0xFF, /**< Low-byte mask.            */
  k_u32_bytes    = 4,    /**< Bytes in a u32.           */
  k_gzip_hdr_len = 10,   /**< gzip fixed header length. */
} mdl_serial_t;

/** @brief Radices and bounded XML/EPUB text expansion sizes. */
typedef enum : uint16_t {
  k_decimal_radix       = 10U,  /**< Decimal integer parsing radix.       */
  k_xml_amp_entity_len  = 5U,   /**< Bytes in the XML entity "&amp;".     */
  k_meta_line_slack     = 128U, /**< Key/delimiter space beyond a path.   */
  k_epub_creator_factor = 12U,  /**< Writer plus artist expansion factor. */
  k_epub_fragment_slack = 64U,  /**< Fixed bytes around an XML fragment.  */
  k_epub_creator_slack  = 128U, /**< Fixed bytes around all creators.     */
} mdl_text_bounds_t;

/** @brief Fixed storage used to describe a validated external cover. */
typedef enum : uint8_t {
  k_cover_ext_bytes   = 8U,  /**< Canonical image extension buffer. */
  k_cover_mime_bytes  = 32U, /**< Canonical image MIME buffer.      */
  k_cover_entry_bytes = 32U, /**< Canonical archive member path.    */
} mdl_cover_bounds_t;

/** @brief FNV-1a values used to derive deterministic publication UUIDs. */
typedef enum : uint64_t {
  k_uuid_fnv_prime = UINT64_C(1099511628211),        /**< FNV-1a 64-bit prime.      */
  k_uuid_seed_one  = UINT64_C(14695981039346656037), /**< Primary FNV offset basis. */
  k_uuid_seed_two  = UINT64_C(7809847782465536322),  /**< Independent second seed.  */
} mdl_uuid_hash_t;

/** @brief RFC 4122 UUID byte layout, masks, and version/variant bits. */
typedef enum : uint8_t {
  k_uuid_separator       = 0xFFU, /**< Separator mixed between hash fields. */
  k_uuid_byte_count      = 16U,   /**< Bytes in an RFC 4122 UUID.           */
  k_uuid_half_bytes      = 8U,    /**< Bytes contributed by each hash.      */
  k_uuid_top_shift       = 56U,   /**< Shift selecting the top hash byte.   */
  k_uuid_version_byte    = 6U,    /**< UUID version field byte index.       */
  k_uuid_version_mask    = 0x0FU, /**< Mask retaining non-version bits.     */
  k_uuid_version_five    = 0x50U, /**< RFC 4122 version-five field bits.    */
  k_uuid_variant_byte    = 8U,    /**< UUID variant field byte index.       */
  k_uuid_variant_mask    = 0x3FU, /**< Mask retaining non-variant bits.     */
  k_uuid_variant_rfc4122 = 0x80U, /**< RFC 4122 variant field bits.         */
  k_uuid_node_offset     = 10U,   /**< First byte of the UUID node field.   */
  k_uuid_last_byte       = 15U,   /**< Final UUID byte index.               */
} mdl_uuid_layout_t;

/** @brief Validated external cover details retained without owning storage. */
typedef struct {
  const char* source;                     /**< Trusted caller-owned source path.  */
  char        entry[k_cover_entry_bytes]; /**< Canonical relative member path.    */
  char        mime[k_cover_mime_bytes];   /**< MIME derived from magic bytes.     */
  bool        external;                   /**< True when a separate cover exists. */
} mdl_external_cover_t;

/**
 * @brief ustar magic field: the 5 ASCII chars plus the NUL POSIX requires.
 * @details Raw bytes, not a string literal -- the NUL here is payload (the
 *          field is exactly 6 bytes wide), not a C string terminator.
 */
static const uint8_t k_ustar_magic[] = {'u', 's', 't', 'a', 'r', '\0'};
/** @brief ustar version field: the two ASCII digits "00", unterminated. */
static const uint8_t k_ustar_version[] = {'0', '0'};

static_assert(sizeof(k_ustar_magic) == (size_t)k_len_magic,
              "ustar magic constant must fill the 6-byte magic field exactly");

mdl_format_t mdl_format_from_str(const char* s)
{
  if ((s == nullptr) || (strcmp(s, "loose") == 0)) {
    return k_mdl_fmt_loose;
  }
  if (strcmp(s, "cbz") == 0) {
    return k_mdl_fmt_cbz;
  }
  if (strcmp(s, "cbt") == 0) {
    return k_mdl_fmt_cbt;
  }
  if (strcmp(s, "cbt.gz") == 0) {
    return k_mdl_fmt_cbt_gz;
  }
  if (strcmp(s, "epub") == 0) {
    return k_mdl_fmt_epub;
  }
  if (strcmp(s, "jof") == 0) {
    return k_mdl_fmt_jof;
  }
  return k_mdl_fmt_invalid;
}

const char* mdl_format_ext(mdl_format_t fmt)
{
  switch (fmt) {
    case k_mdl_fmt_cbz:
      return "cbz";
    case k_mdl_fmt_cbt:
      return "cbt";
    case k_mdl_fmt_cbr:
      return "cbr";
    case k_mdl_fmt_cbt_xz:
      return "cbt.xz";
    case k_mdl_fmt_cbt_gz:
      return "cbt.gz";
    case k_mdl_fmt_epub:
      return "epub";
    case k_mdl_fmt_jof:
      return "jof";
    case k_mdl_fmt_rabook:
      return "rabook";
    case k_mdl_fmt_loose:
    case k_mdl_fmt_invalid:
    default:
      return "";
  }
}

bool mdl_format_is_dir_output(mdl_format_t fmt)
{
  /* JOF is inherently per-page: one `.jof` band atlas is written beside each
   * source image, so a chapter is a directory of atlases rather than a single
   * container file at out_path. Every other archive format produces one file.
   */
  return fmt == k_mdl_fmt_jof;
}

void mdl_export_workspace_init(mdl_export_workspace_t* ws, void* data, size_t cap)
{
  if (ws == nullptr) {
    return;
  }
  ws->data       = (uint8_t*)data;
  ws->cap        = (data == nullptr) ? 0U : cap;
  ws->used       = 0U;
  ws->high_water = 0U;
}

void* mdl_export_workspace_take(mdl_export_workspace_t* ws, size_t bytes, size_t alignment)
{
  if ((ws == nullptr) || (ws->data == nullptr) || (bytes == 0U) || (alignment == 0U) ||
      ((alignment & (alignment - 1U)) != 0U)) {
    return nullptr;
  }
  const size_t mask = alignment - 1U;
  if (ws->used > (SIZE_MAX - mask)) {
    return nullptr;
  }
  const size_t start = (ws->used + mask) & ~mask;
  if ((start > ws->cap) || (bytes > (ws->cap - start))) {
    return nullptr;
  }
  ws->used = start + bytes;
  if (ws->used > ws->high_water) {
    ws->high_water = ws->used;
  }
  return &ws->data[start];
}

/**
 * @brief Validate an optional bounded source-attribution URL.
 * @details Requires complete NUL termination, an HTTP(S) scheme, a nonempty
 *          authority, and no whitespace/control bytes while permitting XML
 *          metacharacters that the container emitters escape later.
 * @param[in] url Fixed-capacity source URL field.
 * @return Source field validation status.
 * @retval k_ra8_ok The field is empty or a valid absolute HTTP(S) URL.
 * @retval k_ra8_err_invalid_size The fixed field lacks a terminating NUL.
 * @retval k_ra8_err_invalid_arg The nonempty URL has an invalid scheme,
 *                               authority, whitespace, or control byte.
 * @pre @p url points to at least ::k_mdl_meta_url_max readable bytes.
 * @pre The caller treats every non-ok result as a hard metadata failure.
 * @post @p url is not modified.
 * @post No allocation, network access, or filesystem access occurs.
 * @note Thread-safe and allocation-free.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t validate_source_url(const char* url)
{
  const size_t len = strnlen(url, (size_t)k_mdl_meta_url_max);
  if (len == (size_t)k_mdl_meta_url_max) {
    return k_ra8_err_invalid_size;
  }
  if (len == 0U) {
    return k_ra8_ok;
  }
  char host[k_mdl_meta_url_max];
  if (!mdl_url_scheme_allowed(url) || !mdl_url_host(url, host, sizeof(host))) {
    return k_ra8_err_invalid_arg;
  }
  for (size_t i = 0U; i < len; ++i) {
    if (isspace((unsigned char)url[i]) || iscntrl((unsigned char)url[i])) {
      return k_ra8_err_invalid_arg;
    }
  }
  return k_ra8_ok;
}

/** @brief Metadata stored immediately before each miniz arena allocation. */
typedef struct {
  max_align_t alignment; /**< Forces following payload to maximum alignment.   */
  size_t      span;      /**< Aligned header-plus-payload bytes in this block. */
  size_t      bytes;     /**< Requested payload bytes currently preserved.     */
  bool        free;      /**< Whether this block may satisfy a later request.  */
} mdl_zip_block_t;

/** @brief Per-writer adapter from miniz allocation callbacks to one workspace. */
typedef struct {
  mdl_export_workspace_t* ws;        /**< Exclusive caller-owned workspace.  */
  mdl_zip_block_t*        first;     /**< First miniz block, or NULL.        */
  size_t                  floor;     /**< Cursor restored after writer end.  */
  bool                    exhausted; /**< A callback exceeded bounded space. */
} mdl_zip_allocator_t;

/**
 * @brief Round a byte count up to maximum host alignment.
 * @details Performs the power-of-two rounding only after overflow checks.
 * @param[in] bytes Requested payload byte count.
 * @param[out] out Rounded result.
 * @return Whether the aligned size is representable.
 * @retval true @p out contains the aligned size.
 * @retval false @p out is NULL or rounding would overflow.
 * @pre @p bytes is an untrusted bounded allocation request.
 * @pre Maximum alignment is a nonzero power of two.
 * @post Success writes a value not smaller than @p bytes.
 * @post Failure does not write through @p out.
 * @note Thread-safe and side-effect free.
 * @since 0.1.0
 */
RA8_INTERNAL static bool zip_align_size(size_t bytes, size_t* out)
{
  const size_t alignment = _Alignof(max_align_t);
  const size_t mask      = alignment - 1U;
  if ((out == nullptr) || (bytes > (SIZE_MAX - mask))) {
    return false;
  }
  *out = (bytes + mask) & ~mask;
  return true;
}

/**
 * @brief Find an allocator block by its payload address.
 * @details Walks only aligned blocks created after the saved writer floor and
 *          never follows a pointer outside the current workspace high edge.
 * @param[in] alloc Active callback adapter.
 * @param[in] address Candidate payload address.
 * @return Matching block header, or NULL.
 * @retval non-NULL @p address names a block from this adapter.
 * @retval NULL Arguments are invalid or no block matches.
 * @pre @p alloc is NULL or owns a well-formed current block chain.
 * @pre @p address is treated only as an opaque comparison value.
 * @post No allocator block or workspace counter is modified.
 * @post A returned header lies within the active workspace range.
 * @note Not thread-safe with concurrent allocation from the same adapter.
 * @since 0.1.0
 */
RA8_INTERNAL static mdl_zip_block_t* zip_find_block(mdl_zip_allocator_t* alloc, const void* address)
{
  if ((alloc == nullptr) || (alloc->ws == nullptr) || (address == nullptr)) {
    return nullptr;
  }
  uint8_t* const end = alloc->ws->data + alloc->ws->used;
  for (mdl_zip_block_t* block = alloc->first;
       (block != nullptr) && ((uint8_t*)block < end) && (block->span >= sizeof(*block));
       block = ((uint8_t*)block + block->span < end)
                 ? (mdl_zip_block_t*)((uint8_t*)block + block->span)
                 : nullptr) {
    if ((const void*)(block + 1) == address) {
      return block;
    }
  }
  return nullptr;
}

/**
 * @brief Allocate reusable miniz storage from a bounded exporter arena.
 * @details Rejects multiplication overflow, reuses a released first-fit block,
 *          or appends one aligned block through ::mdl_export_workspace_take.
 * @param[in,out] opaque Pointer to the active ::mdl_zip_allocator_t.
 * @param[in] items Element count requested by miniz.
 * @param[in] size Bytes per requested element.
 * @return Maximum-aligned payload storage, or NULL.
 * @retval non-NULL The complete bounded request was satisfied.
 * @retval NULL Arguments overflowed or caller capacity was exhausted.
 * @pre @p opaque points to an active exclusive writer adapter.
 * @pre Its workspace data remains live and writable.
 * @post Success records exact requested bytes in one live block.
 * @post Failure sets the adapter exhaustion flag and performs no system allocation.
 * @note Not thread-safe for a shared adapter.
 * @since 0.1.0
 */
RA8_INTERNAL static void* zip_workspace_alloc(void* opaque, size_t items, size_t size)
{
  mdl_zip_allocator_t* alloc = (mdl_zip_allocator_t*)opaque;
  if ((alloc == nullptr) || (alloc->ws == nullptr) || (items == 0U) || (size == 0U) ||
      (items > (SIZE_MAX / size))) {
    if (alloc != nullptr) {
      alloc->exhausted = true;
    }
    return nullptr;
  }
  const size_t bytes = items * size;
  size_t       payload_span;
  if (!zip_align_size(bytes, &payload_span) ||
      (payload_span > (SIZE_MAX - sizeof(mdl_zip_block_t)))) {
    alloc->exhausted = true;
    return nullptr;
  }
  uint8_t* const end = alloc->ws->data + alloc->ws->used;
  for (mdl_zip_block_t* block = alloc->first;
       (block != nullptr) && ((uint8_t*)block < end) && (block->span >= sizeof(*block));
       block = ((uint8_t*)block + block->span < end)
                 ? (mdl_zip_block_t*)((uint8_t*)block + block->span)
                 : nullptr) {
    if (block->free && ((block->span - sizeof(*block)) >= bytes)) {
      block->bytes = bytes;
      block->free  = false;
      return block + 1;
    }
  }
  const size_t     block_span = sizeof(mdl_zip_block_t) + payload_span;
  mdl_zip_block_t* block =
    (mdl_zip_block_t*)mdl_export_workspace_take(alloc->ws, block_span, _Alignof(max_align_t));
  if (block == nullptr) {
    alloc->exhausted = true;
    return nullptr;
  }
  block->span  = block_span;
  block->bytes = bytes;
  block->free  = false;
  if (alloc->first == nullptr) {
    alloc->first = block;
  }
  return block + 1;
}

/**
 * @brief Release a miniz block for reuse within the current writer.
 * @details Marks only blocks found in the current bounded adapter; no system
 *          deallocator or out-of-arena address is ever invoked.
 * @param[in,out] opaque Pointer to the active ::mdl_zip_allocator_t.
 * @param[in] address Payload returned by this adapter, or NULL.
 * @pre @p opaque is NULL or identifies the active writer allocator.
 * @pre @p address is NULL or was returned by the matching allocator.
 * @post A recognized block becomes available for first-fit reuse.
 * @post Workspace cursors and high-water accounting are unchanged.
 * @note Not thread-safe for a shared writer allocator.
 * @since 0.1.0
 */
RA8_INTERNAL static void zip_workspace_free(void* opaque, void* address)
{
  mdl_zip_allocator_t* alloc = (mdl_zip_allocator_t*)opaque;
  mdl_zip_block_t*     block = zip_find_block(alloc, address);
  if (block != nullptr) {
    block->free = true;
  }
}

/**
 * @brief Resize a miniz block within the bounded reusable arena.
 * @details Retains a sufficiently large block in place; otherwise obtains a
 *          replacement, copies the exact prior request, and releases the old block.
 * @param[in,out] opaque Pointer to the active ::mdl_zip_allocator_t.
 * @param[in] address Existing payload, or NULL for allocation semantics.
 * @param[in] items New element count.
 * @param[in] size Bytes per new element.
 * @return Resized payload storage, or NULL.
 * @retval non-NULL The request was satisfied with preserved prior bytes.
 * @retval NULL Input was invalid or bounded storage was exhausted.
 * @pre @p opaque points to an active exclusive writer adapter.
 * @pre Non-NULL @p address came from the matching adapter and is still live.
 * @post Success preserves the previous requested payload prefix.
 * @post Failure never invokes a system allocator or frees caller storage.
 * @note Not thread-safe for a shared adapter.
 * @since 0.1.0
 */
RA8_INTERNAL static void*
zip_workspace_realloc(void* opaque, void* address, size_t items, size_t size)
{
  mdl_zip_allocator_t* alloc = (mdl_zip_allocator_t*)opaque;
  if (address == nullptr) {
    return zip_workspace_alloc(opaque, items, size);
  }
  if ((alloc == nullptr) || (items == 0U) || (size == 0U) || (items > (SIZE_MAX / size))) {
    if (alloc != nullptr) {
      alloc->exhausted = true;
    }
    return nullptr;
  }
  mdl_zip_block_t* block = zip_find_block(alloc, address);
  const size_t     bytes = items * size;
  if ((block == nullptr) || block->free) {
    alloc->exhausted = true;
    return nullptr;
  }
  if ((block->span - sizeof(*block)) >= bytes) {
    block->bytes = bytes;
    return address;
  }
  void* replacement = zip_workspace_alloc(opaque, items, size);
  if (replacement == nullptr) {
    return nullptr;
  }
  memcpy(replacement, address, block->bytes);
  block->free = true;
  return replacement;
}

/**
 * @brief Bind one zeroed miniz archive to a bounded allocator.
 * @details Records the current workspace floor and installs allocation,
 *          release, and resize callbacks before miniz initialization.
 * @param[out] zip Archive descriptor to initialize.
 * @param[out] alloc Callback adapter with writer lifetime.
 * @param[in,out] ws Exclusive initialized exporter workspace.
 * @pre All pointer arguments are non-NULL.
 * @pre @p ws owns live writable storage through writer teardown.
 * @post @p zip has no default miniz allocator callback.
 * @post @p alloc records the cursor restored by ::zip_workspace_release.
 * @note Not thread-safe for a shared workspace.
 * @since 0.1.0
 */
RA8_INTERNAL static void
zip_workspace_bind(mz_zip_archive* zip, mdl_zip_allocator_t* alloc, mdl_export_workspace_t* ws)
{
  memset(zip, 0, sizeof(*zip));
  *alloc               = (mdl_zip_allocator_t){.ws = ws, .floor = ws->used};
  zip->m_pAlloc        = zip_workspace_alloc;
  zip->m_pFree         = zip_workspace_free;
  zip->m_pRealloc      = zip_workspace_realloc;
  zip->m_pAlloc_opaque = alloc;
}

/**
 * @brief Release all miniz allocations while preserving high-water.
 * @details Restores the bump cursor to its pre-writer floor after miniz has
 *          ended, making transient writer storage reusable by later phases.
 * @param[in,out] alloc Active callback adapter, or NULL.
 * @pre @p alloc is NULL or its writer has ended or failed initialization.
 * @pre The associated workspace is not concurrently accessed.
 * @post A valid workspace cursor equals the saved floor.
 * @post `high_water` retains the largest observed allocation edge.
 * @note Not thread-safe for a shared workspace.
 * @since 0.1.0
 */
RA8_INTERNAL static void zip_workspace_release(mdl_zip_allocator_t* alloc)
{
  if ((alloc != nullptr) && (alloc->ws != nullptr)) {
    alloc->ws->used = alloc->floor;
  }
}

/**
 * @brief Map a miniz failure to bounded exhaustion when applicable.
 * @details Distinguishes an arena callback refusal from ordinary ZIP I/O or
 *          format failures so callers can size their explicit workspace.
 * @param[in] alloc Active callback adapter, or NULL.
 * @return Exporter error matching the recorded allocator state.
 * @retval k_ra8_err_invalid_size Bounded callback capacity was exhausted.
 * @retval k_ra8_fail No allocator exhaustion was recorded.
 * @pre @p alloc is NULL or remains alive through result classification.
 * @pre The writer has reported a failure before this helper is called.
 * @post No workspace counter or block is modified.
 * @post Identical allocator state produces an identical result.
 * @note Thread-safe for immutable distinct adapters.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t zip_workspace_error(const mdl_zip_allocator_t* alloc)
{
  return ((alloc != nullptr) && alloc->exhausted) ? k_ra8_err_invalid_size : k_ra8_fail;
}
/**
 * @brief Compare two fixed-width page-name rows for qsort
 * @details Treats each row as a NUL-terminated filename and delegates lexical ordering to strcmp.
 * @param[in] a First page-name row.
 * @param[in] b Second page-name row.
 * @return Lexical ordering result.
 * @retval int Negative, zero, or positive according to strcmp ordering.
 * @pre Both pointers address NUL-terminated name rows.
 * @pre Neither row is modified during sorting.
 * @post Input rows remain unchanged.
 * @post Equal names produce zero.
 * @note Thread-safe for immutable rows.
 * @since 0.1.0
 */
RA8_INTERNAL static int name_cmp(const void* a, const void* b)
{
  return strcmp((const char*)a, (const char*)b);
}

/**
 * @brief Test a filename suffix without ASCII case sensitivity
 * @details Compares only the tail and rejects a suffix longer than the name.
 * @param[in] name NUL-terminated filename.
 * @param[in] suffix NUL-terminated suffix.
 * @return Whether the complete suffix matches.
 * @retval true The suffix matches ignoring ASCII case.
 * @retval false Length or bytes differ.
 * @pre Both inputs are non-NULL and NUL-terminated.
 * @pre Inputs remain stable during the comparison.
 * @post Neither input is modified.
 * @post The result depends only on the inputs.
 * @note Thread-safe: this is a pure predicate.
 * @since 0.1.0
 */
RA8_INTERNAL static bool ends_with_ci(const char* name, const char* suffix)
{
  const size_t nl = strlen(name);
  const size_t sl = strlen(suffix);
  if (sl > nl) {
    return false;
  }
  const char* tail = name + (nl - sl);
  for (size_t i = 0U; i < sl; ++i) {
    if (tolower((unsigned char)tail[i]) != tolower((unsigned char)suffix[i])) {
      return false;
    }
  }
  return true;
}

/**
 * @brief True if `name` is a raster page image a reader engine can decode.
 *
 * @details
 * Packaging must include ONLY page images. A chapter folder often also holds
 * this tool's own prior output (a sibling `.jof`/`.cbz`, a `.tar.tmp`) or OS
 * junk; folding those into an archive makes the reader choke when it decodes a
 * non-image "page" (the 0x107 that bit re-runs). Filtering by extension keeps
 * packaging idempotent -- re-running any format on a folder is safe.
 * @param[in] name NUL-terminated directory-entry name.
 * @return Whether the suffix names a supported raster page.
 * @retval true A supported raster suffix matched.
 * @retval false The entry is not a page image.
 * @pre @p name is non-NULL and NUL-terminated.
 * @pre Extension classification is sufficient at this enumeration stage.
 * @post @p name is unchanged.
 * @post The result is deterministic for the same name.
 * @note Thread-safe: this is a pure classifier.
 * @since 0.1.0
 */
RA8_INTERNAL static bool is_page_image(const char* name)
{
  static const char* const k_img_exts[] = {".jpg", ".jpeg", ".png", ".webp", ".gif", ".bmp"};
  for (size_t i = 0U; i < (sizeof(k_img_exts) / sizeof(k_img_exts[0])); ++i) {
    if (ends_with_ci(name, k_img_exts[i])) {
      return true;
    }
  }
  return false;
}

/**
 * @brief List a chapter's page-image files (only) into `names`, sorted.
 * @details Sets `*out_truncated` when a qualifying page image exists beyond
 *          `cap`, so the caller fails loudly instead of packaging a chapter
 *          short. Non-image junk beyond the cap does not trip the flag.
 * @param[in] dir NUL-terminated chapter directory path.
 * @param[out] names Fixed-width destination table for page filenames.
 * @param[in] cap Maximum rows available in @p names.
 * @param[out] out_truncated Whether additional qualifying pages were found.
 * @return Number of sorted rows written.
 * @retval size_t Page count, or zero when the directory cannot be opened.
 * @pre All pointers are non-NULL and @p dir is NUL-terminated.
 * @pre @p names contains @p cap writable rows of ::k_name_max bytes.
 * @post Written rows are NUL-terminated and lexically sorted.
 * @post `*out_truncated` distinguishes an empty/open failure from table overflow.
 * @note Not thread-safe against concurrent directory mutation.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t
list_pages(const char* dir, char names[][k_name_max], size_t cap, bool* out_truncated)
{
  *out_truncated = false;
  DIR* d         = opendir(dir);
  if (d == nullptr) {
    return 0U;
  }
  size_t               n = 0U;
  const struct dirent* e = readdir(d);
  while (e != nullptr) {
    /* Skip hidden / AppleDouble, and anything that is not a page image so a
     * dir already holding this tool's output re-packages cleanly. A name that
     * would not fit is rejected rather than silently truncated (which could
     * collide two distinct pages onto one entry). */
    if ((e->d_name[0] != '.') && is_page_image(e->d_name) &&
        (strlen(e->d_name) < (size_t)k_name_max)) {
      if (n >= cap) {
        *out_truncated = true; /* more pages than the fixed table holds */
        break;
      }
      (void)snprintf(names[n], k_name_max, "%s", e->d_name);
      ++n;
    }
    e = readdir(d);
  }
  (void)closedir(d);
  qsort(names, n, k_name_max, name_cmp);

  return n;
}
/**
 * @brief Derive a deterministic UTC modified timestamp from source pages
 * @details Stats every selected page and formats the newest mtime as ISO-8601 UTC.
 * @param[in,out] meta Metadata object receiving the timestamp.
 * @param[in] dir NUL-terminated chapter directory.
 * @param[in] names Sorted page-name table.
 * @param[in] count Number of readable name rows.
 * @return Whether every page was statted and formatted.
 * @retval true `meta->modified` contains the derived timestamp.
 * @retval false A path, stat, clock conversion, or formatting step failed.
 * @pre Inputs are non-NULL and every name is NUL-terminated.
 * @pre @p meta is exclusively owned during the call.
 * @post Success writes one NUL-terminated UTC timestamp.
 * @post Failure is visible and never reported as a valid timestamp.
 * @note Not thread-safe against concurrent page replacement.
 * @since 0.1.0
 */
RA8_INTERNAL static bool metadata_set_page_timestamp(mdl_export_meta_t* meta,
                                                     const char*        dir,
                                                     const char         names[][k_name_max],
                                                     size_t             count)
{
  time_t latest = 0;
  for (size_t i = 0U; i < count; ++i) {
    char        path[PATH_MAX];
    const int   n = snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
    struct stat st;
    if ((n < 0) || ((size_t)n >= sizeof(path)) || (stat(path, &st) != 0)) {
      return false;
    }
    if (st.st_mtime > latest) {
      latest = st.st_mtime;
    }
  }
  struct tm utc;
  if ((latest <= 0) || (gmtime_r(&latest, &utc) == nullptr)) {
    return false;
  }
  return strftime(meta->modified, sizeof(meta->modified), "%Y-%m-%dT%H:%M:%SZ", &utc) != 0U;
}

/**
 * @brief Round a byte count up to a whole tar block
 * @details Applies the fixed ustar record size used by both headers and padding.
 * @param[in] n Byte count to round.
 * @return Smallest tar-aligned size not below @p n.
 * @retval size_t Rounded byte count.
 * @pre @p n leaves room for one block-minus-one addition.
 * @pre ::k_tar_block is a nonzero power-independent record size.
 * @post The result is divisible by ::k_tar_block.
 * @post The result is greater than or equal to @p n.
 * @note Thread-safe: this is a pure arithmetic helper.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t round_block(size_t n)
{
  return ((n + (size_t)k_tar_block - 1U) / (size_t)k_tar_block) * (size_t)k_tar_block;
}

/**
 * @brief Build one deterministic ustar regular-file header
 * @details Encodes bounded name, size, fixed permissions and zero timestamp,
 *          then computes the POSIX checksum without truncating the member name.
 * @param[out] blk Writable tar-record destination.
 * @param[in] name NUL-terminated member name.
 * @param[in] size Member payload size.
 * @return Header construction status.
 * @retval k_ra8_ok A complete header was written.
 * @retval k_ra8_err_invalid_size @p name exceeds the ustar field.
 * @pre @p blk addresses one writable tar block and @p name is valid.
 * @pre @p size is representable by the emitted ustar size field.
 * @post Success initializes every byte of @p blk deterministically.
 * @post Failure never reports a truncated member name as success.
 * @note Thread-safe across distinct header buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t tar_header(uint8_t* blk, const char* name, size_t size)
{
  if (strlen(name) >= (size_t)k_len_name) {
    /* The ustar name field is 100 bytes; a longer name would silently truncate
     * and two distinct pages could collide onto one entry. Refuse instead. */
    return k_ra8_err_invalid_size;
  }
  memset(blk, 0, k_tar_block);
  (void)snprintf((char*)blk + k_off_name, k_len_name, "%s", name);
  (void)snprintf((char*)blk + k_off_mode, k_len_id, "%07o", (unsigned)k_file_mode);
  (void)snprintf((char*)blk + k_off_uid, k_len_id, "%07o", 0U);
  (void)snprintf((char*)blk + k_off_gid, k_len_id, "%07o", 0U);
  (void)snprintf((char*)blk + k_off_size, k_len_size, "%011zo", size);
  (void)snprintf((char*)blk + k_off_mtime, k_len_mtime, "%011o", 0U);
  blk[k_off_type] = '0';
  memcpy(blk + k_off_magic, k_ustar_magic, sizeof(k_ustar_magic));
  memcpy(blk + k_off_version, k_ustar_version, sizeof(k_ustar_version));

  memset(blk + k_off_chksum, ' ', k_len_chksum);
  unsigned sum = 0U;
  for (size_t i = 0U; i < (size_t)k_tar_block; ++i) {
    sum += blk[i];
  }
  (void)snprintf((char*)blk + k_off_chksum, k_len_chksum - 1U, "%06o", sum);
  blk[k_off_chksum + k_len_chksum - 1U] = ' ';
  return k_ra8_ok;
}

/** @brief Streaming copy-buffer size (bounds the per-file tar/gzip working
 * set). */
typedef enum : uint32_t {
  k_stream_chunk = 65536U, /**< File-copy chunk in bytes. */
} mdl_stream_t;

/**
 * @brief Copy exactly `size` bytes from open `in` to `out`, padding to a block.
 * @details Reads through a fixed chunk buffer and writes straight to @p out, so
 *          no whole-file buffer is ever held. Copies precisely the byte count
 *          the header was written with, so a file that grew after `fstat`
 *          cannot spill past the entry and one that shrank fails loudly.
 * @param[in,out] in Open source file positioned at payload start.
 * @param[in,out] out Open tar output file.
 * @param[in] size Exact payload byte count from fstat.
 * @return Copy status.
 * @retval k_ra8_ok Exactly @p size bytes plus zero padding were written.
 * @retval k_ra8_fail Reading or writing failed.
 * @pre Both FILE pointers are valid and open in binary mode.
 * @pre @p size matches the header already emitted for this member.
 * @post Success advances @p out to the next tar block boundary.
 * @post Failure is returned on a short source read.
 * @note Not thread-safe for shared FILE streams.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t tar_copy_file(FILE* in, FILE* out, size_t size)
{
  uint8_t chunk[k_stream_chunk];
  size_t  remaining = size;
  while (remaining > 0U) {
    const size_t want = (remaining < sizeof(chunk)) ? remaining : sizeof(chunk);
    const size_t got  = fread(chunk, 1U, want, in);
    if ((got == 0U) || (fwrite(chunk, 1U, got, out) != got)) {
      return k_ra8_fail;
    }
    remaining -= got;
  }
  const size_t pad = round_block(size) - size;
  if (pad > 0U) {
    const uint8_t zeros[k_tar_block] = {};
    if (fwrite(zeros, 1U, pad, out) != pad) {
      return k_ra8_fail;
    }
  }
  return k_ra8_ok;
}
/**
 * @brief Write one bounded in-memory tar member and its padding
 * @details Emits a deterministic header, the complete payload, and zero bytes
 *          through the next record boundary.
 * @param[in,out] out Open tar output file.
 * @param[in] name NUL-terminated member name.
 * @param[in] data Payload bytes.
 * @param[in] len Readable payload length.
 * @return Member-write status.
 * @retval k_ra8_ok Header, payload, and padding were written.
 * @retval k_ra8_err_invalid_size The member name exceeds ustar bounds.
 * @retval k_ra8_fail File output failed.
 * @pre @p out is open for binary writing and @p name is valid.
 * @pre @p data addresses @p len readable bytes.
 * @post Success leaves @p out on a tar block boundary.
 * @post Failure remains visible to the caller.
 * @note Not thread-safe for a shared FILE.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
tar_write_mem(FILE* out, const char* name, const void* data, size_t len)
{
  uint8_t         hdr[k_tar_block];
  const ra8_err_t hrc = tar_header(hdr, name, len);
  if ((hrc != k_ra8_ok) || (fwrite(hdr, 1U, sizeof(hdr), out) != sizeof(hdr)) ||
      (fwrite(data, 1U, len, out) != len)) {
    return (hrc == k_ra8_ok) ? k_ra8_fail : hrc;
  }
  const uint8_t zeros[k_tar_block] = {};
  const size_t  pad                = round_block(len) - len;
  return ((pad == 0U) || (fwrite(zeros, 1U, pad, out) == pad)) ? k_ra8_ok : k_ra8_fail;
}

/**
 * @brief Stream chapter pages and ComicInfo into an open ustar
 * @details Sizes each open source descriptor, copies exactly that size, appends
 *          generated metadata, and terminates with two zero records.
 * @param[in] dir NUL-terminated chapter directory.
 * @param[in] names Sorted page-name rows.
 * @param[in] count Number of rows to archive.
 * @param[in] meta Metadata to encode, or NULL for defaults.
 * @param[in,out] out Open tar output file.
 * @return Archive-stream status.
 * @retval k_ra8_ok Every page, metadata member, and trailer was written.
 * @retval k_ra8_err_invalid_size A member or metadata bound was exceeded.
 * @retval k_ra8_fail Opening, reading, or writing failed.
 * @pre Inputs are non-NULL except optional @p meta.
 * @pre @p out is open for binary writing at archive offset zero.
 * @post Success leaves @p out after the complete tar trailer.
 * @post Failure never masquerades as a complete archive.
 * @note Not thread-safe against page mutation or a shared output stream.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t build_tar_to_file(const char*              dir,
                                                char                     names[][k_name_max],
                                                size_t                   count,
                                                const mdl_export_meta_t* meta,
                                                FILE*                    out)
{
  for (size_t i = 0U; i < count; ++i) {
    char path[PATH_MAX];
    (void)snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
      return k_ra8_fail;
    }
    struct stat st = {};
    if (fstat(fileno(f), &st) != 0) {
      (void)fclose(f);
      return k_ra8_fail;
    }
    /* One fstat on the open descriptor sizes the header and bounds the copy, so
     * the two never disagree (a file racing our output cannot overflow). */
    const size_t    sz = (size_t)st.st_size;
    uint8_t         hdr[k_tar_block];
    const ra8_err_t hrc = tar_header(hdr, names[i], sz);
    if (hrc != k_ra8_ok) {
      (void)fclose(f);
      return hrc;
    }
    ra8_err_t rc = (fwrite(hdr, 1U, sizeof(hdr), out) == sizeof(hdr)) ? k_ra8_ok : k_ra8_fail;
    if (rc == k_ra8_ok) {
      rc = tar_copy_file(f, out, sz);
    }
    (void)fclose(f);
    if (rc != k_ra8_ok) {
      return rc;
    }
  }
  uint8_t         trailer[2U * (size_t)k_tar_block] = {}; /* two trailing zero blocks */
  char            comic_xml[4096];
  const ra8_err_t meta_rc =
    mdl_export_build_comicinfo_pages(meta, count, comic_xml, sizeof(comic_xml));
  if (meta_rc != k_ra8_ok) {
    return meta_rc;
  }
  const ra8_err_t xml_rc = tar_write_mem(out, "ComicInfo.xml", comic_xml, strlen(comic_xml));
  if (xml_rc != k_ra8_ok) {
    return xml_rc;
  }
  return (fwrite(trailer, 1U, sizeof(trailer), out) == sizeof(trailer)) ? k_ra8_ok : k_ra8_fail;
}

/**
 * @brief Stream a ustar to `out_path`.
 * @details A partial file on failure is the CALLER's to discard: every path
 *          into here now writes a temp that ::export_atomic aborts, so a second
 *          remove() here would only race its own cleanup.
 * @param[in] dir NUL-terminated chapter directory.
 * @param[in] names Sorted page-name rows.
 * @param[in] count Number of rows to archive.
 * @param[in] out_path NUL-terminated temporary output path.
 * @param[in] meta Metadata to encode, or NULL.
 * @return Tar writer status.
 * @retval k_ra8_ok The complete tar was closed successfully.
 * @retval k_ra8_err_invalid_size A bounded member did not fit.
 * @retval k_ra8_fail File creation, streaming, or close failed.
 * @pre Paths and page rows remain valid for the call.
 * @pre The caller owns cleanup/publication of @p out_path.
 * @post The output FILE is closed on every opened path.
 * @post Success means the full archive and trailer reached storage.
 * @note Not thread-safe for the same output path.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t write_tar_file(const char*              dir,
                                             char                     names[][k_name_max],
                                             size_t                   count,
                                             const char*              out_path,
                                             const mdl_export_meta_t* meta)
{
  FILE* f = fopen(out_path, "wb");
  if (f == nullptr) {
    return k_ra8_fail;
  }
  ra8_err_t  rc       = build_tar_to_file(dir, names, count, meta, f);
  const bool close_ok = (fclose(f) == 0);
  if ((rc == k_ra8_ok) && !close_ok) {
    rc = k_ra8_fail;
  }
  return rc;
}

/**
 * @brief Append one little-endian 32-bit value to a file
 * @details Serializes explicitly so gzip trailers are host-endian independent.
 * @param[in,out] f Open binary output stream.
 * @param[in] v Value to serialize.
 * @return Whether all four bytes were written.
 * @retval true The complete value was appended.
 * @retval false The stream accepted fewer bytes.
 * @pre @p f is non-NULL and open for binary writing.
 * @pre The caller owns stream serialization.
 * @post Success advances @p f by four bytes.
 * @post No host-endian representation leaks into output.
 * @note Not thread-safe for a shared FILE.
 * @since 0.1.0
 */
RA8_INTERNAL static bool put_u32le(FILE* f, uint32_t v)
{
  uint8_t b[k_u32_bytes] = {};
  for (size_t i = 0U; i < (size_t)k_u32_bytes; ++i) {
    b[i] = (uint8_t)(v & (uint32_t)k_byte_mask);
    v >>= (uint32_t)k_byte_bits;
  }
  return fwrite(b, 1U, sizeof(b), f) == sizeof(b);
}

/**
 * @brief Append streaming DEFLATE output to a FILE
 * @details Adapts miniz's put callback to a bounded fwrite result.
 * @param[in] buf Compressed bytes produced by miniz.
 * @param[in] len Number of readable bytes at @p buf.
 * @param[in,out] user Open FILE pointer supplied to miniz.
 * @return Miniz callback success value.
 * @retval MZ_TRUE Every byte was written.
 * @retval MZ_FALSE A short write occurred.
 * @pre @p user is a FILE open for binary writing.
 * @pre @p buf addresses @p len readable bytes and @p len is nonnegative.
 * @post Success advances the file by @p len bytes.
 * @post No compressed bytes are retained.
 * @note Not thread-safe for a shared FILE.
 * @since 0.1.0
 */
RA8_INTERNAL static mz_bool gz_put(const void* buf, int len, void* user)
{
  FILE* f = (FILE*)user;
  return (fwrite(buf, 1U, (size_t)len, f) == (size_t)len) ? MZ_TRUE : MZ_FALSE;
}

/**
 * @brief gzip `in_path` to `out_path`, streaming miniz DEFLATE + RFC-1952
 * framing.
 * @details Feeds the source through a fixed chunk buffer into miniz's streaming
 *          deflator (its put-buf callback writes straight to `out`), so neither
 *          the source nor the compressed stream is ever held whole. CRC32 and
 *          ISIZE are accumulated incrementally across the chunks.
 * @param[in] in_path NUL-terminated source tar path.
 * @param[in] out_path NUL-terminated gzip output path.
 * @param[in,out] ws Caller-owned storage for the compressor state.
 * @return Compression status.
 * @retval k_ra8_ok A complete RFC 1952 stream was written and closed.
 * @retval k_ra8_err_invalid_size Workspace capacity is insufficient.
 * @retval k_ra8_fail File or compression operations failed.
 * @pre Paths are non-NULL, NUL-terminated, and stable.
 * @pre @p ws is exclusive and owns writable arena bytes.
 * @post Every opened FILE is closed.
 * @post Success includes CRC32 and ISIZE trailer fields.
 * @note Not thread-safe for a shared workspace or output path.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
gzip_file(const char* in_path, const char* out_path, mdl_export_workspace_t* ws)
{
  tdefl_compressor* d = (tdefl_compressor*)mdl_export_workspace_take(ws, sizeof(*d), 16U);
  if (d == nullptr) {
    return k_ra8_err_invalid_size;
  }
  FILE* in = fopen(in_path, "rb");
  if (in == nullptr) {
    return k_ra8_fail;
  }
  FILE* out = fopen(out_path, "wb");
  if (out == nullptr) {
    (void)fclose(in);
    return k_ra8_fail;
  }
  const uint8_t hdr[k_gzip_hdr_len] =
    {k_gz_id1, k_gz_id2, k_gz_cm, 0U, 0U, 0U, 0U, 0U, 0U, k_gz_os};
  bool     ok    = (fwrite(hdr, 1U, sizeof(hdr), out) == sizeof(hdr)) &&
                   (tdefl_init(d, gz_put, out, TDEFL_DEFAULT_MAX_PROBES) == TDEFL_STATUS_OKAY);
  uint32_t crc   = (uint32_t)MZ_CRC32_INIT;
  uint32_t isize = 0U; /* ISIZE = total input length mod 2^32 */
  uint8_t  chunk[k_stream_chunk];
  while (ok) {
    const size_t got = fread(chunk, 1U, sizeof(chunk), in);
    if (got == 0U) {
      break;
    }
    crc = (uint32_t)mz_crc32(crc, chunk, got);
    isize += (uint32_t)got;
    ok = (tdefl_compress_buffer(d, chunk, got, TDEFL_NO_FLUSH) == TDEFL_STATUS_OKAY);
  }
  if (ok && (ferror(in) != 0)) {
    ok = false;
  }
  if (ok && (tdefl_compress_buffer(d, nullptr, 0U, TDEFL_FINISH) != TDEFL_STATUS_DONE)) {
    ok = false;
  }
  ok = ok && put_u32le(out, crc) && put_u32le(out, isize);
  ok = (fclose(out) == 0) && ok;
  (void)fclose(in);
  return ok ? k_ra8_ok : k_ra8_fail;
}

/**
 * @brief Test whether an snprintf result fully fit its buffer
 * @details Rejects encoding errors and results equal to capacity because the
 *          terminating NUL would not fit.
 * @param[in] written Return value produced by snprintf.
 * @param[in] cap Destination capacity passed to snprintf.
 * @return Whether formatting completed without truncation.
 * @retval true @p written is nonnegative and below @p cap.
 * @retval false Formatting failed or truncated.
 * @pre @p cap matches the formatted destination.
 * @pre @p written is the unmodified snprintf result.
 * @post No state is modified.
 * @post A true result permits safe use of the destination string.
 * @note Thread-safe: this is a pure predicate.
 * @since 0.1.0
 */
RA8_INTERNAL static bool snprintf_fit(int written, size_t cap)
{
  return (written >= 0) && ((size_t)written < cap);
}

void mdl_meta_init(mdl_export_meta_t* meta)
{
  if (meta == nullptr) {
    return;
  }
  memset(meta, 0, sizeof(*meta));
  meta->cover_index       = -1;
  meta->reading_direction = k_mdl_read_ltr;
  (void)snprintf(meta->language, sizeof(meta->language), "en");
  meta->modified[0] = '\0';
}

/**
 * @brief Copy trimmed text without truncating fixed metadata storage
 * @details Removes leading/trailing whitespace and fails if the complete text
 *          plus NUL cannot fit, leaving the destination unchanged on overflow.
 * @param[out] dst Destination string buffer.
 * @param[in] cap Writable capacity of @p dst.
 * @param[in] src NUL-terminated source text.
 * @return Whether the complete trimmed text fit.
 * @retval true @p dst received a NUL-terminated copy.
 * @retval false Capacity is zero or the value is too long.
 * @pre @p dst and @p src are non-NULL and do not overlap.
 * @pre @p src is NUL-terminated.
 * @post Success writes exactly the trimmed text.
 * @post Failure performs no partial copy.
 * @note Thread-safe across distinct buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static bool str_copy_trimmed(char* dst, size_t cap, const char* src)
{
  while ((*src != '\0') && isspace((unsigned char)*src)) {
    src++;
  }
  size_t len = strlen(src);
  while ((len > 0U) && isspace((unsigned char)src[len - 1U])) {
    len--;
  }
  if ((cap == 0U) || (len >= cap)) {
    return false;
  }
  memcpy(dst, src, len);
  dst[len] = '\0';
  return true;
}

/**
 * @brief Parse one finite nonnegative decimal value strictly
 * @details Requires the entire trimmed string to parse without range errors.
 * @param[in] text NUL-terminated numeric text.
 * @param[out] out Parsed value on success.
 * @return Whether a valid value was parsed.
 * @retval true The complete input represents a finite nonnegative double.
 * @retval false Input is empty, malformed, negative, nonfinite, or out of range.
 * @pre @p text and @p out are non-NULL.
 * @pre @p text is NUL-terminated.
 * @post Success stores the parsed value in @p out.
 * @post Input text is unchanged.
 * @note Thread-safe subject to the C library strtod implementation.
 * @since 0.1.0
 */
RA8_INTERNAL static bool parse_double_strict(const char* text, double* out)
{
  errno              = 0;
  char*        end   = nullptr;
  const double value = strtod(text, &end);
  while ((end != nullptr) && isspace((unsigned char)*end)) {
    ++end;
  }
  if ((end == text) || (end == nullptr) || (*end != '\0') || (errno == ERANGE) ||
      !isfinite(value) || (value < 0.0)) {
    return false;
  }
  *out = value;
  return true;
}

/**
 * @brief Parse one bounded decimal cover index strictly
 * @details Accepts minus one as the unset sentinel through INT_MAX and rejects trailing junk.
 * @param[in] text NUL-terminated numeric text.
 * @param[out] out Parsed integer on success.
 * @return Whether a valid bounded index was parsed.
 * @retval true The complete input lies in the accepted range.
 * @retval false Input is malformed, below minus one, or above INT_MAX.
 * @pre @p text and @p out are non-NULL.
 * @pre @p text is NUL-terminated.
 * @post Success stores the parsed integer in @p out.
 * @post Input text is unchanged.
 * @note Thread-safe subject to the C library strtol implementation.
 * @since 0.1.0
 */
RA8_INTERNAL static bool parse_int_strict(const char* text, int* out)
{
  errno            = 0;
  char*      end   = nullptr;
  const long value = strtol(text, &end, k_decimal_radix);
  while ((end != nullptr) && isspace((unsigned char)*end)) {
    ++end;
  }
  if ((end == text) || (end == nullptr) || (*end != '\0') || (errno == ERANGE) || (value < -1L) ||
      (value > INT_MAX)) {
    return false;
  }
  *out = (int)value;
  return true;
}

/**
 * @brief Extract and unescape one bounded simple XML element
 * @details Finds either a plain or attributed opening tag, copies text through
 *          the matching close tag, decodes five XML entities, and never truncates.
 * @param[in] xml NUL-terminated XML document.
 * @param[in] tag NUL-terminated element name.
 * @param[out] out Destination text buffer.
 * @param[in] cap Writable capacity of @p out.
 * @return Whether extraction completed without overflow.
 * @retval true The tag was absent or its complete text fit.
 * @retval false Raw or decoded text exceeded a fixed bound.
 * @pre All pointers are non-NULL and inputs are NUL-terminated.
 * @pre @p out addresses @p cap writable bytes.
 * @post A present accepted tag leaves NUL-terminated decoded text.
 * @post Overflow is reported instead of silently truncating.
 * @note Thread-safe across distinct buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static bool parse_xml_tag(const char* xml, const char* tag, char* out, size_t cap)
{
  char open_tag[64];
  char close_tag[64];
  (void)snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
  (void)snprintf(close_tag, sizeof(close_tag), "</%s>", tag);

  const char* start = strstr(xml, open_tag);
  if (start == nullptr) {
    (void)snprintf(open_tag, sizeof(open_tag), "<%s ", tag);
    start = strstr(xml, open_tag);
    if (start != nullptr) {
      start = strchr(start, '>');
      if (start != nullptr) {
        start++;
      }
    }
  } else {
    start += strlen(open_tag);
  }
  if (start == nullptr) {
    return true;
  }
  const char* end = strstr(start, close_tag);
  if (end == nullptr) {
    return true;
  }
  size_t len = (size_t)(end - start);
  char   raw[k_mdl_meta_path_max + 1U];
  if (len >= sizeof(raw)) {
    return false;
  }
  memcpy(raw, start, len);
  raw[len] = '\0';

  size_t r = 0U;
  size_t w = 0U;
  while ((raw[r] != '\0') && (w + 1U < cap)) {
    if (raw[r] == '&') {
      if (strncmp(raw + r, "&amp;", k_xml_amp_entity_len) == 0) {
        out[w++] = '&';
        r += k_xml_amp_entity_len;
        continue;
      }
      if (strncmp(raw + r, "&lt;", 4) == 0) {
        out[w++] = '<';
        r += 4;
        continue;
      }
      if (strncmp(raw + r, "&gt;", 4) == 0) {
        out[w++] = '>';
        r += 4;
        continue;
      }
      if (strncmp(raw + r, "&quot;", 6) == 0) {
        out[w++] = '"';
        r += 6;
        continue;
      }
      if (strncmp(raw + r, "&apos;", 6) == 0) {
        out[w++] = '\'';
        r += 6;
        continue;
      }
    }
    out[w++] = raw[r++];
  }
  out[w] = '\0';
  return raw[r] == '\0';
}

ra8_err_t mdl_meta_parse(mdl_export_meta_t* meta, const char* text)
{
  if ((meta == nullptr) || (text == nullptr)) {
    return k_ra8_err_invalid_arg;
  }

  if ((strstr(text, "<ComicInfo") != nullptr) || (strstr(text, "<Title>") != nullptr) ||
      (strstr(text, "<Series>") != nullptr) || (strstr(text, "<Web>") != nullptr)) {
    char val[k_mdl_meta_path_max + 1U];
    val[0] = '\0';
    if (!parse_xml_tag(text, "Title", val, sizeof(val))) {
      return k_ra8_err_invalid_size;
    }
    if (val[0] != '\0') {
      if (!str_copy_trimmed(meta->chapter_title, sizeof(meta->chapter_title), val)) {
        return k_ra8_err_invalid_size;
      }
    }
    val[0] = '\0';
    if (!parse_xml_tag(text, "Series", val, sizeof(val))) {
      return k_ra8_err_invalid_size;
    }
    if (val[0] != '\0') {
      if (!str_copy_trimmed(meta->series_title, sizeof(meta->series_title), val)) {
        return k_ra8_err_invalid_size;
      }
    }
    val[0] = '\0';
    if (!parse_xml_tag(text, "Summary", val, sizeof(val))) {
      return k_ra8_err_invalid_size;
    }
    if (val[0] != '\0') {
      if (!str_copy_trimmed(meta->summary, sizeof(meta->summary), val)) {
        return k_ra8_err_invalid_size;
      }
    }
    val[0] = '\0';
    if (!parse_xml_tag(text, "Writer", val, sizeof(val))) {
      return k_ra8_err_invalid_size;
    }
    if (val[0] != '\0') {
      if (!str_copy_trimmed(meta->writer, sizeof(meta->writer), val)) {
        return k_ra8_err_invalid_size;
      }
    }
    val[0] = '\0';
    if (!parse_xml_tag(text, "Artist", val, sizeof(val))) {
      return k_ra8_err_invalid_size;
    }
    if (val[0] != '\0') {
      if (!str_copy_trimmed(meta->artist, sizeof(meta->artist), val)) {
        return k_ra8_err_invalid_size;
      }
    }
    val[0] = '\0';
    if (!parse_xml_tag(text, "Web", val, sizeof(val))) {
      return k_ra8_err_invalid_size;
    }
    if (val[0] != '\0') {
      if (!str_copy_trimmed(meta->source_url, sizeof(meta->source_url), val)) {
        return k_ra8_err_invalid_size;
      }
      const ra8_err_t source_rc = validate_source_url(meta->source_url);
      if (source_rc != k_ra8_ok) {
        meta->source_url[0] = '\0';
        return source_rc;
      }
    }
    val[0] = '\0';
    if (!parse_xml_tag(text, "Number", val, sizeof(val))) {
      return k_ra8_err_invalid_size;
    }
    if (val[0] != '\0') {
      if (!parse_double_strict(val, &meta->chapter_number)) {
        return k_ra8_err_invalid_arg;
      }
    }
    val[0] = '\0';
    if (!parse_xml_tag(text, "LanguageISO", val, sizeof(val))) {
      return k_ra8_err_invalid_size;
    }
    if (val[0] != '\0') {
      if (!str_copy_trimmed(meta->language, sizeof(meta->language), val)) {
        return k_ra8_err_invalid_size;
      }
    }
    val[0] = '\0';
    if (!parse_xml_tag(text, "Manga", val, sizeof(val))) {
      return k_ra8_err_invalid_size;
    }
    if (strcmp(val, "YesAndRightToLeft") == 0) {
      meta->reading_direction = k_mdl_read_rtl;
    } else if (strcmp(val, "No") == 0) {
      meta->reading_direction = k_mdl_read_ltr;
    }
    val[0] = '\0';
    if (!parse_xml_tag(text, "CoverImage", val, sizeof(val))) {
      return k_ra8_err_invalid_size;
    }
    if (val[0] != '\0') {
      if (!str_copy_trimmed(meta->cover_path, sizeof(meta->cover_path), val)) {
        return k_ra8_err_invalid_size;
      }
    }
    return k_ra8_ok;
  }

  const char* p = text;
  while (*p != '\0') {
    const char* line_end = strchr(p, '\n');
    size_t      line_len = (line_end != nullptr) ? (size_t)(line_end - p) : strlen(p);
    char        line[k_mdl_meta_path_max + k_meta_line_slack];
    if (line_len >= sizeof(line)) {
      return k_ra8_err_invalid_size;
    }
    memcpy(line, p, line_len);
    line[line_len] = '\0';

    p += line_len;
    if (*p == '\n') {
      p++;
    }

    char* s = line;
    while ((*s != '\0') && isspace((unsigned char)*s)) {
      s++;
    }
    if ((*s == '\0') || (*s == '#') || (*s == ';')) {
      continue;
    }

    char* sep = strchr(s, '=');
    if (sep == nullptr) {
      sep = strchr(s, ':');
    }
    if (sep == nullptr) {
      continue;
    }

    *sep            = '\0';
    const char* key = s;
    const char* val = sep + 1;

    char k[128];
    if (!str_copy_trimmed(k, sizeof(k), key)) {
      return k_ra8_err_invalid_size;
    }
    for (size_t i = 0U; k[i] != '\0'; ++i) {
      k[i] = (char)tolower((unsigned char)k[i]);
    }

    if ((strcmp(k, "series") == 0) || (strcmp(k, "series_title") == 0) ||
        (strcmp(k, "seriestitle") == 0) || (strcmp(k, "series title") == 0)) {
      if (!str_copy_trimmed(meta->series_title, sizeof(meta->series_title), val)) {
        return k_ra8_err_invalid_size;
      }
    } else if ((strcmp(k, "summary") == 0) || (strcmp(k, "description") == 0) ||
               (strcmp(k, "abstract") == 0)) {
      if (!str_copy_trimmed(meta->summary, sizeof(meta->summary), val)) {
        return k_ra8_err_invalid_size;
      }
    } else if ((strcmp(k, "writer") == 0) || (strcmp(k, "author") == 0) ||
               (strcmp(k, "creator") == 0)) {
      if (!str_copy_trimmed(meta->writer, sizeof(meta->writer), val)) {
        return k_ra8_err_invalid_size;
      }
    } else if ((strcmp(k, "artist") == 0) || (strcmp(k, "penciller") == 0) ||
               (strcmp(k, "illustrator") == 0)) {
      if (!str_copy_trimmed(meta->artist, sizeof(meta->artist), val)) {
        return k_ra8_err_invalid_size;
      }
    } else if ((strcmp(k, "chapter_title") == 0) || (strcmp(k, "chaptertitle") == 0) ||
               (strcmp(k, "chapter title") == 0) || (strcmp(k, "title") == 0)) {
      if (!str_copy_trimmed(meta->chapter_title, sizeof(meta->chapter_title), val)) {
        return k_ra8_err_invalid_size;
      }
    } else if ((strcmp(k, "source_url") == 0) || (strcmp(k, "source") == 0) ||
               (strcmp(k, "web") == 0)) {
      if (!str_copy_trimmed(meta->source_url, sizeof(meta->source_url), val)) {
        return k_ra8_err_invalid_size;
      }
      const ra8_err_t source_rc = validate_source_url(meta->source_url);
      if (source_rc != k_ra8_ok) {
        meta->source_url[0] = '\0';
        return source_rc;
      }
    } else if ((strcmp(k, "number") == 0) || (strcmp(k, "chapter_number") == 0) ||
               (strcmp(k, "chapternumber") == 0) || (strcmp(k, "chapter number") == 0) ||
               (strcmp(k, "num") == 0)) {
      char v[64];
      if (!str_copy_trimmed(v, sizeof(v), val)) {
        return k_ra8_err_invalid_size;
      }
      if (!parse_double_strict(v, &meta->chapter_number)) {
        return k_ra8_err_invalid_arg;
      }
    } else if ((strcmp(k, "cover") == 0) || (strcmp(k, "cover_path") == 0) ||
               (strcmp(k, "coverpath") == 0) || (strcmp(k, "cover path") == 0) ||
               (strcmp(k, "cover_image") == 0)) {
      if (!str_copy_trimmed(meta->cover_path, sizeof(meta->cover_path), val)) {
        return k_ra8_err_invalid_size;
      }
    } else if ((strcmp(k, "cover_index") == 0) || (strcmp(k, "coverindex") == 0) ||
               (strcmp(k, "cover index") == 0) || (strcmp(k, "cover_idx") == 0)) {
      char v[64];
      if (!str_copy_trimmed(v, sizeof(v), val)) {
        return k_ra8_err_invalid_size;
      }
      if (!parse_int_strict(v, &meta->cover_index)) {
        return k_ra8_err_invalid_arg;
      }
    } else if ((strcmp(k, "language") == 0) || (strcmp(k, "language_iso") == 0) ||
               (strcmp(k, "lang") == 0)) {
      if (!str_copy_trimmed(meta->language, sizeof(meta->language), val)) {
        return k_ra8_err_invalid_size;
      }
    } else if ((strcmp(k, "reading_direction") == 0) || (strcmp(k, "direction") == 0) ||
               (strcmp(k, "page_progression") == 0)) {
      char v[16];
      if (!str_copy_trimmed(v, sizeof(v), val)) {
        return k_ra8_err_invalid_size;
      }
      if (strcmp(v, "rtl") == 0) {
        meta->reading_direction = k_mdl_read_rtl;
      } else if (strcmp(v, "ltr") == 0) {
        meta->reading_direction = k_mdl_read_ltr;
      } else {
        return k_ra8_err_invalid_arg;
      }
    } else if ((strcmp(k, "identifier") == 0) || (strcmp(k, "uuid") == 0)) {
      if (!str_copy_trimmed(meta->identifier, sizeof(meta->identifier), val)) {
        return k_ra8_err_invalid_size;
      }
    } else if ((strcmp(k, "modified") == 0) || (strcmp(k, "date_modified") == 0)) {
      if (!str_copy_trimmed(meta->modified, sizeof(meta->modified), val)) {
        return k_ra8_err_invalid_size;
      }
    }
  }

  return k_ra8_ok;
}

ra8_err_t mdl_meta_load_dir(mdl_export_meta_t* meta, const char* dir)
{
  if ((meta == nullptr) || (dir == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  mdl_meta_init(meta);

  static const char* const candidate_files[] = {"metadata.txt",
                                                "ComicInfo.xml",
                                                ".mdl_meta",
                                                "metadata.conf",
                                                "../metadata.txt"};

  for (size_t i = 0U; i < (sizeof(candidate_files) / sizeof(candidate_files[0])); ++i) {
    char      path[PATH_MAX];
    const int path_len = snprintf(path, sizeof(path), "%s/%s", dir, candidate_files[i]);
    if (!snprintf_fit(path_len, sizeof(path))) {
      return k_ra8_err_invalid_size;
    }
    FILE* f = fopen(path, "r");
    if (f != nullptr) {
      char         buf[4096];
      const size_t got   = fread(buf, 1U, sizeof(buf) - 1U, f);
      const int    extra = fgetc(f);
      (void)fclose(f);
      if (extra != EOF) {
        return k_ra8_err_invalid_size;
      }
      if (got > 0U) {
        buf[got]           = '\0';
        const ra8_err_t rc = mdl_meta_parse(meta, buf);
        if (rc != k_ra8_ok) {
          return rc;
        }
      }
    }
  }
  return k_ra8_ok;
}

ra8_err_t mdl_export_build_comicinfo_pages(const mdl_export_meta_t* meta,
                                           size_t                   page_count,
                                           char*                    buf,
                                           size_t                   cap)
{
  if ((buf == nullptr) || (cap == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  mdl_export_meta_t m;
  if (meta != nullptr) {
    m = *meta;
  } else {
    mdl_meta_init(&m);
  }
  const ra8_err_t source_rc = validate_source_url(m.source_url);
  if (source_rc != k_ra8_ok) {
    return source_rc;
  }

  char esc_title[k_mdl_meta_title_max * 6U];
  char esc_series[k_mdl_meta_title_max * 6U];
  char esc_summary[k_mdl_meta_summary_max * 6U];
  char esc_writer[k_mdl_meta_name_max * 6U];
  char esc_artist[k_mdl_meta_name_max * 6U];
  char esc_language[k_mdl_meta_lang_max * 6U];
  char esc_source[k_mdl_meta_url_max * 6U];

  const char* raw_title  = (m.chapter_title[0] != '\0')
                             ? m.chapter_title
                             : ((m.series_title[0] != '\0') ? m.series_title : "Chapter");
  const char* raw_series = (m.series_title[0] != '\0') ? m.series_title : "Series";

  if (!mdl_xml_escape(raw_title, esc_title, sizeof(esc_title))) {
    (void)snprintf(esc_title, sizeof(esc_title), "Chapter");
  }
  if (!mdl_xml_escape(raw_series, esc_series, sizeof(esc_series))) {
    (void)snprintf(esc_series, sizeof(esc_series), "Series");
  }
  if (!mdl_xml_escape(m.summary, esc_summary, sizeof(esc_summary))) {
    esc_summary[0] = '\0';
  }
  if (!mdl_xml_escape(m.writer, esc_writer, sizeof(esc_writer))) {
    esc_writer[0] = '\0';
  }
  if (!mdl_xml_escape(m.artist, esc_artist, sizeof(esc_artist))) {
    esc_artist[0] = '\0';
  }
  if (!mdl_xml_escape(m.language, esc_language, sizeof(esc_language))) {
    return k_ra8_err_invalid_size;
  }
  if (!mdl_xml_escape(m.source_url, esc_source, sizeof(esc_source))) {
    return k_ra8_err_invalid_size;
  }

  char web[k_mdl_meta_url_max * 6U + k_epub_fragment_slack];
  web[0] = '\0';
  if (esc_source[0] != '\0') {
    const int web_n = snprintf(web, sizeof(web), "  <Web>%s</Web>\n", esc_source);
    if (!snprintf_fit(web_n, sizeof(web))) {
      return k_ra8_err_invalid_size;
    }
  }

  char num_buf[32];
  if (m.chapter_number > 0.0) {
    if (m.chapter_number == (double)(long)m.chapter_number) {
      (void)snprintf(num_buf, sizeof(num_buf), "%ld", (long)m.chapter_number);
    } else {
      (void)snprintf(num_buf, sizeof(num_buf), "%.1f", m.chapter_number);
    }
  } else {
    (void)snprintf(num_buf, sizeof(num_buf), "1");
  }

  char pages[160];
  pages[0] = '\0';
  if ((m.cover_index >= 0) && ((size_t)m.cover_index < page_count)) {
    const int pn = snprintf(pages,
                            sizeof(pages),
                            "  <Pages><Page Image=\"%d\" Type=\"FrontCover\"/></Pages>\n",
                            m.cover_index);
    if (!snprintf_fit(pn, sizeof(pages))) {
      return k_ra8_err_invalid_size;
    }
  }
  const char* manga = (m.reading_direction == k_mdl_read_rtl) ? "YesAndRightToLeft" : "No";
  const int written = snprintf(buf,
                               cap,
                               "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                               "<ComicInfo xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
                               "xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\">\n"
                               "  <Title>%s</Title>\n"
                               "  <Series>%s</Series>\n"
                               "  <Number>%s</Number>\n"
                               "  <Summary>%s</Summary>\n"
                               "  <Writer>%s</Writer>\n"
                               "  <Artist>%s</Artist>\n"
                               "%s"
                               "  <PageCount>%zu</PageCount>\n"
                               "  <LanguageISO>%s</LanguageISO>\n"
                               "  <Manga>%s</Manga>\n"
                               "%s"
                               "</ComicInfo>",
                               esc_title,
                               esc_series,
                               num_buf,
                               esc_summary,
                               esc_writer,
                               esc_artist,
                               web,
                               page_count,
                               esc_language,
                               manga,
                               pages);

  return snprintf_fit(written, cap) ? k_ra8_ok : k_ra8_err_invalid_size;
}

ra8_err_t mdl_export_build_comicinfo(const mdl_export_meta_t* meta, char* buf, size_t cap)
{
  return mdl_export_build_comicinfo_pages(meta, 0U, buf, cap);
}

/**
 * @brief Mix one NUL-terminated metadata field into a UUID hash
 * @details Applies 64-bit FNV-1a and a separator byte so adjacent fields cannot
 *          collapse into the same concatenated hash stream.
 * @param[in] hash Incoming hash state.
 * @param[in] text NUL-terminated metadata field.
 * @return Updated hash state after the field and separator.
 * @retval uint64_t Deterministic updated FNV state.
 * @pre @p text is non-NULL and NUL-terminated.
 * @pre @p hash is the state for all preceding canonical fields.
 * @post @p text is not modified.
 * @post Equal input states and text produce equal output.
 * @note Thread-safe: this is a pure hash helper.
 * @since 0.1.0
 */
RA8_INTERNAL static uint64_t uuid_hash_text(uint64_t hash, const char* text)
{
  while (*text != '\0') {
    hash ^= (uint8_t)*text++;
    hash *= (uint64_t)k_uuid_fnv_prime;
  }
  hash ^= (uint8_t)k_uuid_separator;
  return hash * (uint64_t)k_uuid_fnv_prime;
}

/**
 * @brief Derive a stable RFC-4122-shaped identifier from canonical metadata
 * @details Hashes canonical fields in both directions and sets version-five and
 *          RFC variant bits before formatting a URN into caller storage.
 * @param[out] out Destination UUID string buffer.
 * @param[in] cap Writable capacity of @p out.
 * @param[in] meta Metadata to hash, or NULL for defaults.
 * @pre @p out is non-NULL and addresses @p cap writable bytes.
 * @pre @p meta is NULL or contains bounded NUL-terminated fields.
 * @post @p out contains a deterministic NUL-terminated UUID URN when capacity permits.
 * @post Input metadata remains unchanged.
 * @note Thread-safe across distinct output buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static void mdl_generate_uuid(char* out, size_t cap, const mdl_export_meta_t* meta)
{
  mdl_export_meta_t m;
  if (meta != nullptr) {
    m = *meta;
  } else {
    mdl_meta_init(&m);
  }
  const char* fields[] =
    {m.series_title, m.chapter_title, m.writer, m.artist, m.language, m.source_url, m.cover_path};
  uint64_t h1 = (uint64_t)k_uuid_seed_one;
  uint64_t h2 = (uint64_t)k_uuid_seed_two;
  for (size_t i = 0U; i < (sizeof(fields) / sizeof(fields[0])); ++i) {
    h1 = uuid_hash_text(h1, fields[i]);
    h2 = uuid_hash_text(h2, fields[(sizeof(fields) / sizeof(fields[0])) - 1U - i]);
  }
  uint8_t b[k_uuid_byte_count];
  for (size_t i = 0U; i < (size_t)k_uuid_half_bytes; ++i) {
    b[i] = (uint8_t)(h1 >> ((uint8_t)k_uuid_top_shift - (i * (size_t)k_byte_bits)));
    b[i + (size_t)k_uuid_half_bytes] =
      (uint8_t)(h2 >> ((uint8_t)k_uuid_top_shift - (i * (size_t)k_byte_bits)));
  }
  b[k_uuid_version_byte] =
    (uint8_t)((b[k_uuid_version_byte] & k_uuid_version_mask) | k_uuid_version_five);
  b[k_uuid_variant_byte] =
    (uint8_t)((b[k_uuid_variant_byte] & k_uuid_variant_mask) | k_uuid_variant_rfc4122);
  (void)snprintf(out,
                 cap,
                 "urn:uuid:%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%"
                 "02x%02x%02x%02x%02x",
                 b[0],
                 b[1],
                 b[2],
                 b[3],
                 b[4],
                 b[5],
                 b[6],
                 b[7],
                 b[8],
                 b[9],
                 b[k_uuid_node_offset],
                 b[k_uuid_node_offset + 1U],
                 b[k_uuid_node_offset + 2U],
                 b[k_uuid_node_offset + 3U],
                 b[k_uuid_node_offset + 4U],
                 b[k_uuid_last_byte]);
}

/**
 * @brief Validate and canonicalize a cover not already present as a page
 * @details Recognizes an in-chapter page by exact name; otherwise requires a
 *          stable regular image file, sniffs its bytes, and hides the trusted
 *          host path behind `cover/cover.<actual-type>`.
 * @param[in] meta Metadata carrying the trusted cover path, or NULL.
 * @param[in] names Sorted chapter page rows.
 * @param[in] count Number of readable rows.
 * @param[out] cover Canonical external-cover descriptor.
 * @return Cover classification status.
 * @retval k_ra8_ok No external cover is needed or one was validated.
 * @retval k_ra8_err_invalid_arg The fixed path lacks a terminating NUL.
 * @retval k_ra8_err_invalid_size The canonical member path does not fit.
 * @retval k_ra8_err_validation_failed The source is absent, nonregular, or not an image.
 * @pre @p cover and @p names are valid for @p count rows.
 * @pre @p meta is NULL or exclusively stable for the call.
 * @post Success fully initializes @p cover.
 * @post An external source path is never copied into an archive member name.
 * @note Not thread-safe against concurrent replacement of the cover file.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t prepare_external_cover(const mdl_export_meta_t* meta,
                                                     char                     names[][k_name_max],
                                                     size_t                   count,
                                                     mdl_external_cover_t*    cover)
{
  memset(cover, 0, sizeof(*cover));
  if ((meta == nullptr) || (meta->cover_path[0] == '\0')) {
    return k_ra8_ok;
  }
  if (strnlen(meta->cover_path, sizeof(meta->cover_path)) == sizeof(meta->cover_path)) {
    return k_ra8_err_invalid_arg;
  }
  for (size_t i = 0U; i < count; ++i) {
    if (strcmp(meta->cover_path, names[i]) == 0) {
      return k_ra8_ok;
    }
  }

  struct stat st;
  if ((stat(meta->cover_path, &st) != 0) || !S_ISREG(st.st_mode) || (st.st_size <= 0)) {
    return k_ra8_err_validation_failed;
  }
  char ext[k_cover_ext_bytes];
  if (!mdl_urlname_sniff_file(meta->cover_path,
                              nullptr,
                              ext,
                              sizeof(ext),
                              cover->mime,
                              sizeof(cover->mime))) {
    return k_ra8_err_validation_failed;
  }
  const int written = snprintf(cover->entry, sizeof(cover->entry), "cover/cover.%s", ext);
  if (!snprintf_fit(written, sizeof(cover->entry))) {
    return k_ra8_err_invalid_size;
  }
  cover->source   = meta->cover_path;
  cover->external = true;
  return k_ra8_ok;
}

/**
 * @brief Write a CBZ with pages, canonical cover, and ComicInfo metadata
 * @details Stores each page without recompression, inserts an external cover
 *          first when present, and finalizes only after ComicInfo succeeds.
 * @param[in] dir NUL-terminated chapter directory.
 * @param[in] names Sorted page-name rows.
 * @param[in] count Number of pages to archive.
 * @param[in] out_path NUL-terminated temporary CBZ path.
 * @param[in] meta Metadata to embed, or NULL.
 * @param[in,out] ws Exclusive caller-owned workspace.
 * @return CBZ writer status.
 * @retval k_ra8_ok The ZIP central directory finalized successfully.
 * @retval k_ra8_err_validation_failed An external cover is invalid.
 * @retval k_ra8_err_invalid_size Metadata or a bounded name does not fit.
 * @retval k_ra8_fail ZIP creation, member writing, or finalization failed.
 * @pre Paths and name rows are valid and stable.
 * @pre The caller owns publication or cleanup of @p out_path.
 * @post Success leaves a finalized CBZ with ComicInfo.
 * @post External cover paths are represented only by canonical member names.
 * @note Not thread-safe for the same output or changing source files.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t export_cbz(const char*              dir,
                                         char                     names[][k_name_max],
                                         size_t                   count,
                                         const char*              out_path,
                                         const mdl_export_meta_t* meta,
                                         mdl_export_workspace_t*  ws)
{
  mdl_external_cover_t cover;
  ra8_err_t            rc = prepare_external_cover(meta, names, count, &cover);
  if (rc != k_ra8_ok) {
    return rc;
  }
  mz_zip_archive      zip;
  mdl_zip_allocator_t zip_alloc;
  zip_workspace_bind(&zip, &zip_alloc, ws);
  if (mz_zip_writer_init_file(&zip, out_path, 0) /* alloc-allow: callbacks use caller arena */ ==
      MZ_FALSE) {
    const ra8_err_t zip_rc = zip_workspace_error(&zip_alloc);
    zip_workspace_release(&zip_alloc);
    return zip_rc;
  }
  if (cover.external && (mz_zip_writer_add_file(&zip, /* alloc-allow: callbacks use caller arena */
                                                cover.entry,
                                                cover.source,
                                                nullptr,
                                                0,
                                                MZ_NO_COMPRESSION) == MZ_FALSE)) {
    (void)mz_zip_writer_end(&zip); /* alloc-allow: releases caller-arena blocks */
    const ra8_err_t zip_rc = zip_workspace_error(&zip_alloc);
    zip_workspace_release(&zip_alloc);
    return zip_rc;
  }
  for (size_t i = 0U; i < count; ++i) {
    char src[PATH_MAX];
    (void)snprintf(src, sizeof(src), "%s/%s", dir, names[i]);
    if (mz_zip_writer_add_file(&zip, /* alloc-allow: callbacks use caller arena */
                               names[i],
                               src,
                               nullptr,
                               0,
                               MZ_NO_COMPRESSION) == MZ_FALSE) {
      (void)mz_zip_writer_end(&zip); /* alloc-allow: releases caller-arena blocks */
      const ra8_err_t zip_rc = zip_workspace_error(&zip_alloc);
      zip_workspace_release(&zip_alloc);
      return zip_rc;
    }
  }
  mdl_export_meta_t        cover_meta;
  const mdl_export_meta_t* comic_meta  = meta;
  size_t                   comic_pages = count;
  if (cover.external) {
    cover_meta             = *meta;
    cover_meta.cover_index = 0;
    comic_meta             = &cover_meta;
    comic_pages++;
  }
  char            comic_xml[4096];
  const ra8_err_t meta_rc =
    mdl_export_build_comicinfo_pages(comic_meta, comic_pages, comic_xml, sizeof(comic_xml));
  if ((meta_rc != k_ra8_ok) ||
      (mz_zip_writer_add_mem(&zip, /* alloc-allow: callbacks use caller arena */
                             "ComicInfo.xml",
                             comic_xml,
                             strlen(comic_xml),
                             MZ_NO_COMPRESSION) == MZ_FALSE)) {
    (void)mz_zip_writer_end(&zip); /* alloc-allow: releases caller-arena blocks */
    const ra8_err_t zip_rc = (meta_rc == k_ra8_ok) ? zip_workspace_error(&zip_alloc) : meta_rc;
    zip_workspace_release(&zip_alloc);
    return zip_rc;
  }
  const bool ok =
    (mz_zip_writer_finalize_archive(&zip) /* alloc-allow: callbacks use caller arena */ !=
     MZ_FALSE);
  (void)mz_zip_writer_end(&zip); /* alloc-allow: releases caller-arena blocks */
  const ra8_err_t zip_rc = ok ? k_ra8_ok : zip_workspace_error(&zip_alloc);
  zip_workspace_release(&zip_alloc);
  return zip_rc;
}

/**
 * @brief Stream a tar to `<out_path>.tar.tmp`, then `compress` it to out_path.
 * @details The tar is streamed to a sibling temp file (never held whole in
 * RAM), `compress` reads that file and streams its own output, and the temp
 *          file is removed on every exit path.
 */
RA8_INTERNAL static ra8_err_t export_tar_wrapped(const char*              dir,
                                                 char                     names[][k_name_max],
                                                 size_t                   count,
                                                 const char*              out_path,
                                                 const mdl_export_meta_t* meta,
                                                 ra8_err_t (*compress)(const char* in_path,
                                                                       const char* out_path,
                                                                       mdl_export_workspace_t* ws),
                                                 mdl_export_workspace_t* ws)
{
  /* A truncated suffix would name a DIFFERENT file than intended -- possibly
   * one that already exists -- so overflow aborts rather than proceeding. */
  char      tmp[PATH_MAX];
  const int n = snprintf(tmp, sizeof(tmp), "%s.tar.tmp", out_path);
  if ((n < 0) || ((size_t)n >= sizeof(tmp))) {
    return k_ra8_fail;
  }
  ra8_err_t rc = write_tar_file(dir, names, count, tmp, meta);
  if (rc == k_ra8_ok) {
    rc = compress(tmp, out_path, ws);
  }
  (void)remove(tmp);
  return rc;
}

/* --- EPUB (self-contained: a valid EPUB3 of the page images via miniz) ---- */

/**
 * @brief EPUB string-buffer sizing (grows with the page count).
 * @details Sized for the WORST case, not the typical `page_NNN.jpg`: a page
 *          filename may be up to ::k_name_max bytes and, XML-escaped, expand
 *          6x (a name of all `&quot;`). That escaped name is embedded once in
 *          the page's manifest fragment and once in its xhtml document, so both
 *          the fragment buffer and the per-page accumulator budget must exceed
 *          the fixed template text plus ::k_epub_name_esc_max. Undersizing here
 *          does not truncate silently -- ::str_cat and ::snprintf_fit report it
 *          and the export fails -- but correct sizing is what lets a legitimate
 *          long-name chapter package rather than error.
 */
typedef enum : uint32_t {
  k_epub_name_esc_max   = 1536U, /**< XML-escaped page name (k_name_max * 6).       */
  k_epub_frag_max       = 2048U, /**< One manifest fragment (fixed + escaped name). */
  k_epub_xhtml_max      = 2048U, /**< One page's xhtml document (embeds the name).  */
  k_epub_entry_max      = 320U,  /**< A zip entry path ("OEBPS/images/" + name).    */
  k_epub_base_bytes     = 4096U, /**< Fixed opf/nav overhead.                       */
  k_epub_per_page_bytes = 2048U, /**< Per-page opf/nav accumulator growth.          */
  k_epub_workspace_cap  = k_epub_base_bytes + (k_max_pages * k_epub_per_page_bytes),
  /**< Maximum bounded XML accumulator bytes. */
} mdl_epub_size_t;

/** @brief OCF container pointing at the OPF package (fixed). */
static const char* const k_epub_container_xml =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
  "<container version=\"1.0\" "
  "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles>"
  "<rootfile full-path=\"OEBPS/content.opf\" "
  "media-type=\"application/oebps-package+xml\"/></rootfiles></container>";

/** @brief Image media-type for a page file, inspecting magic bytes first then
 * extension. */
RA8_INTERNAL static const char* epub_media_type(const char* dir, const char* name)
{
  if (dir != nullptr) {
    char src[PATH_MAX];
    (void)snprintf(src, sizeof(src), "%s/%s", dir, name);
    char mime[64];
    if (mdl_urlname_sniff_file(src, nullptr, nullptr, 0, mime, sizeof(mime))) {
      if (strcmp(mime, "image/png") == 0) {
        return "image/png";
      }
      if (strcmp(mime, "image/gif") == 0) {
        return "image/gif";
      }
      if (strcmp(mime, "image/webp") == 0) {
        return "image/webp";
      }
      if (strcmp(mime, "image/jpeg") == 0) {
        return "image/jpeg";
      }
    }
  }
  const char* dot = strrchr(name, '.');
  if (dot != nullptr) {
    if ((strcmp(dot, ".png") == 0) || (strcmp(dot, ".PNG") == 0)) {
      return "image/png";
    }
    if ((strcmp(dot, ".gif") == 0) || (strcmp(dot, ".GIF") == 0)) {
      return "image/gif";
    }
    if ((strcmp(dot, ".webp") == 0) || (strcmp(dot, ".WEBP") == 0)) {
      return "image/webp";
    }
    if ((strcmp(dot, ".bmp") == 0) || (strcmp(dot, ".BMP") == 0)) {
      return "image/bmp";
    }
  }
  return "image/jpeg";
}

/**
 * @brief Append `text` to NUL-terminated `dst`; report whether it fully fit.
 * @details Never truncates: if the append (plus its NUL) would not fit in
 *          @p cap it leaves @p dst unchanged and returns false, so the caller
 *          can fail loudly rather than emit a manifest cut off mid-element.
 * @return true when the whole of @p text was appended, false if it would
 * overrun.
 * @param[in,out] dst Existing NUL-terminated accumulator.
 * @param[in] cap Total writable capacity of @p dst.
 * @param[in] text NUL-terminated text to append.
 * @retval true The complete text and terminator fit.
 * @retval false Capacity is insufficient and @p dst is unchanged.
 * @pre @p dst and @p text are non-NULL and do not overlap.
 * @pre @p dst is NUL-terminated within @p cap.
 * @post Success appends the complete source string.
 * @post Failure preserves the original destination.
 * @note Thread-safe across distinct accumulators.
 * @since 0.1.0
 */
RA8_INTERNAL static bool str_cat(char* dst, size_t cap, const char* text)
{
  const size_t cur = strlen(dst);
  const size_t add = strlen(text);
  if (cur + add + 1U > cap) {
    return false;
  }
  memcpy(dst + cur, text, add + 1U);
  return true;
}

/**
 * @brief Add an in-memory string as a stored ZIP entry
 * @details Passes the complete string length to miniz without compression.
 * @param[in,out] zip Initialized ZIP writer.
 * @param[in] name Safe NUL-terminated member name.
 * @param[in] body NUL-terminated member body.
 * @return Whether miniz accepted the complete member.
 * @retval true The entry was added.
 * @retval false The ZIP writer rejected it.
 * @pre All pointers are non-NULL and strings are NUL-terminated.
 * @pre @p zip remains initialized for writing.
 * @post Success adds exactly one stored member.
 * @post Input strings remain unchanged.
 * @note Not thread-safe for a shared ZIP writer.
 * @since 0.1.0
 */
RA8_INTERNAL static bool epub_add_str(mz_zip_archive* zip, const char* name, const char* body)
{
  return mz_zip_writer_add_mem(zip, /* alloc-allow: callbacks use caller arena */
                               name,
                               body,
                               strlen(body),
                               MZ_NO_COMPRESSION) != MZ_FALSE;
}

/**
 * @brief Store and declare a validated external EPUB cover
 * @details Writes the file under its canonical OEBPS path and appends one
 *          cover-image manifest item using byte-derived MIME data.
 * @param[in,out] zip Initialized EPUB ZIP writer.
 * @param[in] cover Prepared cover descriptor.
 * @param[in,out] mani NUL-terminated manifest accumulator.
 * @param[in] cap Capacity of @p mani.
 * @return Cover-addition status.
 * @retval k_ra8_ok No external cover was requested or it was added.
 * @retval k_ra8_fail A path, ZIP, or manifest operation failed.
 * @pre All pointers are non-NULL and @p mani is terminated within @p cap.
 * @pre An external descriptor has already passed content validation.
 * @post Success adds at most one canonical cover member and declaration.
 * @post The trusted host source path is not exposed in the manifest.
 * @note Not thread-safe for a shared ZIP writer or manifest buffer.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t epub_add_external_cover(mz_zip_archive*             zip,
                                                      const mdl_external_cover_t* cover,
                                                      char*                       mani,
                                                      size_t                      cap)
{
  if (!cover->external) {
    return k_ra8_ok;
  }
  char      archive_entry[k_epub_entry_max];
  const int entry_len = snprintf(archive_entry, sizeof(archive_entry), "OEBPS/%s", cover->entry);
  if (!snprintf_fit(entry_len, sizeof(archive_entry)) ||
      (mz_zip_writer_add_file(zip, /* alloc-allow: callbacks use caller arena */
                              archive_entry,
                              cover->source,
                              nullptr,
                              0,
                              MZ_NO_COMPRESSION) == MZ_FALSE)) {
    return k_ra8_fail;
  }
  char      frag[k_epub_frag_max];
  const int frag_len = snprintf(frag,
                                sizeof(frag),
                                "<item id=\"cover-image\" href=\"%s\" media-type=\"%s\" "
                                "properties=\"cover-image\"/>",
                                cover->entry,
                                cover->mime);
  return snprintf_fit(frag_len, sizeof(frag)) && str_cat(mani, cap, frag) ? k_ra8_ok : k_ra8_fail;
}

/**
 * @brief Append one page's manifest, spine, and navigation fragments
 * @details Builds all three bounded fragments from an escaped filename and
 *          marks the image manifest item when it is the logical cover.
 * @param[in,out] mani Manifest accumulator.
 * @param[in,out] spine Spine accumulator.
 * @param[in,out] nav Navigation accumulator.
 * @param[in] cap Capacity shared by all accumulators.
 * @param[in] esc_name XML-escaped image filename.
 * @param[in] media Image MIME string.
 * @param[in] idx Zero-based manifest identifier index.
 * @param[in] n One-based displayed page number.
 * @param[in] is_cover Whether to emit the cover-image property.
 * @return Fragment append status.
 * @retval k_ra8_ok All fragments fit.
 * @retval k_ra8_fail Formatting or any accumulator overflowed.
 * @pre All strings and accumulators are NUL-terminated within their bounds.
 * @pre @p mani, @p spine, and @p nav reference distinct writable buffers.
 * @post Success appends coherent identifiers across all three documents.
 * @post Failure is explicit and no truncated fragment is reported as valid.
 * @note Thread-safe across distinct accumulator sets.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t epub_append_frags(char*       mani,
                                                char*       spine,
                                                char*       nav,
                                                size_t      cap,
                                                const char* esc_name,
                                                const char* media,
                                                size_t      idx,
                                                unsigned    n,
                                                bool        is_cover)
{
  char        frag[k_epub_frag_max];
  const char* prop_attr = is_cover ? " properties=\"cover-image\"" : "";
  const int   fn        = snprintf(frag,
                                   sizeof(frag),
                                   "<item id=\"pg%zu\" href=\"page_%03u.xhtml\" "
                                   "media-type=\"application/xhtml+xml\"/>"
                                   "<item id=\"img%zu\" href=\"images/%s\" media-type=\"%s\"%s/>",
                                   idx,
                                   n,
                                   idx,
                                   esc_name,
                                   media,
                                   prop_attr);
  if (!snprintf_fit(fn, sizeof(frag))) {
    return k_ra8_fail;
  }
  if (!str_cat(mani, cap, frag)) {
    return k_ra8_fail;
  }
  (void)snprintf(frag, sizeof(frag), "<itemref idref=\"pg%zu\"/>", idx);
  if (!str_cat(spine, cap, frag)) {
    return k_ra8_fail;
  }
  (void)snprintf(frag, sizeof(frag), "<li><a href=\"page_%03u.xhtml\">Page %u</a></li>", n, n);
  if (!str_cat(nav, cap, frag)) {
    return k_ra8_fail;
  }
  return k_ra8_ok;
}

/**
 * @brief Add one fixed-layout EPUB page and its image
 * @details Escapes the source name, writes page XHTML and stored image members,
 *          derives the real MIME where possible, and appends package fragments.
 * @param[in,out] zip Initialized EPUB ZIP writer.
 * @param[in] dir Chapter directory.
 * @param[in] name Page filename.
 * @param[in] idx Zero-based page index.
 * @param[in,out] mani Manifest accumulator.
 * @param[in,out] spine Spine accumulator.
 * @param[in,out] nav Navigation accumulator.
 * @param[in] cap Capacity shared by accumulators.
 * @param[in] meta Metadata controlling cover selection, or NULL.
 * @return Page-addition status.
 * @retval k_ra8_ok Image, XHTML, and fragments were added.
 * @retval k_ra8_fail Escaping, formatting, ZIP writing, or append failed.
 * @pre Paths and accumulators are valid NUL-terminated strings.
 * @pre @p zip is initialized and accumulator buffers are distinct.
 * @post Success adds one XHTML and one image member.
 * @post Success appends matching manifest, spine, and navigation references.
 * @note Not thread-safe for a shared ZIP writer or buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t epub_add_page(mz_zip_archive*          zip,
                                            const char*              dir,
                                            const char*              name,
                                            size_t                   idx,
                                            char*                    mani,
                                            char*                    spine,
                                            char*                    nav,
                                            size_t                   cap,
                                            const mdl_export_meta_t* meta)
{
  const unsigned n = (unsigned)(idx + 1U);
  char           esc[k_epub_name_esc_max];
  if (!mdl_xml_escape(name, esc, sizeof(esc))) {
    return k_ra8_fail; /* untrusted filename must not break the container XML */
  }
  char      xhtml[k_epub_xhtml_max];
  const int xn = snprintf(xhtml,
                          sizeof(xhtml),
                          "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                          "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Page %u"
                          "</title></head><body><img src=\"images/%s\" alt=\"Page %u\"/>"
                          "</body></html>",
                          n,
                          esc,
                          n);
  if (!snprintf_fit(xn, sizeof(xhtml))) {
    return k_ra8_fail;
  }
  char entry[k_epub_entry_max];
  (void)snprintf(entry, sizeof(entry), "OEBPS/page_%03u.xhtml", n);
  if (!epub_add_str(zip, entry, xhtml)) {
    return k_ra8_fail;
  }
  char src[PATH_MAX];
  (void)snprintf(src, sizeof(src), "%s/%s", dir, name);
  const int en = snprintf(entry, sizeof(entry), "OEBPS/images/%s", name);
  if (!snprintf_fit(en, sizeof(entry))) {
    return k_ra8_fail;
  }
  if (mz_zip_writer_add_file(zip, /* alloc-allow: callbacks use caller arena */
                             entry,
                             src,
                             nullptr,
                             0,
                             MZ_NO_COMPRESSION) == MZ_FALSE) {
    return k_ra8_fail;
  }
  bool is_cover = false;
  if (meta != nullptr) {
    if ((meta->cover_index >= 0) && ((size_t)meta->cover_index == idx)) {
      is_cover = true;
    } else if ((meta->cover_path[0] != '\0') && (strcmp(name, meta->cover_path) == 0)) {
      is_cover = true;
    }
  }
  return epub_append_frags(mani,
                           spine,
                           nav,
                           cap,
                           esc,
                           epub_media_type(dir, name),
                           idx,
                           n,
                           is_cover);
}

/**
 * @brief Build EPUB package metadata and finalize the archive
 * @details Escapes deterministic metadata, composes bounded OPF/navigation
 *          documents, adds them to the ZIP, and writes the central directory.
 * @param[in,out] zip Initialized EPUB ZIP writer.
 * @param[in] mani Complete manifest fragments.
 * @param[in] spine Complete spine fragments.
 * @param[in] nav Complete navigation fragments.
 * @param[in] page_count Logical reading-order page count.
 * @param[in] meta Metadata to encode, or NULL.
 * @param[out] opf Caller workspace for content.opf.
 * @param[in] opf_buf_cap Capacity of @p opf.
 * @param[out] navdoc Caller workspace for nav.xhtml.
 * @param[in] nav_buf_cap Capacity of @p navdoc.
 * @return Metadata/finalization status.
 * @retval k_ra8_ok Both documents were added and ZIP finalized.
 * @retval k_ra8_err_invalid_size Required bounded XML storage is insufficient.
 * @retval k_ra8_fail Formatting, member addition, or finalization failed.
 * @pre All document strings are NUL-terminated and pointers are non-NULL.
 * @pre Output buffers are distinct and @p zip is initialized.
 * @post Success leaves a finalized EPUB central directory.
 * @post Failure is explicit and the caller owns temp cleanup.
 * @note Not thread-safe for a shared ZIP writer or buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t epub_add_meta(mz_zip_archive*          zip,
                                            const char*              mani,
                                            const char*              spine,
                                            const char*              nav,
                                            size_t                   page_count,
                                            const mdl_export_meta_t* meta,
                                            char*                    opf,
                                            size_t                   opf_buf_cap,
                                            char*                    navdoc,
                                            size_t                   nav_buf_cap)
{
  mdl_export_meta_t m;
  if (meta != nullptr) {
    m = *meta;
  } else {
    mdl_meta_init(&m);
  }
  const ra8_err_t source_rc = validate_source_url(m.source_url);
  if (source_rc != k_ra8_ok) {
    return source_rc;
  }

  char raw_id[k_mdl_meta_id_max];
  if (m.identifier[0] != '\0') {
    (void)snprintf(raw_id, sizeof(raw_id), "%s", m.identifier);
  } else {
    mdl_generate_uuid(raw_id, sizeof(raw_id), &m);
  }
  char uuid_str[k_mdl_meta_id_max * 6U];
  char esc_lang[k_mdl_meta_lang_max * 6U];
  char esc_modified[k_mdl_meta_date_max * 6U];
  if (!mdl_xml_escape(raw_id, uuid_str, sizeof(uuid_str)) ||
      !mdl_xml_escape(m.language, esc_lang, sizeof(esc_lang)) ||
      !mdl_xml_escape(m.modified, esc_modified, sizeof(esc_modified))) {
    return k_ra8_err_invalid_size;
  }
  const char* progression = (m.reading_direction == k_mdl_read_rtl) ? "rtl" : "ltr";

  char        esc_title[k_mdl_meta_title_max * 6U];
  const char* raw_title = (m.chapter_title[0] != '\0')
                            ? m.chapter_title
                            : ((m.series_title[0] != '\0') ? m.series_title : "chapter");
  if (!mdl_xml_escape(raw_title, esc_title, sizeof(esc_title))) {
    (void)snprintf(esc_title, sizeof(esc_title), "chapter");
  }

  char creators[k_mdl_meta_name_max * k_epub_creator_factor + k_epub_creator_slack];
  creators[0] = '\0';
  if (m.writer[0] != '\0') {
    char esc_w[k_mdl_meta_name_max * 6U];
    (void)mdl_xml_escape(m.writer, esc_w, sizeof(esc_w));
    char frag[k_mdl_meta_name_max * 6U + k_epub_fragment_slack];
    (void)snprintf(frag, sizeof(frag), "<dc:creator opf:role=\"aut\">%s</dc:creator>", esc_w);
    (void)str_cat(creators, sizeof(creators), frag);
  }
  if (m.artist[0] != '\0') {
    char esc_a[k_mdl_meta_name_max * 6U];
    (void)mdl_xml_escape(m.artist, esc_a, sizeof(esc_a));
    char frag[k_mdl_meta_name_max * 6U + k_epub_fragment_slack];
    (void)snprintf(frag, sizeof(frag), "<dc:creator opf:role=\"art\">%s</dc:creator>", esc_a);
    (void)str_cat(creators, sizeof(creators), frag);
  }
  if (creators[0] == '\0') {
    (void)snprintf(creators, sizeof(creators), "<dc:creator>media_dl</dc:creator>");
  }

  char desc[k_mdl_meta_summary_max * 6U + k_epub_fragment_slack];
  desc[0] = '\0';
  if (m.summary[0] != '\0') {
    char esc_s[k_mdl_meta_summary_max * 6U];
    (void)mdl_xml_escape(m.summary, esc_s, sizeof(esc_s));
    (void)snprintf(desc, sizeof(desc), "<dc:description>%s</dc:description>", esc_s);
  }

  char source[k_mdl_meta_url_max * 6U + k_epub_fragment_slack];
  source[0] = '\0';
  if (m.source_url[0] != '\0') {
    char esc_source[k_mdl_meta_url_max * 6U];
    if (!mdl_xml_escape(m.source_url, esc_source, sizeof(esc_source))) {
      return k_ra8_err_invalid_size;
    }
    const int source_n = snprintf(source, sizeof(source), "<dc:source>%s</dc:source>", esc_source);
    if (!snprintf_fit(source_n, sizeof(source))) {
      return k_ra8_err_invalid_size;
    }
  }

  const size_t opf_need = strlen(mani) + strlen(spine) + strlen(creators) + strlen(desc) +
                          strlen(source) + (size_t)k_epub_base_bytes;
  const size_t nav_need = strlen(nav) + (size_t)k_epub_base_bytes;
  if ((opf_need > opf_buf_cap) || (nav_need > nav_buf_cap)) {
    return k_ra8_err_invalid_size;
  }
  const size_t opf_cap = opf_need;
  const size_t nav_cap = nav_need;
  const int    opf_n =
    snprintf(opf,
             opf_cap,
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" "
             "unique-identifier=\"bookid\"><metadata "
             "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
             "xmlns:opf=\"http://www.idpf.org/2007/opf\">"
             "<dc:identifier id=\"bookid\">%s</dc:identifier>"
             "<dc:title>%s</dc:title>%s%s%s<dc:language>%s</dc:language>"
             "<meta property=\"dcterms:modified\">%s</meta>"
             "<meta property=\"schema:numberOfPages\">%zu</meta>"
             "</metadata><manifest><item id=\"nav\" href=\"nav.xhtml\" "
             "media-type=\"application/xhtml+xml\" properties=\"nav\"/>%s</manifest>"
             "<spine page-progression-direction=\"%s\">%s</spine></package>",
             uuid_str,
             esc_title,
             creators,
             desc,
             source,
             esc_lang,
             esc_modified,
             page_count,
             mani,
             progression,
             spine);
  const int  nav_n = snprintf(navdoc,
                              nav_cap,
                              "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                              "<html xmlns=\"http://www.w3.org/1999/xhtml\" "
                              "xmlns:epub=\"http://www.idpf.org/2007/ops\"><head><title>Contents"
                              "</title></head><body><nav epub:type=\"toc\"><ol>%s</ol></nav>"
                              "</body></html>",
                              nav);
  const bool ok =
    snprintf_fit(opf_n, opf_cap) && snprintf_fit(nav_n, nav_cap) &&
    epub_add_str(zip, "OEBPS/content.opf", opf) && epub_add_str(zip, "OEBPS/nav.xhtml", navdoc) &&
    (mz_zip_writer_finalize_archive(zip) /* alloc-allow: callbacks use caller arena */ != MZ_FALSE);
  return ok ? k_ra8_ok : k_ra8_fail;
}

/**
 * @brief Package chapter pages into a valid fixed-layout EPUB3
 * @details Carves all XML accumulators from caller storage, adds OCF roots,
 *          canonical cover, page members, metadata, and finalizes atomically upstream.
 * @param[in] dir Chapter directory.
 * @param[in] names Sorted page-name rows.
 * @param[in] count Number of page rows.
 * @param[in] out_path Temporary EPUB output path.
 * @param[in] meta Metadata to encode, or NULL.
 * @param[in,out] ws Exclusive caller-owned workspace.
 * @return EPUB writer status.
 * @retval k_ra8_ok A complete finalized EPUB was written.
 * @retval k_ra8_err_invalid_size Workspace or metadata bounds were exceeded.
 * @retval k_ra8_err_validation_failed External cover validation failed.
 * @retval k_ra8_fail ZIP, XHTML, or metadata writing failed.
 * @pre Paths and rows are valid, stable, and NUL-terminated.
 * @pre @p ws is exclusive and owns writable arena storage.
 * @post The ZIP writer is ended on every initialized path.
 * @post Success includes required mimetype, container, OPF, and navigation members.
 * @note Not thread-safe for shared output or workspace.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t export_epub(const char*              dir,
                                          char                     names[][k_name_max],
                                          size_t                   count,
                                          const char*              out_path,
                                          const mdl_export_meta_t* meta,
                                          mdl_export_workspace_t*  ws)
{
  mdl_external_cover_t cover;
  ra8_err_t            rc = prepare_external_cover(meta, names, count, &cover);
  if (rc != k_ra8_ok) {
    return rc;
  }
  const size_t cap    = (size_t)k_epub_base_bytes + (count * (size_t)k_epub_per_page_bytes);
  char* const  mani   = (char*)mdl_export_workspace_take(ws, cap, 1U);
  char* const  spine  = (char*)mdl_export_workspace_take(ws, cap, 1U);
  char* const  nav    = (char*)mdl_export_workspace_take(ws, cap, 1U);
  char* const  opf    = (char*)mdl_export_workspace_take(ws, cap, 1U);
  char* const  navdoc = (char*)mdl_export_workspace_take(ws, cap, 1U);
  if ((mani == nullptr) || (spine == nullptr) || (nav == nullptr) || (opf == nullptr) ||
      (navdoc == nullptr)) {
    return k_ra8_err_invalid_size;
  }
  mani[0]  = '\0';
  spine[0] = '\0';
  nav[0]   = '\0';
  mz_zip_archive      zip;
  mdl_zip_allocator_t zip_alloc;
  zip_workspace_bind(&zip, &zip_alloc, ws);
  const bool zip_open =
    (mz_zip_writer_init_file(&zip, out_path, 0) /* alloc-allow: callbacks use caller arena */ !=
     MZ_FALSE);
  rc = zip_open ? k_ra8_ok : zip_workspace_error(&zip_alloc);
  if ((rc == k_ra8_ok) && (!epub_add_str(&zip, "mimetype", "application/epub+zip") ||
                           !epub_add_str(&zip, "META-INF/container.xml", k_epub_container_xml))) {
    rc = k_ra8_fail;
  }
  if (rc == k_ra8_ok) {
    rc = epub_add_external_cover(&zip, &cover, mani, cap);
  }
  mdl_export_meta_t        page_meta;
  const mdl_export_meta_t* page_meta_ptr = meta;
  if (cover.external) {
    page_meta               = *meta;
    page_meta.cover_index   = -1;
    page_meta.cover_path[0] = '\0';
    page_meta_ptr           = &page_meta;
  }
  for (size_t i = 0U; (rc == k_ra8_ok) && (i < count); ++i) {
    rc = epub_add_page(&zip, dir, names[i], i, mani, spine, nav, cap, page_meta_ptr);
  }
  if (rc == k_ra8_ok) {
    rc = epub_add_meta(&zip, mani, spine, nav, count, meta, opf, cap, navdoc, cap);
  }
  if (zip_open) {
    (void)mz_zip_writer_end(&zip); /* alloc-allow: releases caller-arena blocks */
  }
  if ((rc == k_ra8_fail) && zip_alloc.exhausted) {
    rc = k_ra8_err_invalid_size;
  }
  zip_workspace_release(&zip_alloc);
  /* A partial EPUB is ::export_atomic's temp to discard, not ours. */
  return rc;
}

/**
 * @brief Dispatch one selected container writer
 * @details Centralizes format-to-writer mapping while preserving the shared
 *          metadata and caller-workspace contract.
 * @param[in] fmt Selected output format.
 * @param[in] dir Chapter directory.
 * @param[in] names Sorted page-name rows.
 * @param[in] count Page count.
 * @param[in] out_path Temporary output path.
 * @param[in] meta Metadata to embed, or NULL.
 * @param[in,out] ws Exclusive caller-owned workspace.
 * @return Selected writer status.
 * @retval k_ra8_ok The selected writer completed.
 * @retval k_ra8_err_invalid_arg The format has no container writer here.
 * @retval k_ra8_err_not_supported An optional writer is unavailable.
 * @retval k_ra8_fail The selected writer failed.
 * @pre Pointer arguments are valid for the selected format.
 * @pre @p ws is exclusive and owns writable arena bytes.
 * @post At most one writer is invoked.
 * @post The selected writer's status is returned unchanged.
 * @note Not thread-safe for shared output or workspace.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t export_dispatch(mdl_format_t             fmt,
                                              const char*              dir,
                                              char                     names[][k_name_max],
                                              size_t                   count,
                                              const char*              out_path,
                                              const mdl_export_meta_t* meta,
                                              mdl_export_workspace_t*  ws)
{
  switch (fmt) {
    case k_mdl_fmt_cbz:
      return export_cbz(dir, names, count, out_path, meta, ws);
    case k_mdl_fmt_cbt:
      return write_tar_file(dir, names, count, out_path, meta);
    case k_mdl_fmt_cbt_gz:
      return export_tar_wrapped(dir, names, count, out_path, meta, gzip_file, ws);
    case k_mdl_fmt_epub:
      return export_epub(dir, names, count, out_path, meta, ws);
    case k_mdl_fmt_cbr:
    case k_mdl_fmt_cbt_xz:
    case k_mdl_fmt_rabook:
      return k_ra8_err_not_supported;
    case k_mdl_fmt_jof:
    case k_mdl_fmt_loose:
    case k_mdl_fmt_invalid:
    default:
      return k_ra8_err_invalid_arg;
  }
}

/**
 * @brief Build `out_path` through a sibling temp, committing only on success.
 * @details The ONE atomicity seam for every container format, rather than seven
 *          writers each remembering. A re-export that fails part-way because
 *          caller storage is exhausted, metadata is invalid, or output I/O
 *          fails cannot truncate the previously-good archive: the destination
 *          is untouched until a complete copy exists. See mdl_atomic.h.
 * @param[in] fmt Selected output format.
 * @param[in] dir Chapter directory.
 * @param[in] names Sorted page-name rows.
 * @param[in] count Page count.
 * @param[in] out_path Final destination path.
 * @param[in] meta Metadata to embed, or NULL.
 * @param[in,out] ws Exclusive caller-owned workspace.
 * @return Atomic export status.
 * @retval k_ra8_ok The completed temp was durably committed.
 * @retval k_ra8_fail Temp reservation, writer, or commit failed.
 * @retval k_ra8_err_invalid_size A selected writer exceeded a bound.
 * @retval k_ra8_err_validation_failed Cover or container validation failed.
 * @pre Paths and page rows are valid and stable.
 * @pre @p ws is exclusive and remains alive through commit.
 * @post Failure aborts the reserved temp and preserves prior output.
 * @post Success publishes exactly one complete container.
 * @note Not thread-safe for the same destination or workspace.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t export_atomic(mdl_format_t             fmt,
                                            const char*              dir,
                                            char                     names[][k_name_max],
                                            size_t                   count,
                                            const char*              out_path,
                                            const mdl_export_meta_t* meta,
                                            mdl_export_workspace_t*  ws)
{
  char tmp_path[PATH_MAX];
  if (!mdl_atomic_tmp_path(out_path, tmp_path, sizeof(tmp_path))) {
    return k_ra8_fail;
  }
  const ra8_err_t rc = export_dispatch(fmt, dir, names, count, tmp_path, meta, ws);
  if (rc != k_ra8_ok) {
    mdl_atomic_abort(tmp_path);
    return rc;
  }
  return mdl_atomic_commit(tmp_path, out_path) ? k_ra8_ok : k_ra8_fail;
}

ra8_err_t mdl_export_chapter_meta_ws(mdl_format_t             fmt,
                                     const char*              chapter_dir,
                                     const char*              out_path,
                                     const mdl_export_meta_t* meta,
                                     mdl_export_workspace_t*  ws)
{
  if ((chapter_dir == nullptr) || (out_path == nullptr) || (fmt == k_mdl_fmt_loose) ||
      (fmt == k_mdl_fmt_invalid) || (ws == nullptr) || (ws->data == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  ws->used = 0U;
  if ((fmt == k_mdl_fmt_cbr) || (fmt == k_mdl_fmt_cbt_xz) || (fmt == k_mdl_fmt_rabook)) {
    return k_ra8_err_not_supported;
  }

  ws->high_water            = 0U;
  char (*names)[k_name_max] = mdl_export_workspace_take(ws,
                                                        (size_t)k_max_pages * (size_t)k_name_max,
                                                        _Alignof(char[k_name_max]));
  if (names == nullptr) {
    return k_ra8_err_invalid_size;
  }

  bool         truncated = false;
  const size_t count     = list_pages(chapter_dir, names, (size_t)k_max_pages, &truncated);
  if (truncated) {
    /* More page images than the fixed table holds: fail rather than silently
     * package a short chapter the reader would discover missing pages in. */
    return k_ra8_err_invalid_size;
  }
  if (count == 0U) {
    return k_ra8_err_empty;
  }
  mdl_export_meta_t resolved;
  if (meta != nullptr) {
    resolved = *meta;
  } else {
    mdl_meta_init(&resolved);
  }
  const ra8_err_t source_rc = validate_source_url(resolved.source_url);
  if (source_rc != k_ra8_ok) {
    return source_rc;
  }
  if ((resolved.modified[0] == '\0') &&
      !metadata_set_page_timestamp(&resolved, chapter_dir, names, count)) {
    return k_ra8_fail;
  }

  if (fmt == k_mdl_fmt_jof) {
    /* JOF writes one `.jof` sibling per page into chapter_dir; out_path names
     * no single container (see mdl_format_is_dir_output), so there is no single
     * file to rename into place -- mdl_export_jof commits each page itself. */
    return mdl_export_jof(chapter_dir, names, count, ws);
  }
  return export_atomic(fmt, chapter_dir, names, count, out_path, &resolved, ws);
}

ra8_err_t mdl_export_chapter_ws(mdl_format_t            fmt,
                                const char*             chapter_dir,
                                const char*             out_path,
                                mdl_export_workspace_t* ws)
{
  mdl_export_meta_t meta;
  if (chapter_dir != nullptr) {
    (void)mdl_meta_load_dir(&meta, chapter_dir);
  } else {
    mdl_meta_init(&meta);
  }
  return mdl_export_chapter_meta_ws(fmt, chapter_dir, out_path, &meta, ws);
}
