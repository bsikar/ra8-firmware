/**
 * @file jof_worker.c
 * @brief Single-image to JOF transcode behind jof_worker.h.
 * @details Reads one encoded source file whole (bounded), probes its geometry
 *          with `jof_probe_dims()`, sizes the exact producer arenas for that
 *          geometry, and streams `jof_produce()` into the output file through
 *          a raw-descriptor sink. All I/O uses bounded `read`/`write` loops
 *          with partial-transfer and `EINTR` handling; no `FILE *` appears.
 *
 * [Ring 4 / Domain] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "jof_worker.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "jof_produce.h"
#include "ra8_attributes.h"

/** @brief Named resource limits for the worker. */
typedef enum : uint64_t {
  k_worker_max_input = 256U * 1024U * 1024U, /**< Largest accepted input, bytes. */
  k_worker_tile_h    = 256U,                 /**< Band-tile height cap, pixels.  */
  k_worker_out_mode  = 0644U,                /**< File mode: rw-r--r--.          */
} worker_limit_t;

/** @brief WebP container-head offsets for the whole-frame-arena decision. */
typedef enum : uint8_t {
  k_worker_webp_riff_ofs = 0U,  /**< Offset of the "RIFF" fourCC.     */
  k_worker_webp_form_ofs = 8U,  /**< Offset of the "WEBP" fourCC.     */
  k_worker_webp_head_len = 12U, /**< Bytes needed to sniff both tags. */
  k_worker_webp_tag_len  = 4U,  /**< Length of one fourCC tag.        */
} worker_webp_t;

/** @brief WebP RIFF container tag (source head bytes 0..3). */
static const uint8_t s_worker_riff[k_worker_webp_tag_len] = {'R', 'I', 'F', 'F'};

/** @brief WebP form-type fourCC (source head bytes 8..11). */
static const uint8_t s_worker_webp[k_worker_webp_tag_len] = {'W', 'E', 'B', 'P'};

/** @brief Pull cursor over the in-RAM encoded source. */
typedef struct {
  const uint8_t* data; /**< Encoded bytes. */
  size_t         len;  /**< Total length.  */
  size_t         pos;  /**< Read cursor.   */
} worker_pull_t;

/** @brief Sink state over the raw output descriptor. */
typedef struct {
  int  fd;     /**< Open output descriptor.            */
  bool failed; /**< Latched true on any write failure. */
} worker_sink_t;

/**
 * @brief Read exactly `len` bytes unless the descriptor fails or ends early.
 * @details Loops over `read()` until `len` bytes have been transferred. Handles
 *          transient `EINTR` signals without aborting. Returns false if EOF is
 *          encountered prematurely or an unrecoverable read error occurs.
 * @param[in]  fd  Open input descriptor.
 * @param[out] buf Destination holding `len` writable bytes.
 * @param[in]  len Byte count to read.
 * @return Whether all `len` bytes arrived.
 * @retval true  All @p len bytes were read successfully into @p buf.
 * @retval false Reading failed or reached EOF before @p len bytes arrived.
 * @pre @p fd is a valid open file descriptor.
 * @pre @p buf points to at least @p len writable bytes.
 * @post On true, @p buf contains @p len bytes read from @p fd.
 * @post On false, the buffer content is undefined and partial.
 * @note Reentrant and thread-safe for distinct descriptors.
 * @since 0.1.0
 */
static bool worker_read_all(int fd, uint8_t* buf, size_t len)
{
  size_t done = 0U;
  while (done < len) {
    const ssize_t got = read(fd, buf + done, len - done);
    if (got == 0) {
      return false;
    }
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    done += (size_t)got;
  }
  return true;
}

