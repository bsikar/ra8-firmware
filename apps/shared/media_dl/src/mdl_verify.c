/**
 * @file mdl_verify.c
 * @brief Portable bounded structural validators for media artifacts.
 * @details Reads only through an injected ::mdl_storage_t. ZIP uses miniz's
 *          positioned callback and JOF uses its production pread seam; the
 *          incremental TAR and gzip validators live beside this file in
 *          mdl_verify_tarball.c.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_verify.h"

#include <ctype.h>
#include <limits.h>
#include <string.h>

#include "mdl_verify_internal.h"
#include "mdl_verify_rabook_internal.h"
#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_jof.h"

/** @brief Header stored before each monotonic miniz allocation. */
typedef struct {
  size_t      bytes; /**< Payload extent.                   */
  max_align_t align; /**< Forces maximum payload alignment. */
} mdl_alloc_header_t;

RA8_PRIV void*
priv_mdl_verify_workspace_take(mdl_export_workspace_t* workspace, size_t bytes, size_t alignment)
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

RA8_PRIV bool priv_mdl_verify_is_image(const char* name)
{
  return internal_ends_ci(name, ".jpg") || internal_ends_ci(name, ".jpeg") ||
         internal_ends_ci(name, ".png") || internal_ends_ci(name, ".webp") ||
         internal_ends_ci(name, ".gif") || internal_ends_ci(name, ".bmp");
}

RA8_PRIV bool priv_mdl_verify_safe_member_name(const char* name)
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

RA8_PRIV void* priv_mdl_verify_arena_alloc(void* opaque, size_t items, size_t size)
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
  mdl_alloc_header_t* header =
    (mdl_alloc_header_t*)priv_mdl_verify_workspace_take(arena->workspace,
                                                        sizeof(*header) + bytes,
                                                        _Alignof(max_align_t));
  if (header == nullptr) {
    arena->exhausted = true;
    return nullptr;
  }
  header->bytes = bytes;
  return (void*)(header + 1);
}

RA8_PRIV void priv_mdl_verify_arena_free(void* opaque, void* address)
{
  (void)opaque;
  (void)address;
}

/** @brief Grow a miniz span by copying it to the next arena allocation. */
RA8_INTERNAL static void*
internal_arena_realloc(void* opaque, void* address, size_t items, size_t size)
{
  if (address == nullptr) {
    return priv_mdl_verify_arena_alloc(opaque, items, size);
  }
  if ((items != 0U) && (size > (SIZE_MAX / items))) {
    ((mdl_verify_arena_t*)opaque)->exhausted = true;
    return nullptr;
  }
  mdl_alloc_header_t* previous = ((mdl_alloc_header_t*)address) - 1;
  void*               next     = priv_mdl_verify_arena_alloc(opaque, items, size);
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

RA8_PRIV ra8_err_t priv_mdl_verify_io_read_up_to(mdl_verify_io_t* io,
                                                 uint8_t*         destination,
                                                 size_t           length,
                                                 size_t*          out_read)
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
    error = priv_mdl_verify_io_read_up_to(io, (uint8_t*)destination, length, &got);
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
  if (!priv_mdl_verify_safe_member_name(member.m_filename)) {
    return k_ra8_err_validation_failed;
  }
  if (!member.m_is_directory &&
      (mz_zip_reader_extract_to_callback(zip, index, internal_discard_zip, nullptr, 0) ==
       MZ_FALSE)) {
    return k_ra8_err_validation_failed;
  }
  const char* name = member.m_filename;
  scan->pages += priv_mdl_verify_is_image(name) ? 1U : 0U;
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
  zip.m_pAlloc             = priv_mdl_verify_arena_alloc;
  zip.m_pFree              = priv_mdl_verify_arena_free;
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
  return (error == k_ra8_ok) ? priv_mdl_verify_io_read_up_to(io, destination, length, got) : error;
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
      return priv_mdl_verify_tar(io, report);
    case k_ra8_mdl_format_jof:
      return internal_verify_jof(io, report);
    case k_ra8_mdl_format_cbt_gz:
      return priv_mdl_verify_gzip_tar(io, workspace, report);
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
