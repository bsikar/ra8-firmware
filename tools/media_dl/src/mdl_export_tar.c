/**
 * @file mdl_export_tar.c
 * @brief Stream deterministic CBT and gzip-wrapped CBT containers.
 *
 * @details Emits ustar records directly into a caller-owned portable sink.
 * Gzip wraps that same stream online, so no intermediate archive or host
 * stream exists and all publication remains one validated transaction.
 *
 * [Ring 4 / Domain] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#include <stdio.h>
#include <string.h>

#include "mdl_export.h"
#include "mdl_export_internal.h"
#include "miniz.h"
#include "ra8_attributes.h"

/** @brief ustar header field offsets and widths. */
typedef enum : uint16_t {
  k_tar_block   = 512U, /**< Tar record size.          */
  k_off_name    = 0U,   /**< Name field offset.        */
  k_len_name    = 100U, /**< Name field width.         */
  k_off_mode    = 100U, /**< Mode field offset.        */
  k_off_uid     = 108U, /**< UID field offset.         */
  k_off_gid     = 116U, /**< GID field offset.         */
  k_len_id      = 8U,   /**< Mode/UID/GID field width. */
  k_off_size    = 124U, /**< Size field offset.        */
  k_len_size    = 12U,  /**< Size field width.         */
  k_off_mtime   = 136U, /**< Timestamp field offset.   */
  k_len_mtime   = 12U,  /**< Timestamp field width.    */
  k_off_chksum  = 148U, /**< Checksum field offset.    */
  k_len_chksum  = 8U,   /**< Checksum field width.     */
  k_off_type    = 156U, /**< Entry-type field offset.  */
  k_off_magic   = 257U, /**< Ustar magic offset.       */
  k_len_magic   = 6U,   /**< Ustar magic width.        */
  k_off_version = 263U, /**< Ustar version offset.     */
} mdl_tar_layout_t;

/** @brief Fixed regular-file mode in deterministic tar headers. */
typedef enum : uint16_t {
  k_file_mode = 0644U, /**< Portable regular-file permission bits. */
} mdl_tar_mode_t;

/** @brief Gzip framing and serialization constants. */
typedef enum : uint16_t {
  k_gzip_header_bytes = 10U,  /**< Fixed RFC 1952 header. */
  k_gzip_u32_bytes    = 4U,   /**< Trailer scalar width.  */
  k_byte_bits         = 8U,   /**< Bits shifted per byte. */
  k_byte_mask         = 255U, /**< Low-byte extraction.   */
} mdl_gzip_layout_t;

/** @brief Direct tar sink adapter. */
typedef struct {
  mdl_export_output_t* output; /**< Active staged transaction. */
} internal_tar_sink_t;

/** @brief Online gzip state receiving the uncompressed tar stream. */
typedef struct {
  mdl_export_output_t* output;     /**< Active gzip output stage.    */
  tdefl_compressor*    compressor; /**< Caller-arena DEFLATE state.  */
  uint32_t             crc;        /**< Running tar CRC32.           */
  uint32_t             size;       /**< Tar size modulo 2^32.        */
  ra8_err_t            error;      /**< First output/compress error. */
} internal_gzip_sink_t;

/** @brief Fixed ustar magic and version payloads. */
static const uint8_t s_ustar_magic[k_len_magic] = {'u', 's', 't', 'a', 'r', '\0'};
static const uint8_t s_ustar_version[2]         = {'0', '0'};