/**
 * @brief Append producer output bytes to the output descriptor.
 * @details Writes atlas bytes sequentially to the underlying file descriptor.
 *          Handles partial transfers and `EINTR` interrupts. On write error or
 *          unexpected EOF, sets the `failed` flag in the sink context and returns
 *          `k_ra8_fail`.
 * @param[in,out] ctx Sink state (a `worker_sink_t *`).
 * @param[in]     buf Atlas bytes to append.
 * @param[in]     len Readable byte count at `buf`.
 * @return Sink status.
 * @retval k_ra8_ok   All @p len bytes were appended successfully.
 * @retval k_ra8_fail Write operation failed or descriptor was closed.
 * @pre @p ctx is non-NULL and points to an open @p worker_sink_t structure.
 * @pre @p buf points to at least @p len readable bytes.
 * @post On success, exactly @p len bytes are appended to the descriptor.
 * @post On failure, `ctx->failed` is set to true.
 * @note Reentrant across distinct sink contexts.
 * @since 0.1.0
 */
static ra8_err_t worker_sink(void* ctx, const uint8_t* buf, size_t len)
{
  worker_sink_t* s    = (worker_sink_t*)ctx;
  size_t         done = 0U;
  while (done < len) {
    const ssize_t wrote = write(s->fd, buf + done, len - done);
    if (wrote == 0) {
      s->failed = true;
      return k_ra8_fail;
    }
    if (wrote < 0) {
      if (errno == EINTR) {
        continue;
      }
      s->failed = true;
      return k_ra8_fail;
    }
    done += (size_t)wrote;
  }
  return k_ra8_ok;
}

/**
 * @brief Copy the next encoded source span into a producer buffer.
 * @details Reads up to `cap` bytes from the in-memory cursor and advances the
 *          read position. When the cursor reaches the end of the source buffer,
 *          reports zero bytes read to indicate end-of-input.
 * @param[in,out] ctx Pull cursor (a `worker_pull_t *`).
 * @param[out]    buf Destination holding `cap` writable bytes.
 * @param[in]     cap Writable capacity of `buf`.
 * @param[out]    got Bytes copied (0 at end of input).
 * @return Status code (always `k_ra8_ok`).
 * @retval k_ra8_ok The memory span was copied successfully.
 * @pre @p ctx is non-NULL and points to valid source data.
 * @pre @p buf points to at least @p cap writable bytes.
 * @post `*got` holds the count of transferred bytes (up to @p cap).
 * @post `ctx->pos` advances by `*got` bytes.
 * @note Reentrant across distinct pull cursor instances.
 * @since 0.1.0
 */
static ra8_err_t worker_pull(void* ctx, uint8_t* buf, size_t cap, size_t* got)
{
  worker_pull_t* s = (worker_pull_t*)ctx;
  size_t         n = s->len - s->pos;
  if (n > cap) {
    n = cap;
  }
  memcpy(buf, s->data + s->pos, n);
  s->pos += n;
  *got = n;
  return k_ra8_ok;
}

/**
 * @brief Test whether source bytes carry the WebP RIFF container head.
 * @details Mirrors the producer's own dispatch sniff: both fourCCs must match,
 *          so a non-WebP RIFF (WAVE, AVI) is not mistaken for WebP and charged
 *          the whole-frame arena.
 * @param[in] data Source bytes.
 * @param[in] len  Readable byte count at `data`.
 * @return Whether `data` begins with a WebP container head.
 * @retval true  Source begins with RIFF and WEBP fourCC identifiers.
 * @retval false Source is shorter than header length or tags do not match.
 * @pre @p data points to at least @p len readable bytes.
 * @pre @p len is a valid non-negative byte length.
 * @post No memory or descriptor state is modified.
 * @post Return value depends strictly on first 12 bytes of @p data.
 * @note Reentrant and thread-safe.
 * @since 0.1.0
 */
static bool worker_is_webp(const uint8_t* data, size_t len)
{
  if (len < (size_t)k_worker_webp_head_len) {
    return false;
  }
  return (memcmp(&data[k_worker_webp_riff_ofs], s_worker_riff, sizeof(s_worker_riff)) == 0) &&
         (memcmp(&data[k_worker_webp_form_ofs], s_worker_webp, sizeof(s_worker_webp)) == 0);
}

