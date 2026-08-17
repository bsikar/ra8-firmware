/**
 * @file mdl_verify.c
 * @brief Portable bounded structural validators for media artifacts.
 * @details Reads only through an injected ::mdl_storage_t. ZIP uses miniz's
 *          positioned callback, JOF uses its production pread seam, and TAR
 *          plus gzip are validated incrementally without retaining archives.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_verify.h"

#include <ctype.h>
#include <limits.h>
#include <string.h>

#include "mdl_verify_rabook_internal.h"
#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_jof.h"

/** @brief POSIX tar field layout and record sizing. */
typedef enum : uint16_t {
  k_tar_name_bytes      = 100U, /**< USTAR name-field extent.                */
  k_tar_size_offset     = 124U, /**< USTAR size-field byte offset.           */
  k_tar_size_bytes      = 12U,  /**< USTAR size-field extent.                */
  k_tar_checksum_offset = 148U, /**< USTAR checksum-field byte offset.       */
  k_tar_checksum_end    = 156U, /**< First byte after the checksum field.    */
  k_tar_type_offset     = 156U, /**< USTAR type-flag byte offset.            */
  k_tar_block_bytes     = 512U, /**< TAR logical record extent.              */
  k_tar_padding_mask    = 511U, /**< Mask used to round payloads to records. */
} mdl_verify_tar_layout_t;

/** @brief Fixed RFC 1952 framing emitted and accepted by media_dl. */
typedef enum : uint32_t {
  k_gzip_id_one         = 0x1FU,   /**< First RFC 1952 magic byte.          */
  k_gzip_id_two         = 0x8BU,   /**< Second RFC 1952 magic byte.         */
  k_gzip_method_deflate = 8U,      /**< RFC 1952 DEFLATE method identifier. */
  k_gzip_header_bytes   = 10U,     /**< Fixed gzip header extent.           */
  k_gzip_trailer_bytes  = 8U,      /**< CRC32 plus ISIZE trailer extent.    */
  k_gzip_min_bytes      = 18U,     /**< Smallest fixed-frame gzip extent.   */
  k_verify_member_max   = 100000U, /**< Maximum archive member count.       */
} mdl_verify_limit_t;

/** @brief Byte shifts for little-endian decoding. */
typedef enum : uint8_t {
  k_u32_byte_one_shift   = 8U,  /**< Shift for byte one.   */
  k_u32_byte_two_shift   = 16U, /**< Shift for byte two.   */
  k_u32_byte_three_shift = 24U, /**< Shift for byte three. */
} mdl_verify_u32_shift_t;

/** @brief Header stored before each monotonic miniz allocation. */
typedef struct {
  size_t      bytes; /**< Payload extent.                   */
  max_align_t align; /**< Forces maximum payload alignment. */
} mdl_alloc_header_t;

/** @brief Allocation bridge retaining an explicit capacity failure. */
typedef struct {
  mdl_export_workspace_t* workspace; /**< Caller-owned bump arena.   */
  bool                    exhausted; /**< An allocation did not fit. */
} mdl_verify_arena_t;

/** @brief One open portable input and its immutable size snapshot. */
typedef struct {
  mdl_storage_t* storage;    /**< Borrowed storage binding.                */
  fw_fs_file_t*  file;       /**< Borrowed or locally owned facade handle. */
  fw_fs_file_t   owned_file; /**< Path wrapper's facade handle storage.    */
  uint64_t       size_bytes; /**< Length observed after open.              */
  ra8_err_t      read_error; /**< First random-reader failure.             */
  bool           owned;      /**< Whether close remains required.          */
} mdl_verify_io_t;

/** @brief Incremental TAR structural state. */
typedef struct {
  uint8_t  block[k_tar_block_bytes]; /**< Partial header record.         */
  uint64_t skip_bytes;               /**< Padded payload remaining.      */
  uint64_t total_bytes;              /**< Total decoded TAR bytes.       */
  size_t   block_used;               /**< Bytes resident in block.       */
  size_t   pages;                    /**< Image member count.            */
  size_t   members;                  /**< Regular member count.          */
  uint8_t  zero_blocks;              /**< Consecutive terminal records.  */
  bool     metadata;                 /**< ComicInfo.xml was found.       */
  bool     ended;                    /**< Two zero blocks were consumed. */
} mdl_tar_stream_t;

/** @brief Streaming gzip inflater and nested TAR consumer. */
typedef struct {
  mz_stream        stream;     /**< Raw-DEFLATE miniz state.       */
  mdl_tar_stream_t tar;        /**< Incremental decoded TAR state. */
  uint8_t*         output;     /**< One bounded decoded chunk.     */
  uint32_t         output_cap; /**< Extent of output.              */
  uint32_t         crc;        /**< Running decoded CRC32.         */
  uint64_t         raw_bytes;  /**< Exact decoded byte count.      */
  bool             ended;      /**< DEFLATE end marker observed.   */
} mdl_gzip_stream_t;

/** @brief Reserve one aligned span from the verifier's caller-owned arena. */
RA8_INTERNAL static void*
internal_workspace_take(mdl_export_workspace_t* workspace, size_t bytes, size_t alignment)
{
  if ((bytes == 0U) || (alignment == 0U) || ((alignment & (alignment - 1U)) != 0U)) {
    return nullptr;
  }
  const size_t mask = alignment - 1U;
  if (workspace->used > (SIZE_MAX - mask)) {
    return nullptr;
  }
  const size_t start = (workspace->used + mask) & ~mask;
  if ((start > workspace->cap) || (bytes > (workspace->cap - start))) {
    return nullptr;
  }
  workspace->used = start + bytes;
  if (workspace->used > workspace->high_water) {
    workspace->high_water = workspace->used;
  }
  return &workspace->data[start];
}

/**
 * @brief Test a suffix without case sensitivity. @details Compares only the tail of a valid string.
 * @param[in] text Candidate string. @param[in] suffix Required suffix. @return Whether the suffix matches. @retval true On a match.
 * @pre Both pointers address terminated strings. @pre Their lengths fit in size_t.
 * @post Neither string is modified. @post The result depends only on input bytes. @note ASCII suffixes are expected. @since v0.1.0
 */