/**
 * @brief Round one payload extent to a whole tar record
 * @details Adds at most one record-minus-one before integer division so no
 *          loop or hidden state is required.
 * @param[in] bytes Payload extent.
 * @return Smallest record-aligned extent not below @p bytes.
 * @retval size_t Record-aligned padded extent.
 * @pre @p bytes leaves room for one record-minus-one addition.
 * @pre ::k_tar_block is a nonzero compile-time record size.
 * @post The result is divisible by ::k_tar_block.
 * @post The result is not smaller than @p bytes.
 * @note Thread-safe and side-effect free.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t internal_round_block(size_t bytes)
{
  return ((bytes + (size_t)k_tar_block - 1U) / (size_t)k_tar_block) * (size_t)k_tar_block;
}

/**
 * @brief Build one deterministic ustar regular-file header
 * @details Zeroes the complete record, encodes fixed owner/time fields, then
 *          computes the checksum with the checksum field represented by spaces.
 * @param[out] block Writable tar record.
 * @param[in] name Bounded archive member name.
 * @param[in] size Exact payload extent.
 * @return Header construction status.
 * @retval k_ra8_ok The complete header was written.
 * @retval k_ra8_err_invalid_size A field cannot represent its input.
 * @pre Pointers are valid and @p block covers ::k_tar_block bytes.
 * @pre @p name is NUL-terminated and stable for the call.
 * @post Success initializes every record byte deterministically.
 * @post Failure never reports a truncated name or unrepresentable size as valid.
 * @note Thread-safe across distinct output records.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_tar_header(uint8_t* block, const char* name, size_t size)
{
  if ((strlen(name) >= (size_t)k_len_name) || (size > 077777777777ULL)) {
    return k_ra8_err_invalid_size;
  }
  memset(block, 0, k_tar_block);
  (void)snprintf((char*)block + k_off_name, k_len_name, "%s", name);
  (void)snprintf((char*)block + k_off_mode, k_len_id, "%07o", (unsigned)k_file_mode);
  (void)snprintf((char*)block + k_off_uid, k_len_id, "%07o", 0U);
  (void)snprintf((char*)block + k_off_gid, k_len_id, "%07o", 0U);
  (void)snprintf((char*)block + k_off_size, k_len_size, "%011zo", size);
  (void)snprintf((char*)block + k_off_mtime, k_len_mtime, "%011o", 0U);
  block[k_off_type] = '0';
  memcpy(block + k_off_magic, s_ustar_magic, sizeof(s_ustar_magic));
  memcpy(block + k_off_version, s_ustar_version, sizeof(s_ustar_version));
  memset(block + k_off_chksum, ' ', k_len_chksum);
  unsigned sum = 0U;
  for (size_t i = 0U; i < (size_t)k_tar_block; ++i) {
    sum += block[i];
  }
  (void)snprintf((char*)block + k_off_chksum, k_len_chksum - 1U, "%06o", sum);
  block[k_off_chksum + k_len_chksum - 1U] = ' ';
  return k_ra8_ok;
}

/**
 * @brief Append bytes directly to an archive publication stage
 * @details Adapts the generic tar sink contract to the exporter transaction's
 *          complete-write operation without taking ownership.
 * @param[in,out] ctx Bound ::internal_tar_sink_t.
 * @param[in] bytes Source bytes.
 * @param[in] length Source extent.
 * @return Transaction write status.
 * @retval k_ra8_ok Every byte was appended.
 * @retval k_ra8_fail The injected transaction sink failed.
 * @pre Context and byte span are valid.
 * @pre The bound output owns an active transaction.
 * @post Success appends exactly @p length bytes.
 * @post Failure remains retained by the output transaction.
 * @note Not thread-safe for a shared stage.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_direct_sink(void* ctx, const uint8_t* bytes, uint32_t length)
{
  return priv_mdl_export_output_write(((internal_tar_sink_t*)ctx)->output, bytes, length);
}

/**
 * @brief Emit zero padding through one archive sink
 * @details Uses one bounded zero record and never emits a callback for a
 *          zero-length request.
 * @param[in] sink Destination callback.
 * @param[in,out] ctx Destination context.
 * @param[in] length Zero-byte extent, at most one tar record.
 * @return Sink status.
 * @retval k_ra8_ok Padding was empty or completely accepted.
 * @retval k_ra8_fail The injected sink rejected the padding.
 * @pre @p sink is non-null and @p length does not exceed ::k_tar_block.
 * @pre @p ctx remains valid for the callback duration.
 * @post Success appends exactly @p length zero bytes.
 * @post No caller-owned input buffer is mutated.
 * @note Thread-safe across distinct sinks.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_write_zeros(mdl_export_sink_fn_t sink, void* ctx, uint32_t length)
{
  const uint8_t zeros[k_tar_block] = {};
  return (length == 0U) ? k_ra8_ok : sink(ctx, zeros, length);
}

/**
 * @brief Emit one bounded in-memory tar member
 * @details Writes a deterministic header, the exact payload, and record
 *          padding sequentially through the caller sink.
 * @param[in] sink Destination callback.
 * @param[in,out] ctx Destination context.
 * @param[in] name Archive member name.
 * @param[in] bytes Payload bytes.
 * @param[in] length Payload extent.
 * @return Header, payload, or padding status.
 * @retval k_ra8_ok The complete member was emitted.
 * @retval k_ra8_err_invalid_size A header field or payload bound was exceeded.
 * @pre Inputs satisfy their declared bounds.
 * @pre @p sink and @p ctx remain valid for all callbacks.
 * @post Success leaves the sink at a tar-record boundary.
 * @post Failure stops at the first rejected component.
 * @note Not thread-safe for a shared sink.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_tar_write_memory(mdl_export_sink_fn_t sink,
                                                        void*                ctx,
                                                        const char*          name,
                                                        const uint8_t*       bytes,
                                                        size_t               length)
{
  uint8_t   header[k_tar_block];
  ra8_err_t err = internal_tar_header(header, name, length);
  if (err == k_ra8_ok) {
    err = sink(ctx, header, sizeof(header));
  }
  if ((err == k_ra8_ok) && (length > UINT32_MAX)) {
    err = k_ra8_err_invalid_size;
  }
  if ((err == k_ra8_ok) && (length != 0U)) {
    err = sink(ctx, bytes, (uint32_t)length);
  }
  if (err == k_ra8_ok) {
    err = internal_write_zeros(sink, ctx, (uint32_t)(internal_round_block(length) - length));
  }
  return err;
}

/**
 * @brief Emit one portable source as a verified tar member
 * @details Snapshots the regular source, writes its deterministic header,
 *          streams the first pass, verifies an independent reread, and pads.
 * @param[in,out] storage Bound portable filesystem.
 * @param[in] path Canonical source path.
 * @param[in] member Archive member name.
 * @param[in] sink Destination callback.
 * @param[in,out] ctx Destination context.
 * @return Source, header, sink, or preservation status.
 * @retval k_ra8_ok The complete stable source member was emitted.
 * @retval k_ra8_err_validation_failed The source changed between passes.
 * @pre Inputs remain stable and storage is exclusively owned.
 * @pre @p sink and @p ctx remain callable for the complete member.
 * @post Success independently verifies the complete source first pass.
 * @post Every opened source stream is closed on success and failure.
 * @note Concurrent source mutation fails closed.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_tar_write_source(mdl_storage_t*       storage,
                                                        const char*          path,
                                                        const char*          member,
                                                        mdl_export_sink_fn_t sink,
                                                        void*                ctx)
{
  mdl_export_source_t source      = {};
  ra8_err_t           err         = priv_mdl_export_source_open(&source, storage, path);
  uint64_t            source_size = source.size;
  if ((err == k_ra8_ok) && (source.size > SIZE_MAX)) {
    err = k_ra8_err_invalid_size;
  }
  uint8_t header[k_tar_block];
  if (err == k_ra8_ok) {
    err = internal_tar_header(header, member, (size_t)source.size);
  }
  if (err == k_ra8_ok) {
    err = sink(ctx, header, sizeof(header));
  }
  if (err == k_ra8_ok) {
    err = priv_mdl_export_source_copy(&source, sink, ctx);
  } else if (source.file.is_open) {
    const ra8_err_t closed = priv_mdl_export_source_close(&source);
    if (closed != k_ra8_ok) {
      err = closed;
    }
  }
  if (err == k_ra8_ok) {
    err = internal_write_zeros(
      sink,
      ctx,
      (uint32_t)(internal_round_block((size_t)source_size) - (size_t)source_size));
  }
  return err;
}

/**
 * @brief Stream a complete deterministic ustar archive
 * @details Emits page members in sorted order, one generated ComicInfo member,
 *          and the two-record ustar trailer without retaining the archive.
 * @param[in,out] storage Bound portable filesystem.
 * @param[in] directory Chapter directory.
 * @param[in] names Sorted page rows.
 * @param[in] count Page count.
 * @param[in] meta Metadata to encode.
 * @param[in] sink Destination callback.
 * @param[in,out] ctx Destination context.
 * @return Complete tar-stream status.
 * @retval k_ra8_ok The complete archive stream was accepted.
 * @retval k_ra8_fail A source, metadata, or sink operation failed.
 * @pre All pointers are valid and sources remain stable.
 * @pre @p names contains @p count terminated rows in desired member order.
 * @post Success includes ComicInfo and two zero trailer records.
 * @post Failure stops at the first rejected source or output span.
 * @note Not thread-safe for shared storage or sink state.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_build_tar(mdl_storage_t*           storage,
                                                 const char*              directory,
                                                 char                     names[][k_name_max],
                                                 size_t                   count,
                                                 const mdl_export_meta_t* meta,
                                                 mdl_export_sink_fn_t     sink,
                                                 void*                    ctx)
{
  ra8_err_t err = k_ra8_ok;
  for (size_t i = 0U; (err == k_ra8_ok) && (i < count); ++i) {
    char path[k_fw_fs_path_cap];
    err = priv_mdl_export_path_join(path, sizeof(path), directory, names[i]);
    if (err == k_ra8_ok) {
      err = internal_tar_write_source(storage, path, names[i], sink, ctx);
    }
  }
  char comic_xml[4096];
  if (err == k_ra8_ok) {
    err = mdl_export_build_comicinfo_pages(meta, count, comic_xml, sizeof(comic_xml));
  }
  if (err == k_ra8_ok) {
    err = internal_tar_write_memory(sink,
                                    ctx,
                                    "ComicInfo.xml",
                                    (const uint8_t*)comic_xml,
                                    strlen(comic_xml));
  }
  if (err == k_ra8_ok) {
    const uint8_t trailer[2U * (size_t)k_tar_block] = {};
    err                                             = sink(ctx, trailer, sizeof(trailer));
  }
  return err;
}

/**
 * @brief Append DEFLATE callback output to the active stage
 * @details Converts miniz's signed callback count to the bounded transaction
 *          writer contract and retains the first output failure.
 * @param[in] bytes Compressed bytes.
 * @param[in] length Signed miniz byte count.
 * @param[in,out] ctx Bound ::internal_gzip_sink_t.
 * @return Miniz callback status.
 * @retval MZ_TRUE The complete compressed span was accepted.
 * @retval MZ_FALSE Arguments or the output transaction failed.
 * @pre Context and nonnegative byte span are valid.
 * @pre The bound output owns an active transaction.
 * @post Failure is retained for exact caller propagation.
 * @post Success appends exactly @p length compressed bytes.
 * @note Not thread-safe for shared gzip state.
 * @since 0.1.0
 */