/**
 * @brief Map a producer return code to the worker result contract.
 * @details Geometry-shaped codes (bad tile math, over-budget source, short
 *          arena) become `geometry`; hostile-or-unsupported sources become
 *          `decode`; arena exhaustion becomes `memory`. Sink/pull failures
 *          never reach here: the caller applies the output-close, sink, pull,
 *          producer-error precedence before consulting this map.
 * @param[in] err Producer return code.
 * @return Mapped worker result (never `ok`: success is decided by the caller).
 * @retval k_jof_worker_geometry Input dimensions or tile sizing cannot be produced.
 * @retval k_jof_worker_memory   Arena memory limit was exceeded during transcode.
 * @retval k_jof_worker_decode   Corrupted, hostile, or unsupported image encoding.
 * @pre @p err is a valid ::ra8_err_t failure status.
 * @pre @p err is not `k_ra8_ok`.
 * @post No system or worker state is modified.
 * @post Output is uniquely mapped according to error category.
 * @note Pure function; reentrant and thread-safe.
 * @since 0.1.0
 */
static jof_worker_result_t worker_map_producer(ra8_err_t err)
{
  switch (err) {
    case k_ra8_err_invalid_arg:
    case k_ra8_err_invalid_size:
      return k_jof_worker_geometry;
    case k_ra8_err_no_mem:
      return k_jof_worker_memory;
    default:
      return k_jof_worker_decode;
  }
}

/**
 * @brief Read encoded source file into an allocated RAM buffer.
 * @details Opens the source file, inspects file status to ensure it is within
 *          accepted input limits, allocates a memory buffer of the exact file
 *          size, and reads the entire file contents. Closes the descriptor before
 *          returning.
 * @param[in]  in_path Path to the encoded source image.
 * @param[out] out_src Destination pointer receiving the allocated source bytes.
 * @param[out] out_len Destination pointer receiving the source byte count.
 * @return One ::jof_worker_result_t member.
 * @retval k_jof_worker_ok     File read successfully into allocated memory.
 * @retval k_jof_worker_input  File open, stat, or read operation failed.
 * @retval k_jof_worker_memory Failed to allocate heap memory for source image.
 * @pre @p in_path is non-NULL and points to a NUL-terminated path.
 * @pre @p out_src and @p out_len are non-NULL destination pointers.
 * @post On success, `*out_src` contains allocated file bytes and `*out_len` is its size.
 * @post On failure, no heap memory is leaked and descriptor is closed.
 * @note Reentrant for distinct input paths.
 * @since 0.1.0
 */
static jof_worker_result_t
worker_read_source(const char* in_path, uint8_t** out_src, size_t* out_len)
{
  const int in_fd = open(in_path, O_RDONLY);
  if (in_fd < 0) {
    return k_jof_worker_input;
  }
  struct stat st;
  if (fstat(in_fd, &st) != 0) {
    (void)close(in_fd);
    return k_jof_worker_input;
  }
  if ((st.st_size <= 0) || ((uint64_t)st.st_size > (uint64_t)k_worker_max_input)) {
    (void)close(in_fd);
    return k_jof_worker_input;
  }
  const size_t src_len = (size_t)st.st_size;
  uint8_t*     src = (uint8_t*)malloc(src_len); /* alloc-allow: bounded probe-sized source image */
  if (src == nullptr) {
    (void)close(in_fd);
    return k_jof_worker_memory;
  }
  if (!worker_read_all(in_fd, src, src_len)) {
    free(src); /* alloc-allow: bounded probe-sized source image */
    (void)close(in_fd);
    return k_jof_worker_input;
  }
  (void)close(in_fd);
  *out_src = src;
  *out_len = src_len;
  return k_jof_worker_ok;
}