RA8_INTERNAL static bool internal_ends_ci(const char* text, const char* suffix)
{
  const size_t text_len   = strlen(text);
  const size_t suffix_len = strlen(suffix);
  if (suffix_len > text_len) {
    return false;
  }
  for (size_t i = 0U; i < suffix_len; ++i) {
    const unsigned char left  = (unsigned char)text[text_len - suffix_len + i];
    const unsigned char right = (unsigned char)suffix[i];
    if (tolower(left) != tolower(right)) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Recognize supported image suffixes. @details Applies the archive image allowlist case-insensitively.
 * @param[in] name Archive member name. @return Whether its suffix is supported. @retval true For a supported image.
 * @pre name is a terminated string. @pre name remains valid for the call.
 * @post name is unchanged. @post No storage is accessed. @note Content magic is validated during export. @since v0.1.0
 */
RA8_INTERNAL static bool internal_is_image(const char* name)
{
  return internal_ends_ci(name, ".jpg") || internal_ends_ci(name, ".jpeg") ||
         internal_ends_ci(name, ".png") || internal_ends_ci(name, ".webp") ||
         internal_ends_ci(name, ".gif") || internal_ends_ci(name, ".bmp");
}

/**
 * @brief Reject unsafe archive member paths. @details Rejects absolute, empty, dot, parent, and backslash segments.
 * @param[in] name Archive member name. @return Whether the path is contained. @retval true For a safe relative path.
 * @pre name is null or terminated. @pre Non-null input remains valid for the call.
 * @post name is unchanged. @post No filesystem lookup occurs. @note This is a lexical containment check. @since v0.1.0
 */
RA8_INTERNAL static bool internal_safe_member_name(const char* name)
{
  if ((name == nullptr) || (name[0] == '\0') || (name[0] == '/') || (name[0] == '\\')) {
    return false;
  }
  const char* segment = name;
  for (const char* cursor = name;; ++cursor) {
    if ((*cursor == '\\') || (*cursor == '/') || (*cursor == '\0')) {
      const size_t bytes = (size_t)(cursor - segment);
      if ((*cursor == '\\') || (bytes == 0U) || ((bytes == 1U) && (segment[0] == '.')) ||
          ((bytes == 2U) && (segment[0] == '.') && (segment[1] == '.'))) {
        return false;
      }
      if (*cursor == '\0') {
        return true;
      }
      segment = cursor + 1;
    }
  }
}

ra8_err_t mdl_format_from_path(const char* path, ra8_mdl_format_t* out_format)
{
  if ((path == nullptr) || (out_format == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  static const struct {
    const char*      suffix; /**< Complete artifact suffix. */
    ra8_mdl_format_t format; /**< Corresponding format.     */
  } formats[] = {{".cbt.gz", k_ra8_mdl_format_cbt_gz},
                 {".rabook", k_ra8_mdl_format_rabook},
                 {".epub", k_ra8_mdl_format_epub},
                 {".cbz", k_ra8_mdl_format_cbz},
                 {".cbt", k_ra8_mdl_format_cbt},
                 {".jof", k_ra8_mdl_format_jof}};
  for (size_t i = 0U; i < (sizeof(formats) / sizeof(formats[0])); ++i) {
    if (internal_ends_ci(path, formats[i].suffix)) {
      *out_format = formats[i].format;
      return k_ra8_ok;
    }
  }
  *out_format = k_ra8_mdl_format_invalid;
  return k_ra8_err_not_supported;
}

bool mdl_format_is_verifiable(ra8_mdl_format_t format)
{
  return (format == k_ra8_mdl_format_cbz) || (format == k_ra8_mdl_format_cbt) ||
         (format == k_ra8_mdl_format_cbt_gz) || (format == k_ra8_mdl_format_epub) ||
         (format == k_ra8_mdl_format_jof) || (format == k_ra8_mdl_format_rabook);
}

/** @brief Allocate one aligned miniz span from a monotonic arena. */
RA8_INTERNAL static void* internal_arena_alloc(void* opaque, size_t items, size_t size)
{
  mdl_verify_arena_t* arena = (mdl_verify_arena_t*)opaque;
  if ((items != 0U) && (size > (SIZE_MAX / items))) {
    arena->exhausted = true;
    return nullptr;
  }
  const size_t bytes = items * size;
  if (bytes > (SIZE_MAX - sizeof(mdl_alloc_header_t))) {
    arena->exhausted = true;
    return nullptr;
  }
  mdl_alloc_header_t* header = (mdl_alloc_header_t*)internal_workspace_take(arena->workspace,
                                                                            sizeof(*header) + bytes,
                                                                            _Alignof(max_align_t));
  if (header == nullptr) {
    arena->exhausted = true;
    return nullptr;
  }
  header->bytes = bytes;
  return (void*)(header + 1);
}

/**
 * @brief Accept a miniz free for a monotonic arena. @details Individual allocations remain until workspace reset.
 * @param[in,out] opaque Borrowed arena context. @param[in] address Allocation being released.
 * @pre opaque is the callback context. @pre address is null or arena-owned.
 * @post Arena offsets are unchanged. @post address is not accessed. @note Miniz requires a free callback. @since v0.1.0
 */
RA8_INTERNAL static void internal_arena_free(void* opaque, void* address)
{
  (void)opaque;
  (void)address;
}

/** @brief Grow a miniz span by copying it to the next arena allocation. */
RA8_INTERNAL static void*
internal_arena_realloc(void* opaque, void* address, size_t items, size_t size)
{
  if (address == nullptr) {
    return internal_arena_alloc(opaque, items, size);
  }
  if ((items != 0U) && (size > (SIZE_MAX / items))) {
    ((mdl_verify_arena_t*)opaque)->exhausted = true;
    return nullptr;
  }
  mdl_alloc_header_t* previous = ((mdl_alloc_header_t*)address) - 1;
  void*               next     = internal_arena_alloc(opaque, items, size);
  if (next != nullptr) {
    const size_t next_bytes = items * size;
    memcpy(next, address, previous->bytes < next_bytes ? previous->bytes : next_bytes);
  }
  return next;
}

/**
 * @brief Open a portable read-only input. @details Opens through storage and snapshots the immutable validation size.
 * @param[in] storage Bound storage facade. @param[in] path Canonical bound-root path. @param[in,out] io Output state. @return Status. @retval k_ra8_ok On success.
 * @pre All pointers are valid. @pre storage workspaces satisfy its contract.
 * @post Success leaves io open. @post Failure leaves no owned file. @note The caller must close successful opens. @since v0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_io_open(mdl_storage_t* storage, const char* path, mdl_verify_io_t* io)
{
  *io             = (mdl_verify_io_t){.storage = storage, .read_error = k_ra8_ok};
  io->file        = &io->owned_file;
  ra8_err_t error = fw_fs_open(&storage->fs->streams,
                               path,
                               k_fw_fs_open_read,
                               io->file,
                               storage->file_workspace,
                               storage->file_workspace_bytes);
  if (error != k_ra8_ok) {
    return error;
  }
  io->owned = true;
  error     = fw_fs_file_size(io->file, &io->size_bytes);
  if (error != k_ra8_ok) {
    (void)fw_fs_close(io->file);
    io->owned = false;
  }
  return error;
}

/**
 * @brief Close an input without masking prior failure. @details A close error is returned only when prior succeeded.
 * @param[in,out] io Open input state. @param[in] prior Earlier operation status. @return Final status. @retval k_ra8_ok When both stages succeed.
 * @pre io was initialized by internal_io_open. @pre prior is a valid error code.
 * @post io is marked closed. @post The first failure remains observable. @note Close is attempted exactly once. @since v0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_io_close(mdl_verify_io_t* io, ra8_err_t prior)
{
  if (!io->owned) {
    return prior;
  }
  const ra8_err_t close_error = fw_fs_close(io->file);
  io->owned                   = false;
  return (prior == k_ra8_ok) ? close_error : prior;
}

/**
 * @brief Read up to a requested portable span. @details Repeats short reads and stops only on EOF or failure.
 * @param[in,out] io Open input. @param[out] destination Output buffer. @param[in] length Capacity. @param[out] out_read Produced byte count. @return Status. @retval k_ra8_ok On data or EOF.
 * @pre io is open. @pre destination spans length bytes.
 * @post out_read never exceeds length. @post Read failures remain in io. @note Zero bytes with success denotes EOF. @since v0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_io_read_up_to(mdl_verify_io_t* io, uint8_t* destination, size_t length, size_t* out_read)
{
  size_t total = 0U;
  while (total < length) {
    const size_t    remaining = length - total;
    const uint32_t  chunk     = (remaining > UINT32_MAX) ? UINT32_MAX : (uint32_t)remaining;
    uint32_t        got       = 0U;
    const ra8_err_t err       = fw_fs_read(io->file, destination + total, chunk, &got);
    if (err != k_ra8_ok) {
      *out_read = total;
      return err;
    }
    total += got;
    if (got == 0U) {
      break;
    }
  }
  *out_read = total;
  return k_ra8_ok;
}

/**
 * @brief Read one exact structural span. @details Maps a successful early EOF to validation failure.
 * @param[in,out] io Open input. @param[out] destination Output buffer. @param[in] length Required bytes. @return Status. @retval k_ra8_ok When length bytes arrive.
 * @pre io is open. @pre destination spans length bytes.
 * @post Success initializes the complete span. @post Short EOF is explicit corruption. @note Backend faults are preserved. @since v0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_io_read_exact(mdl_verify_io_t* io, uint8_t* destination, size_t length)
{
  size_t          got = 0U;
  const ra8_err_t err = internal_io_read_up_to(io, destination, length, &got);
  if (err != k_ra8_ok) {
    return err;
  }
  return (got == length) ? k_ra8_ok : k_ra8_err_validation_failed;
}

/**
 * @brief Adapt portable reads to miniz. @details Seeks and fills the bounded random-read request.
 * @param[in,out] opaque Verifier input. @param[in] offset File offset. @param[out] destination Output bytes. @param[in] length Requested bytes. @return Bytes read. @retval 0 On failure.
 * @pre opaque and destination are valid. @pre length fits the destination.
 * @post Reads stay within the size snapshot. @post The first fault is retained. @note Miniz observes faults as short reads. @since v0.1.0
 */
RA8_INTERNAL static size_t
internal_zip_read(void* opaque, mz_uint64 offset, void* destination, size_t length)
{
  mdl_verify_io_t* io = (mdl_verify_io_t*)opaque;
  if ((io->read_error != k_ra8_ok) || (offset > io->size_bytes)) {
    return 0U;
  }
  const uint64_t available = io->size_bytes - offset;
  if ((uint64_t)length > available) {
    length = (size_t)available;
  }
  ra8_err_t error = fw_fs_seek(io->file, offset);
  size_t    got   = 0U;
  if (error == k_ra8_ok) {
    error = internal_io_read_up_to(io, (uint8_t*)destination, length, &got);
  }
  if ((error == k_ra8_ok) && (got != length)) {
    error = k_ra8_err_validation_failed;
  }
  if (error != k_ra8_ok) {
    io->read_error = error;
  }
  return got;
}

/**
 * @brief Discard verified ZIP output. @details Supplies a bounded sink so miniz computes and checks member CRCs.
 * @param[in,out] opaque Unused callback context. @param[in] offset Output offset. @param[in] data Decoded bytes. @param[in] bytes Byte count. @return Accepted bytes. @retval bytes Always.
 * @pre data spans bytes bytes. @pre offset is supplied by miniz.
 * @post data is unchanged. @post No bytes are retained. @note This validates rather than extracts. @since v0.1.0
 */
RA8_INTERNAL static size_t
internal_discard_zip(void* opaque, mz_uint64 offset, const void* data, size_t bytes)
{
  (void)opaque;
  (void)offset;
  (void)data;
  return bytes;
}

/** @brief Semantic markers accumulated while scanning ZIP members. */
typedef struct {
  size_t members;   /**< Total member count.          */
  size_t pages;     /**< Image member count.          */
  bool   mimetype;  /**< EPUB mimetype found.         */
  bool   container; /**< EPUB container.xml found.    */
  bool   opf;       /**< EPUB package document found. */
  bool   nav;       /**< EPUB navigation found.       */
  bool   comicinfo; /**< CBZ ComicInfo.xml found.     */
} mdl_zip_scan_t;

/**
 * @brief Validate one ZIP member. @details Checks bounded safe names and forces CRC-checked extraction.
 * @param[in,out] zip Open miniz archive. @param[in] index Member index. @param[in,out] scan Semantic accumulator. @return Status. @retval k_ra8_ok For a safe valid member.
 * @pre zip and scan are valid. @pre index is in the archive range.
 * @post Success updates scan once. @post Failure retains no output. @note Directories are structural only. @since v0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_zip_member(mz_zip_archive* zip, mz_uint index, mdl_zip_scan_t* scan)
{
  mz_zip_archive_file_stat member;
  if (mz_zip_reader_file_stat(zip, index, &member) == MZ_FALSE) {
    return k_ra8_err_validation_failed;
  }
  if (!internal_safe_member_name(member.m_filename)) {
    return k_ra8_err_validation_failed;
  }
  if (!member.m_is_directory &&
      (mz_zip_reader_extract_to_callback(zip, index, internal_discard_zip, nullptr, 0) ==
       MZ_FALSE)) {
    return k_ra8_err_validation_failed;
  }
  const char* name = member.m_filename;
  scan->pages += internal_is_image(name) ? 1U : 0U;
  scan->comicinfo = scan->comicinfo || internal_ends_ci(name, "ComicInfo.xml");
  scan->mimetype  = scan->mimetype || (strcmp(name, "mimetype") == 0);
  scan->container = scan->container || (strcmp(name, "META-INF/container.xml") == 0);
  scan->opf       = scan->opf || internal_ends_ci(name, ".opf");
  scan->nav       = scan->nav || internal_ends_ci(name, "nav.xhtml");
  return k_ra8_ok;
}

/**
 * @brief Enforce ZIP format semantics. @details Distinguishes CBZ image policy from EPUB mimetype requirements.
 * @param[in] format Requested format. @param[in,out] scan Completed scan. @param[in,out] report Candidate report. @return Status. @retval k_ra8_ok When required members exist.
 * @pre scan and report are valid. @pre format is CBZ or EPUB.
 * @post Success publishes semantic counts. @post Failure does not touch caller output. @note ZIP structure is already verified. @since v0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_zip_semantics(ra8_mdl_format_t      format,
                                                     const mdl_zip_scan_t* scan,
                                                     mdl_verify_report_t*  report)
{
  if ((scan->members == 0U) || (scan->pages == 0U)) {
    return k_ra8_err_validation_failed;
  }
  if ((format == k_ra8_mdl_format_cbz) && !scan->comicinfo) {
    return k_ra8_err_validation_failed;
  }
  if ((format == k_ra8_mdl_format_epub) &&
      !(scan->mimetype && scan->container && scan->opf && scan->nav)) {
    return k_ra8_err_validation_failed;
  }
  report->page_count       = scan->pages;
  report->member_count     = scan->members;
  report->metadata_present = (format == k_ra8_mdl_format_cbz) ? scan->comicinfo : scan->opf;
  return k_ra8_ok;
}

/**
 * @brief Validate a ZIP-backed artifact. @details Runs miniz over borrowed reads and a caller-owned arena.
 * @param[in,out] io Borrowed input. @param[in] format CBZ or EPUB. @param[in,out] workspace Scratch arena. @param[in,out] report Candidate report. @return Status. @retval k_ra8_ok For a valid archive.
 * @pre All pointers are valid. @pre workspace is reset and bounded.
 * @post The input remains open. @post Success fills report. @note Every file member is CRC checked. @since v0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_verify_zip(mdl_verify_io_t*        io,
                                                  ra8_mdl_format_t        format,
                                                  mdl_export_workspace_t* workspace,
                                                  mdl_verify_report_t*    report)
{
  mdl_verify_arena_t arena = {.workspace = workspace};
  mz_zip_archive     zip   = {};
  zip.m_pAlloc             = internal_arena_alloc;
  zip.m_pFree              = internal_arena_free;
  zip.m_pRealloc           = internal_arena_realloc;
  zip.m_pAlloc_opaque      = &arena;
  zip.m_pRead              = internal_zip_read;
  zip.m_pIO_opaque         = io;
  ra8_err_t error;
  if ((io->size_bytes == 0U) || (mz_zip_reader_init(&zip, io->size_bytes, 0) == MZ_FALSE)) {
    error = arena.exhausted ? k_ra8_err_invalid_size : k_ra8_err_validation_failed;
  } else {
    const mz_uint  total = mz_zip_reader_get_num_files(&zip);
    mdl_zip_scan_t scan  = {.members = total};
    error                = (total > k_verify_member_max) ? k_ra8_err_invalid_size : k_ra8_ok;
    for (mz_uint i = 0U; (error == k_ra8_ok) && (i < total); ++i) {
      error = internal_zip_member(&zip, i, &scan);
    }
    if ((error == k_ra8_ok) && (mz_zip_reader_end(&zip) == MZ_FALSE)) {
      error = k_ra8_err_validation_failed;
    }
    if (arena.exhausted) {
      error = k_ra8_err_invalid_size;
    } else if (error == k_ra8_ok) {
      error = internal_zip_semantics(format, &scan, report);
    }
  }
  if (io->read_error != k_ra8_ok) {
    error = io->read_error;
  }
  return error;
}

/**
 * @brief Parse a bounded TAR octal field. @details Accepts padding but rejects non-octal data and overflow.
 * @param[in] field Input field. @param[in] length Field extent. @param[out] out Parsed value. @return Status. @retval k_ra8_ok For valid octal.
 * @pre field and out are valid. @pre field spans length bytes.
 * @post Success initializes out. @post Failure exposes no partial value. @note Base-256 extensions are unsupported. @since v0.1.0
 */
RA8_INTERNAL static bool internal_parse_octal(const uint8_t* field, size_t length, uint64_t* out)
{
  size_t index = 0U;
  while ((index < length) && ((field[index] == ' ') || (field[index] == '\0'))) {
    ++index;
  }
  uint64_t value = 0U;
  bool     any   = false;
  for (; (index < length) && (field[index] != '\0') && (field[index] != ' '); ++index) {
    if ((field[index] < '0') || (field[index] > '7') || (value > (UINT64_MAX >> 3U))) {
      return false;
    }
    value = (value << 3U) + (uint64_t)(field[index] - '0');
    any   = true;
  }
  *out = value;
  return any;
}

/**
 * @brief Validate one TAR header. @details Checks checksum, type, path, count, and padded payload size.
 * @param[in,out] state Incremental TAR state. @return Status. @retval k_ra8_ok For a supported member.
 * @pre state holds one complete header. @pre state counters are bounded.
 * @post Success advances member accounting. @post Failure stops validation. @note Only regular files are accepted. @since v0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_tar_member(mdl_tar_stream_t* state)
{
  unsigned checksum = 0U;
  for (size_t i = 0U; i < k_tar_block_bytes; ++i) {
    checksum +=
      ((i >= k_tar_checksum_offset) && (i < k_tar_checksum_end)) ? (unsigned)' ' : state->block[i];
  }
  uint64_t expected = 0U;
  uint64_t size     = 0U;
  if (!internal_parse_octal(state->block + k_tar_checksum_offset,
                            k_tar_checksum_end - k_tar_checksum_offset,
                            &expected) ||
      ((uint64_t)checksum != expected) ||
      !internal_parse_octal(state->block + k_tar_size_offset, k_tar_size_bytes, &size) ||
      (state->block[k_tar_type_offset] != '0') || (size > UINT64_MAX - k_tar_padding_mask)) {
    return k_ra8_err_validation_failed;
  }
  char name[k_tar_name_bytes + 1U];
  memcpy(name, state->block, k_tar_name_bytes);
  name[k_tar_name_bytes] = '\0';
  if (!internal_safe_member_name(name)) {
    return k_ra8_err_validation_failed;
  }
  if (state->members >= k_verify_member_max) {
    return k_ra8_err_invalid_size;
  }
  ++state->members;
  state->pages += internal_is_image(name) ? 1U : 0U;
  state->metadata   = state->metadata || (strcmp(name, "ComicInfo.xml") == 0);
  state->skip_bytes = ((size + k_tar_padding_mask) / k_tar_block_bytes) * k_tar_block_bytes;
  return k_ra8_ok;
}

/**
 * @brief Process one complete 512-byte TAR block once fully buffered.
 * @details Classifies the block as all-zero (an end-of-archive marker) or a
 * real header, updating end-of-archive and skip-byte state.
 * @param[in,out] state Incremental TAR validation state with a full block.
 * @return Status. @retval k_ra8_ok The block was a zero marker or a valid header.
 * @pre state->block_used == k_tar_block_bytes on entry.
 * @post state->block_used is reset to zero. @post A zero block advances the
 * end-of-archive counter; a header block resets it and derives the next
 * skip-byte count. @note Not thread-safe; shares the caller's incremental
 * state. @since v0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_tar_process_block(mdl_tar_stream_t* state)
{
  bool zero = true;
  for (size_t i = 0U; i < k_tar_block_bytes; ++i) {
    zero = zero && (state->block[i] == 0U);
  }
  state->block_used = 0U;
  if (zero) {
    state->zero_blocks += 1U;
    state->ended = state->zero_blocks >= 2U;
    return k_ra8_ok;
  }
  state->zero_blocks = 0U;
  return internal_tar_member(state);
}

/**
 * @brief Feed bytes into TAR validation. @details Assembles headers and skips bounded padded payloads incrementally.
 * @param[in,out] state Incremental state. @param[in] bytes Input bytes. @param[in] length Input extent. @return Status. @retval k_ra8_ok When the prefix remains valid.
 * @pre state and bytes are valid. @pre bytes spans length bytes.
 * @post Every input byte is consumed or rejected. @post Counters remain bounded. @note Data after termination must be zero. @since v0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_tar_feed(mdl_tar_stream_t* state, const uint8_t* bytes, size_t length)
{
  if (state->total_bytes > (UINT64_MAX - length)) {
    return k_ra8_err_invalid_size;
  }
  state->total_bytes += length;
  size_t offset = 0U;
  while (offset < length) {
    if (state->ended) {
      if (bytes[offset++] != 0U) {
        return k_ra8_err_validation_failed;
      }
      continue;
    }
    if (state->skip_bytes != 0U) {
      const uint64_t available = (uint64_t)(length - offset);
      const size_t   consumed =
        (state->skip_bytes < available) ? (size_t)state->skip_bytes : (length - offset);
      state->skip_bytes -= consumed;
      offset += consumed;
      continue;
    }
    const size_t missing = k_tar_block_bytes - state->block_used;
    const size_t copied  = ((length - offset) < missing) ? (length - offset) : missing;
    memcpy(state->block + state->block_used, bytes + offset, copied);
    state->block_used += copied;
    offset += copied;
    if (state->block_used == k_tar_block_bytes) {
      const ra8_err_t error = internal_tar_process_block(state);
      if (error != k_ra8_ok) {
        return error;
      }
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Finish TAR validation. @details Requires aligned complete termination before publishing counts.
 * @param[in,out] state Completed stream state. @param[in,out] report Candidate report. @return Status. @retval k_ra8_ok For a complete TAR.
 * @pre state and report are valid. @pre All source bytes were fed.
 * @post Success fills report counts. @post Failure leaves caller output private. @note Two zero records are required. @since v0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_tar_finish(const mdl_tar_stream_t* state,
                                                  mdl_verify_report_t*    report)
{
  if (!state->ended || (state->block_used != 0U) || (state->skip_bytes != 0U) ||
      ((state->total_bytes % k_tar_block_bytes) != 0U) || (state->pages == 0U) ||
      !state->metadata) {
    return k_ra8_err_validation_failed;
  }
  report->page_count       = state->pages;
  report->member_count     = state->members;
  report->metadata_present = state->metadata;
  return k_ra8_ok;
}

/**
 * @brief Validate an uncompressed CBT. @details Streams borrowed reads directly into the TAR state machine.
 * @param[in,out] io Borrowed input. @param[in,out] report Candidate report. @return Status. @retval k_ra8_ok For a valid CBT.
 * @pre Both pointers are valid. @pre io storage supplies a nonzero I/O buffer.
 * @post The input remains open. @post Success fills report. @note No archive-sized buffer is used. @since v0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_verify_tar(mdl_verify_io_t* io, mdl_verify_report_t* report)
{
  mdl_storage_t*   storage = io->storage;
  ra8_err_t        error   = k_ra8_ok;
  mdl_tar_stream_t tar     = {};
  for (;;) {
    size_t got = 0U;
    error      = internal_io_read_up_to(io, storage->io_buffer, storage->io_buffer_bytes, &got);
    if (error != k_ra8_ok) {
      break;
    }
    if (got == 0U) {
      error = internal_tar_finish(&tar, report);
      break;
    }
    error = internal_tar_feed(&tar, storage->io_buffer, got);
    if (error != k_ra8_ok) {
      break;
    }
  }
  return error;
}

/**
 * @brief Decode one little-endian word. @details Combines exactly four bytes without alignment assumptions.
 * @param[in] bytes Four input bytes. @return Decoded word. @retval UINT32_MAX When encoded as all ones.
 * @pre bytes spans four bytes. @pre bytes remains readable for the call.
 * @post bytes is unchanged. @post The result is host-endian independent. @note Used by gzip trailers. @since v0.1.0
 */
RA8_INTERNAL static uint32_t internal_get_u32le(const uint8_t* bytes)
{
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << k_u32_byte_one_shift) |
         ((uint32_t)bytes[2] << k_u32_byte_two_shift) |
         ((uint32_t)bytes[3] << k_u32_byte_three_shift);
}

/**
 * @brief Inflate a raw-DEFLATE chunk. @details Feeds bounded decoded chunks into TAR while tracking CRC and size.
 * @param[in,out] gzip Inflate and TAR state. @param[in] bytes Compressed input. @param[in] length Input extent. @return Status. @retval k_ra8_ok While the stream is valid.
 * @pre gzip is initialized. @pre bytes is non-null even when length is zero.
 * @post Input is consumed or rejected. @post Produced bytes update TAR and CRC. @note Output never exceeds its fixed chunk. @since v0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_gzip_feed(mdl_gzip_stream_t* gzip, const uint8_t* bytes, uint32_t length)
{
  gzip->stream.next_in  = bytes;
  gzip->stream.avail_in = length;
  do {
    const mz_uint before_in = gzip->stream.avail_in;
    gzip->stream.next_out   = gzip->output;
    gzip->stream.avail_out  = gzip->output_cap;
    const int      status   = mz_inflate(&gzip->stream, MZ_NO_FLUSH);
    const uint32_t produced = gzip->output_cap - gzip->stream.avail_out;
    if (gzip->raw_bytes > (uint64_t)UINT32_MAX - produced) {
      return k_ra8_err_invalid_size;
    }
    if (produced != 0U) {
      const ra8_err_t error = internal_tar_feed(&gzip->tar, gzip->output, produced);
      if (error != k_ra8_ok) {
        return error;
      }
      gzip->crc = (uint32_t)mz_crc32(gzip->crc, gzip->output, produced);
      gzip->raw_bytes += produced;
    }
    if (status == MZ_STREAM_END) {
      gzip->ended = true;
      return (gzip->stream.avail_in == 0U) ? k_ra8_ok : k_ra8_err_validation_failed;
    }
    if ((status != MZ_OK) || ((before_in == gzip->stream.avail_in) && (produced == 0U))) {
      return k_ra8_err_validation_failed;
    }
  } while ((gzip->stream.avail_in != 0U) || (gzip->stream.avail_out == 0U));
  return k_ra8_ok;
}

/**
 * @brief Finish raw-DEFLATE validation. @details Requires an explicit end marker without additional compressed bytes.
 * @param[in,out] gzip Initialized stream state. @return Status. @retval k_ra8_ok When the end marker is observed.
 * @pre gzip is initialized. @pre All compressed input was supplied.
 * @post Success sets ended. @post Stalls become validation failures. @note A non-null zero-byte sentinel avoids miniz UB. @since v0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_gzip_finish_deflate(mdl_gzip_stream_t* gzip)
{
  const uint8_t empty_input = 0U;
  while (!gzip->ended) {
    /* miniz performs pointer arithmetic even for zero-byte input. */
    const ra8_err_t error = internal_gzip_feed(gzip, &empty_input, 0U);
    if (error != k_ra8_ok) {
      return error;
    }
    if (!gzip->ended && (gzip->stream.avail_out != 0U)) {
      return k_ra8_err_validation_failed;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Validate the fixed gzip header. @details Accepts DEFLATE with no optional RFC 1952 fields.
 * @param[in] header Fixed header bytes. @return Whether supported. @retval true For the accepted frame.
 * @pre header spans k_gzip_header_bytes. @pre header is readable.
 * @post header is unchanged. @post The result is deterministic. @note Optional fields are rejected explicitly. @since v0.1.0
 */
RA8_INTERNAL static bool internal_gzip_header_valid(const uint8_t* header)
{
  return (header[0] == k_gzip_id_one) && (header[1] == k_gzip_id_two) &&
         (header[2] == k_gzip_method_deflate) && (header[3] == 0U);
}

/**
 * @brief Validate gzip trailer accounting. @details Compares stored CRC32 and ISIZE with streamed output.
 * @param[in] trailer Eight trailer bytes. @param[in,out] gzip Completed stream state. @return Whether both fields match. @retval true On an exact match.
 * @pre Both pointers are valid. @pre raw_bytes fits RFC 1952 ISIZE policy.
 * @post Inputs are unchanged. @post Both fields are checked. @note ISIZE is compared modulo uint32_t. @since v0.1.0
 */
RA8_INTERNAL static bool internal_gzip_trailer_valid(const uint8_t*           trailer,
                                                     const mdl_gzip_stream_t* gzip)
{
  return (internal_get_u32le(trailer) == gzip->crc) &&
         (internal_get_u32le(trailer + 4U) == (uint32_t)gzip->raw_bytes);
}

/**
 * @brief Initialize the gzip inflater bound to workspace-backed scratch.
 * @details Reserves the output buffer from workspace, wires the miniz
 * allocator to the arena, and opens the raw-deflate inflater.
 * @param[in,out] workspace Scratch arena backing both the output buffer and
 * miniz's internal allocations.
 * @param[in,out] arena Miniz allocator state bound to @p workspace.
 * @param[in,out] gzip Stream state to initialize.
 * @return Status. @retval k_ra8_ok The output buffer and inflater are ready.
 * @pre gzip->output_cap and gzip->crc are already set by the caller.
 * @post On success gzip->output and gzip->stream are ready to feed.
 * @note Not thread-safe for a shared workspace. @since v0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_gzip_init_inflate(mdl_export_workspace_t* workspace,
                                                         mdl_verify_arena_t*     arena,
                                                         mdl_gzip_stream_t*      gzip)
{
  gzip->output =
    (uint8_t*)internal_workspace_take(workspace, gzip->output_cap, _Alignof(max_align_t));
  arena->exhausted    = gzip->output == nullptr;
  gzip->stream.zalloc = internal_arena_alloc;
  gzip->stream.zfree  = internal_arena_free;
  gzip->stream.opaque = arena;
  if ((gzip->output == nullptr) ||
      (mz_inflateInit2(&gzip->stream, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK)) {
    return arena->exhausted ? k_ra8_err_invalid_size : k_ra8_err_validation_failed;
  }
  return k_ra8_ok;
}

/**
 * @brief Feed the compressed remainder into the inflater and validate the trailer.
 * @details Streams chunked reads until end-of-stream or @p remaining is
 * exhausted, finishes any pending deflate output, then reads and checks the
 * eight-byte gzip trailer against the streamed CRC/size.
 * @param[in,out] io Borrowed input positioned after the gzip header.
 * @param[in,out] arena Miniz allocator state; inspected for exhaustion.
 * @param[in,out] gzip Stream state advanced by this call.
 * @param[in] remaining Compressed bytes left to read, excluding the trailer.
 * @return Status. @retval k_ra8_ok The stream ended cleanly and the trailer matches.
 * @pre @p io is positioned at the first compressed byte.
 * @pre @p gzip was successfully initialized by ::internal_gzip_init_inflate.
 * @post On k_ra8_ok every compressed byte and the trailer have been consumed.
 * @note Not thread-safe for a shared input or workspace. @since v0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_gzip_consume(mdl_verify_io_t*    io,
                                                    mdl_verify_arena_t* arena,
                                                    mdl_gzip_stream_t*  gzip,
                                                    uint64_t            remaining)
{
  mdl_storage_t* storage = io->storage;
  ra8_err_t      error   = k_ra8_ok;
  while ((error == k_ra8_ok) && (remaining != 0U) && !gzip->ended) {
    const uint32_t chunk =
      (remaining < storage->io_buffer_bytes) ? (uint32_t)remaining : storage->io_buffer_bytes;
    error = internal_io_read_exact(io, storage->io_buffer, chunk);
    if (error == k_ra8_ok) {
      remaining -= chunk;
      error = internal_gzip_feed(gzip, storage->io_buffer, chunk);
    }
  }
  if ((error == k_ra8_ok) && gzip->ended && (remaining != 0U)) {
    error = k_ra8_err_validation_failed;
  }
  if ((error == k_ra8_ok) && !gzip->ended) {
    error = internal_gzip_finish_deflate(gzip);
  }
  uint8_t trailer[k_gzip_trailer_bytes];
  if (error == k_ra8_ok) {
    error = internal_io_read_exact(io, trailer, sizeof(trailer));
  }
  if ((error == k_ra8_ok) && !internal_gzip_trailer_valid(trailer, gzip)) {
    error = k_ra8_err_validation_failed;
  }
  if ((error == k_ra8_ok) && arena->exhausted) {
    error = k_ra8_err_invalid_size;
  }
  return error;
}

/**
 * @brief Validate a gzip-compressed CBT. @details Streams raw inflate into TAR and checks framing, CRC, size, and trailing data.
 * @param[in,out] io Borrowed input. @param[in,out] workspace Scratch arena. @param[in,out] report Candidate report. @return Status. @retval k_ra8_ok For valid CBT.GZ.
 * @pre All pointers are valid. @pre workspace and storage buffers satisfy capacity.
 * @post The input remains open and inflater is closed. @post Success fills report. @note No whole compressed or raw archive is retained. @since v0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_verify_gzip_tar(mdl_verify_io_t*        io,
                                                       mdl_export_workspace_t* workspace,
                                                       mdl_verify_report_t*    report)
{
  ra8_err_t error;
  uint8_t   header[k_gzip_header_bytes];
  error = (io->size_bytes < k_gzip_min_bytes) ? k_ra8_err_validation_failed
                                              : internal_io_read_exact(io, header, sizeof(header));
  if ((error == k_ra8_ok) && !internal_gzip_header_valid(header)) {
    error = k_ra8_err_validation_failed;
  }
  mdl_verify_arena_t arena = {.workspace = workspace};
  mdl_gzip_stream_t  gzip  = {.output_cap = k_mdl_storage_io_bytes, .crc = MZ_CRC32_INIT};
  if (error == k_ra8_ok) {
    error = internal_gzip_init_inflate(workspace, &arena, &gzip);
  }
  if (error == k_ra8_ok) {
    const uint64_t remaining =
      (io->size_bytes >= k_gzip_min_bytes) ? io->size_bytes - k_gzip_min_bytes : 0U;
    error = internal_gzip_consume(io, &arena, &gzip, remaining);
  }
  if (gzip.stream.state != nullptr) {
    (void)mz_inflateEnd(&gzip.stream);
  }
  if (error == k_ra8_ok) {
    error = internal_tar_finish(&gzip.tar, report);
  }
  return error;
}

/**
 * @brief Adapt JOF positioned reads. @details Uses portable seek and bounded reads against the size snapshot.
 * @param[in,out] opaque Verifier input. @param[in] offset File offset. @param[out] destination Output buffer. @param[in] length Requested bytes. @param[out] got Produced bytes. @return Status. @retval k_ra8_ok On data or EOF.
 * @pre All pointers are valid. @pre destination spans length bytes.
 * @post got never exceeds length. @post Reads stay within the snapshot. @note Parser-visible EOF is a zero-byte success. @since v0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_jof_pread(void* opaque, uint64_t offset, uint8_t* destination, size_t length, size_t* got)
{
  mdl_verify_io_t* io = (mdl_verify_io_t*)opaque;
  *got                = 0U;
  if (offset > io->size_bytes) {
    return k_ra8_ok;
  }
  const uint64_t available = io->size_bytes - offset;
  if ((uint64_t)length > available) {
    length = (size_t)available;
  }
  const ra8_err_t error = fw_fs_seek(io->file, offset);
  return (error == k_ra8_ok) ? internal_io_read_up_to(io, destination, length, got) : error;
}

/**
 * @brief Validate a JOF artifact. @details Invokes the production parser through the injected positioned-read adapter.
 * @param[in,out] io Borrowed input. @param[in,out] report Candidate report. @return Status. @retval k_ra8_ok For valid JOF.
 * @pre Both pointers are valid. @pre io stream capabilities include seek.
 * @post The input remains open. @post Success publishes JOF counts. @note Parser faults are preserved. @since v0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_verify_jof(mdl_verify_io_t* io, mdl_verify_report_t* report)
{
  ra8_err_t error;
  if (io->size_bytes == 0U) {
    error = k_ra8_err_validation_failed;
  } else {
    ra8_jof_info_t info;
    error = ra8_jof_parse(internal_jof_pread, io, io->size_bytes, &info);
    if (error == k_ra8_ok) {
      report->page_count   = 1U;
      report->member_count = info.tile_count;
    }
  }
  return error;
}

/**
 * @brief Dispatch validation over one borrowed input. @details Keeps format selection separate from ownership.
 * @param[in,out] io Borrowed verifier input. @param[in] format Expected format. @param[in,out] workspace Scratch arena. @param[in,out] report Candidate report. @return Status. @retval k_ra8_ok For a valid artifact.
 * @pre All pointers are valid. @pre io starts at offset zero.
 * @post io remains open. @post Success fills report. @note Unsupported formats stay explicit. @since v0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_verify_borrowed(mdl_verify_io_t*        io,
                                                       ra8_mdl_format_t        format,
                                                       mdl_export_workspace_t* workspace,
                                                       mdl_verify_report_t*    report)
{
  switch (format) {
    case k_ra8_mdl_format_cbz:
    case k_ra8_mdl_format_epub:
      return internal_verify_zip(io, format, workspace, report);
    case k_ra8_mdl_format_cbt:
      return internal_verify_tar(io, report);
    case k_ra8_mdl_format_jof:
      return internal_verify_jof(io, report);
    case k_ra8_mdl_format_cbt_gz:
      return internal_verify_gzip_tar(io, workspace, report);
    case k_ra8_mdl_format_rabook:
      return priv_mdl_verify_rabook(io->file, io->size_bytes, workspace, report);
    case k_ra8_mdl_format_cbr:
    case k_ra8_mdl_format_cbt_xz:
      return k_ra8_err_not_supported;
    case k_ra8_mdl_format_loose:
    case k_ra8_mdl_format_invalid:
    default:
      return k_ra8_err_invalid_arg;
  }
}

ra8_err_t mdl_verify_open_file(mdl_storage_t*          storage,
                               ra8_mdl_format_t        format,
                               fw_fs_file_t*           file,
                               uint64_t                size_bytes,
                               mdl_export_workspace_t* workspace,
                               mdl_verify_report_t*    report)
{
  if ((storage == nullptr) || (storage->io_buffer == nullptr) || (storage->io_buffer_bytes == 0U) ||
      (file == nullptr) || (workspace == nullptr) || (workspace->data == nullptr) ||
      (report == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  workspace->used               = 0U;
  workspace->high_water         = 0U;
  mdl_verify_report_t candidate = {.format = format};
  mdl_verify_io_t     io        = {.storage    = storage,
                                   .file       = file,
                                   .size_bytes = size_bytes,
                                   .read_error = k_ra8_ok};
  ra8_err_t           error     = fw_fs_seek(file, 0U);
  if (error == k_ra8_ok) {
    error = internal_verify_borrowed(&io, format, workspace, &candidate);
  }
  if (error == k_ra8_ok) {
    *report = candidate;
  }
  return error;
}

ra8_err_t mdl_verify_file(mdl_storage_t*          storage,
                          ra8_mdl_format_t        format,
                          const char*             path,
                          mdl_export_workspace_t* workspace,
                          mdl_verify_report_t*    report)
{
  if ((storage == nullptr) || (storage->fs == nullptr) || (storage->file_workspace == nullptr) ||
      (storage->file_workspace_bytes == 0U) || (storage->io_buffer == nullptr) ||
      (storage->io_buffer_bytes == 0U) || (path == nullptr) || (workspace == nullptr) ||
      (workspace->data == nullptr) || (report == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  workspace->used       = 0U;
  workspace->high_water = 0U;
  mdl_verify_io_t     io;
  ra8_err_t           error = internal_io_open(storage, path, &io);
  mdl_verify_report_t candidate;
  if (error == k_ra8_ok) {
    error = mdl_verify_open_file(storage, format, io.file, io.size_bytes, workspace, &candidate);
    error = internal_io_close(&io, error);
  }
  if (error == k_ra8_ok) {
    *report = candidate;
  }
  return error;
}