RA8_INTERNAL static mz_bool internal_gzip_put(const void* bytes, int length, void* ctx)
{
  internal_gzip_sink_t* gzip = (internal_gzip_sink_t*)ctx;
  if ((gzip == nullptr) || (length < 0) || (gzip->error != k_ra8_ok)) {
    return MZ_FALSE;
  }
  gzip->error = priv_mdl_export_output_write(gzip->output, bytes, (uint32_t)length);
  return (gzip->error == k_ra8_ok) ? MZ_TRUE : MZ_FALSE;
}

/**
 * @brief Feed one uncompressed tar span into streaming DEFLATE
 * @details Updates the gzip CRC and modulo-size before passing the complete
 *          input span to the caller-arena compressor.
 * @param[in,out] ctx Bound ::internal_gzip_sink_t.
 * @param[in] bytes Tar bytes.
 * @param[in] length Tar byte count.
 * @return Compression/output status.
 * @retval k_ra8_ok The complete tar span was consumed.
 * @retval k_ra8_fail Compression or compressed-output delivery failed.
 * @pre Context and byte span are valid.
 * @pre The caller-arena compressor remains initialized.
 * @post Success folds the complete span into CRC, size, and DEFLATE state.
 * @post Failure is retained in the shared gzip sink state.
 * @note Not thread-safe for shared compressor state.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_gzip_sink(void* ctx, const uint8_t* bytes, uint32_t length)
{
  internal_gzip_sink_t* gzip = (internal_gzip_sink_t*)ctx;
  if (gzip->error != k_ra8_ok) {
    return gzip->error;
  }
  gzip->crc = (uint32_t)mz_crc32(gzip->crc, bytes, length);
  gzip->size += length;
  if (tdefl_compress_buffer(gzip->compressor, bytes, length, TDEFL_NO_FLUSH) != TDEFL_STATUS_OKAY) {
    gzip->error = (gzip->error == k_ra8_ok) ? k_ra8_fail : gzip->error;
  }
  return gzip->error;
}

/**
 * @brief Serialize one little-endian gzip trailer scalar
 * @details Emits the low byte first and shifts exactly once per output byte.
 * @param[out] bytes Four writable bytes.
 * @param[in] value Scalar to encode.
 * @pre @p bytes covers ::k_gzip_u32_bytes bytes.
 * @pre ::k_gzip_u32_bytes equals the encoded uint32 width.
 * @post The portable little-endian representation is complete.
 * @post Exactly ::k_gzip_u32_bytes caller-owned bytes are modified.
 * @note Thread-safe across distinct output bytes.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_put_u32le(uint8_t* bytes, uint32_t value)
{
  for (size_t i = 0U; i < (size_t)k_gzip_u32_bytes; ++i) {
    bytes[i] = (uint8_t)(value & (uint32_t)k_byte_mask);
    value >>= (uint32_t)k_byte_bits;
  }
}

RA8_PRIV ra8_err_t priv_mdl_export_tar(mdl_storage_t*           storage,
                                       const char*              dir,
                                       char                     names[][k_name_max],
                                       size_t                   count,
                                       mdl_export_output_t*     output,
                                       const mdl_export_meta_t* meta)
{
  internal_tar_sink_t sink = {.output = output};
  return internal_build_tar(storage, dir, names, count, meta, internal_direct_sink, &sink);
}

RA8_PRIV ra8_err_t priv_mdl_export_tar_gzip(mdl_storage_t*           storage,
                                            const char*              dir,
                                            char                     names[][k_name_max],
                                            size_t                   count,
                                            mdl_export_output_t*     output,
                                            const mdl_export_meta_t* meta,
                                            mdl_export_workspace_t*  ws)
{
  tdefl_compressor* compressor =
    (tdefl_compressor*)mdl_export_workspace_take(ws, sizeof(*compressor), 16U);
  if (compressor == nullptr) {
    return k_ra8_err_invalid_size;
  }
  static const uint8_t header[k_gzip_header_bytes] =
    {0x1FU, 0x8BU, 0x08U, 0U, 0U, 0U, 0U, 0U, 0U, 0xFFU};
  ra8_err_t            err  = priv_mdl_export_output_write(output, header, sizeof(header));
  internal_gzip_sink_t gzip = {.output     = output,
                               .compressor = compressor,
                               .crc        = (uint32_t)MZ_CRC32_INIT,
                               .error      = err};
  if ((err == k_ra8_ok) &&
      (tdefl_init(compressor, internal_gzip_put, &gzip, TDEFL_DEFAULT_MAX_PROBES) !=
       TDEFL_STATUS_OKAY)) {
    err = k_ra8_fail;
  }
  if (err == k_ra8_ok) {
    err = internal_build_tar(storage, dir, names, count, meta, internal_gzip_sink, &gzip);
  }
  if ((err == k_ra8_ok) &&
      (tdefl_compress_buffer(compressor, nullptr, 0U, TDEFL_FINISH) != TDEFL_STATUS_DONE)) {
    err = (gzip.error == k_ra8_ok) ? k_ra8_fail : gzip.error;
  }
  uint8_t trailer[2U * k_gzip_u32_bytes];
  internal_put_u32le(trailer, gzip.crc);
  internal_put_u32le(&trailer[k_gzip_u32_bytes], gzip.size);
  if (err == k_ra8_ok) {
    err = priv_mdl_export_output_write(output, trailer, sizeof(trailer));
  }
  return err;
}