/**
 * @brief Allocate the WebP frame arena if the source format requires one.
 * @details Checks if the source bytes match the WebP container header. If so,
 *          computes the required whole-frame arena byte size using
 *          `jof_webp_work_bytes()` and allocates the arena via malloc. For non-WebP
 *          sources, returns success with a NULL arena.
 * @param[in]  src           Source image bytes.
 * @param[in]  src_len       Byte length of source image bytes.
 * @param[in]  w             Source image width, pixels.
 * @param[in]  h             Source image height, pixels.
 * @param[out] out_webp_work Destination pointer receiving the arena.
 * @param[out] out_webp_cap  Destination pointer receiving the arena capacity.
 * @return One ::jof_worker_result_t member.
 * @retval k_jof_worker_ok       Arena allocated or not needed for non-WebP image.
 * @retval k_jof_worker_geometry WebP source dimensions exceed allowable sizing.
 * @retval k_jof_worker_memory   Heap allocation for WebP scratch arena failed.
 * @pre @p src points to @p src_len readable bytes.
 * @pre @p out_webp_work and @p out_webp_cap are non-NULL destination pointers.
 * @post On success with WebP, `*out_webp_work` holds the allocated buffer.
 * @post On non-WebP source, `*out_webp_work` is set to NULL and capacity to 0.
 * @note Reentrant and thread-safe.
 * @since 0.1.0
 */
static jof_worker_result_t worker_alloc_webp(const uint8_t* src,
                                             size_t         src_len,
                                             uint16_t       w,
                                             uint16_t       h,
                                             uint8_t**      out_webp_work,
                                             size_t*        out_webp_cap)
{
  *out_webp_work = nullptr;
  *out_webp_cap  = 0U;
  if (!worker_is_webp(src, src_len)) {
    return k_jof_worker_ok;
  }
  if (src_len > UINT32_MAX) {
    return k_jof_worker_geometry;
  }
  const uint32_t webp_need = jof_webp_work_bytes(w, h, (uint32_t)src_len);
  if (webp_need == 0U) {
    return k_jof_worker_geometry;
  }
  uint8_t* webp_work =
    (uint8_t*)malloc(webp_need); /* alloc-allow: exact jof_webp_work_bytes arena */
  if (webp_work == nullptr) {
    return k_jof_worker_memory;
  }
  *out_webp_work = webp_work;
  *out_webp_cap  = (size_t)webp_need;
  return k_jof_worker_ok;
}

/**
 * @brief Stream JOF producer output into the destination atlas file.
 * @details Opens the output file descriptor with creation and truncation flags,
 *          initializes pull cursor and sink descriptors, constructs the producer
 *          configuration structure, and invokes `jof_produce()`. Closes the
 *          output descriptor upon completion and evaluates sink status.
 * @param[in] out_path      Destination JOF atlas path.
 * @param[in] src           Source image bytes.
 * @param[in] src_len       Source image byte count.
 * @param[in] w             Image width, pixels.
 * @param[in] h             Image height, pixels.
 * @param[in] tile_h        Tile height, pixels.
 * @param[in] work          Producer work arena.
 * @param[in] work_cap      Capacity of work arena, bytes.
 * @param[in] webp_work     Optional WebP frame arena.
 * @param[in] webp_work_cap Capacity of WebP arena, bytes.
 * @return One ::jof_worker_result_t member.
 * @retval k_jof_worker_ok       Atlas produced and written successfully.
 * @retval k_jof_worker_output   Output file creation, write, or close failed.
 * @retval k_jof_worker_geometry Dimension or tile geometry rejected by producer.
 * @retval k_jof_worker_decode   Image decode failed due to corrupted data.
 * @retval k_jof_worker_memory   Internal producer arena was exhausted.
 * @pre @p out_path is non-NULL and points to a valid destination path.
 * @pre @p work points to at least @p work_cap writable bytes.
 * @post On success, the complete JOF atlas is written to @p out_path.
 * @post The output file descriptor is always closed before return.
 * @note Not thread-safe due to producer module-static decoder contexts.
 * @since 0.1.0
 */
static jof_worker_result_t worker_produce_to_file(const char*    out_path,
                                                  const uint8_t* src,
                                                  size_t         src_len,
                                                  uint16_t       w,
                                                  uint16_t       h,
                                                  uint16_t       tile_h,
                                                  uint8_t*       work,
                                                  size_t         work_cap,
                                                  uint8_t*       webp_work,
                                                  size_t         webp_work_cap)
{
  const int out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, (mode_t)k_worker_out_mode);
  if (out_fd < 0) {
    return k_jof_worker_output;
  }
  worker_pull_t           pull = {.data = src, .len = src_len, .pos = 0U};
  worker_sink_t           sink = {.fd = out_fd, .failed = false};
  const jof_produce_cfg_t cfg  = {
    .pull          = worker_pull,
    .pull_ctx      = &pull,
    .sink          = worker_sink,
    .sink_ctx      = &sink,
    .tile_w        = w,
    .tile_h        = tile_h,
    .codec         = (uint8_t)k_jof_codec_deflate,
    .max_width     = w,
    .max_height    = h,
    .work          = work,
    .work_cap      = work_cap,
    .webp_work     = webp_work,
    .webp_work_cap = webp_work_cap,
  };
  jof_info_t      info       = {};
  const ra8_err_t produce_rc = jof_produce(&cfg, &info);
  const int       close_rc   = close(out_fd);

  if ((close_rc != 0) || sink.failed) {
    return k_jof_worker_output;
  }
  if (produce_rc != k_ra8_ok) {
    return worker_map_producer(produce_rc);
  }
  return k_jof_worker_ok;
}

RA8_NASA_RULE_3_OK("host-only single-image transcode: three bounded arenas sized by probe")
jof_worker_result_t jof_worker_convert(const char* in_path, const char* out_path)
{
  if ((in_path == nullptr) || (out_path == nullptr)) {
    return k_jof_worker_usage;
  }
  uint8_t*            src     = nullptr;
  size_t              src_len = 0U;
  jof_worker_result_t rc      = worker_read_source(in_path, &src, &src_len);
  if (rc != k_jof_worker_ok) {
    return rc;
  }
  uint16_t w = 0U;
  uint16_t h = 0U;
  if (jof_probe_dims(src, src_len, &w, &h) != k_ra8_ok) {
    free(src); /* alloc-allow: bounded probe-sized source image */
    return k_jof_worker_geometry;
  }
  const uint16_t tile_h   = (h > (uint16_t)k_worker_tile_h) ? (uint16_t)k_worker_tile_h : h;
  const uint32_t work_cap = jof_work_bytes(w, h, w, tile_h);
  if (work_cap == 0U) {
    free(src); /* alloc-allow: bounded probe-sized source image */
    return k_jof_worker_geometry;
  }
  uint8_t* work = (uint8_t*)malloc(work_cap); /* alloc-allow: exact jof_work_bytes arena */
  if (work == nullptr) {
    free(src); /* alloc-allow: bounded probe-sized source image */
    return k_jof_worker_memory;
  }
  uint8_t* webp_work     = nullptr;
  size_t   webp_work_cap = 0U;
  rc                     = worker_alloc_webp(src, src_len, w, h, &webp_work, &webp_work_cap);
  if (rc == k_jof_worker_ok) {
    rc = worker_produce_to_file(out_path,
                                src,
                                src_len,
                                w,
                                h,
                                tile_h,
                                work,
                                (size_t)work_cap,
                                webp_work,
                                webp_work_cap);
  }
  free(webp_work); /* alloc-allow: exact jof_webp_work_bytes arena  */
  free(work);      /* alloc-allow: exact jof_work_bytes arena       */
  free(src);       /* alloc-allow: bounded probe-sized source image */
  if (rc != k_jof_worker_ok) {
    (void)unlink(out_path);
  }
  return rc;
}
