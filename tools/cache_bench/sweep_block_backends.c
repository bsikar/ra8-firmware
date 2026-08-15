/**
 * @file sweep_block_backends.c
 * @brief Streamed pseudo-memory and RBKC-z9 block-sweep backends.
 *
 * @details RBKC construction writes directly to an injected scratch
 * transaction. Compression state temporarily overlays the 1 MiB cache backing
 * before the measured cache exists; the region is zeroed before it is rebound
 * as frame storage. Chunk offsets and compressed input are read on demand, so
 * no container, table, or max-chunk staging array is resident.
 *
 * [Ring 7 / Tooling] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#include <string.h>
#include <time.h>

#include "miniz.h"
#include "ra8_book_chunked.h"
#include "ra8_err.h"
#include "sweep_block_internal.h"

typedef enum : uint32_t {
  k_cbs_codec_input_bytes = 4096U, /**< Streamed codec transfer grain.   */
  k_cbs_codec_window_bits = 15U,   /**< Zlib window size as log2(bytes). */
} cbs_codec_limit_t;

static_assert(sizeof(tdefl_compressor) <= (size_t)k_cbs_cache_bytes,
              "tdefl overlay must fit the semantic cache backing");
static_assert(alignof(max_align_t) >= alignof(tdefl_compressor),
              "composition cache alignment must satisfy tdefl");

typedef struct {
  cb_scratch_t* scratch; /**< Destination transaction.    */
  uint64_t      offset;  /**< Next destination byte.      */
  bool          failed;  /**< Sticky publication failure. */
} cbs_pack_sink_t;

typedef struct {
  cb_scratch_t*       scratch;        /**< Container source transaction.  */
  tinfl_decompressor* inflater;       /**< Caller-owned inflate state.    */
  uint8_t*            input;          /**< Compressed-input staging.      */
  uint32_t            input_capacity; /**< Input staging extent.          */
  uint32_t            chunk_bytes;    /**< Logical bytes per chunk.       */
  uint32_t            chunk_count;    /**< Container chunk count.         */
  uint64_t            total_bytes;    /**< Logical payload extent.        */
  uint64_t            payload_offset; /**< First compressed payload byte. */
  uint64_t            source_bytes;   /**< Complete container extent.     */
} cbs_rbkc_t;

RA8_PRIV
uint64_t priv_now_ns(void)
{
  struct timespec ts = {};
  (void)clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((uint64_t)ts.tv_sec * (uint64_t)k_cbs_ns_per_s) + (uint64_t)ts.tv_nsec;
}

RA8_PRIV
ra8_err_t priv_meter_read(void* ctx, uint64_t offset, uint8_t* buffer, uint32_t length)
{
  cbs_meter_t* meter = (cbs_meter_t*)ctx;
  if ((meter == nullptr) || (meter->inner == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  meter->calls++;
  meter->bytes += length;
  return meter->inner(meter->inner_ctx, offset, buffer, length);
}

/**
 * @brief Bind the deterministic payload directly as an uncompressed backend.
 * @details Publishes the injected read callback and logical blob size without
 *          allocating, copying, or using the scratch transaction.
 * @param[in,out] be Backend record to bind.
 * @param[in] payload_read Deterministic payload reader.
 * @param[in] payload_ctx Payload reader context.
 * @param[in] blob_bytes Logical source length.
 * @param[in] block_bytes Swept block size, unused by direct memory.
 * @param[in,out] config Sweep composition, unused by direct memory.
 * @return Zero on a valid bind, otherwise one.
 * @retval 0 @p be publishes the injected source.
 * @retval 1 A required binding is absent or the blob is empty.
 * @pre @p be is writable.
 * @pre @p payload_read and @p payload_ctx remain valid through teardown.
 * @post On success, `be->backing_bytes == blob_bytes`.
 * @post No ownership changes and no bytes are copied.
 * @note The block and config parameters preserve the common backend seam.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_mem_setup(cbs_backend_t*      be,
                              ra8_vsource_read_fn payload_read,
                              void*               payload_ctx,
                              uint32_t            blob_bytes,
                              uint32_t            block_bytes,
                              cb_sweep_config_t*  config)
{
  (void)block_bytes;
  (void)config;
  if ((be == nullptr) || (payload_read == nullptr) || (payload_ctx == nullptr) ||
      (blob_bytes == 0U)) {
    return 1;
  }
  be->read          = payload_read;
  be->read_ctx      = payload_ctx;
  be->backing_bytes = blob_bytes;
  be->src_bytes     = nullptr;
  return 0;
}

/**
 * @brief Clear the borrowed runtime bindings of one backend.
 * @details Makes teardown idempotent by nulling the read, context, and source
 *          counter pointers while leaving static identity fields unchanged.
 * @param[in,out] be Backend to unbind; NULL is accepted.
 * @pre @p be is NULL or points to an initialized backend record.
 * @pre No read is in flight through @p be.
 * @post A non-NULL backend has no active read or counter binding.
 * @post Static name, setup, teardown, and backing-byte fields are unchanged.
 * @note Caller-owned payload and scratch storage are not released here.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_backend_teardown(cbs_backend_t* be)
{
  if (be != nullptr) {
    be->read      = nullptr;
    be->read_ctx  = nullptr;
    be->src_bytes = nullptr;
  }
}

/**
 * @brief Encode the fixed RBKC container header into caller storage.
 * @details Writes magic, block geometry, total length, chunk count, and the
 *          reserved zero field in the reader's native container layout.
 * @param[out] output Header buffer of ::k_ra8_book_container_header_len bytes.
 * @param[in] block_bytes Chunk size encoded in the header.
 * @param[in] total Logical uncompressed byte length.
 * @param[in] count Number of chunks.
 * @pre @p output is non-NULL and large enough for the fixed header.
 * @pre @p block_bytes and @p count describe @p total consistently.
 * @post Every header field is initialized.
 * @post No storage outside the fixed header span is touched.
 * @note Encoding matches the in-tree ::ra8_book_chunked reader contract.
 * @since 0.1.0
 */
RA8_INTERNAL
static void
internal_rbkc_header(uint8_t* output, uint32_t block_bytes, uint64_t total, uint32_t count)
{
  size_t position = 0U;
  memcpy(&output[position], "RBKC", sizeof("RBKC") - 1U);
  position += sizeof("RBKC") - 1U;
  memcpy(&output[position], &block_bytes, sizeof(block_bytes));
  position += sizeof(block_bytes);
  memcpy(&output[position], &total, sizeof(total));
  position += sizeof(total);
  memcpy(&output[position], &count, sizeof(count));
  position += sizeof(count);
  const uint32_t reserved = 0U;
  memcpy(&output[position], &reserved, sizeof(reserved));
}

/**
 * @brief Append one compressor fragment to the injected scratch transaction.
 * @details Implements miniz's output callback and advances the absolute
 *          scratch offset only after a complete injected write.
 * @param[in] data Compressor output bytes.
 * @param[in] length Fragment length from miniz.
 * @param[in,out] user Bound ::cbs_pack_sink_t.
 * @return Miniz callback status.
 * @retval MZ_TRUE The fragment was appended.
 * @retval MZ_FALSE An argument, prior failure, or scratch write failed.
 * @pre @p user identifies a live scratch transaction.
 * @pre @p data is readable for non-negative @p length bytes.
 * @post On success, the sink offset advances by @p length.
 * @post On scratch failure, `sink->failed` remains latched.
 * @note The callback never retries a failed injected transaction write.
 * @since 0.1.0
 */
RA8_INTERNAL
static mz_bool internal_pack_write(const void* data, int length, void* user)
{
  cbs_pack_sink_t* sink = (cbs_pack_sink_t*)user;
  if ((sink == nullptr) || (data == nullptr) || (length < 0) || sink->failed) {
    return MZ_FALSE;
  }
  const cb_io_status_t status =
    sink->scratch->write(sink->scratch->ctx, sink->offset, (void*)data, (size_t)length);
  if (status != k_cb_io_ok) {
    sink->failed = true;
    return MZ_FALSE;
  }
  sink->offset += (uint64_t)length;
  return MZ_TRUE;
}

/**
 * @brief Compress one logical payload chunk into the scratch stream.
 * @details Reinitializes tdefl for an independent zlib stream, pulls the chunk
 *          in bounded input grains, and finishes through ::internal_pack_write.
 * @param[in,out] compressor Caller-provided tdefl state.
 * @param[in,out] sink Scratch append binding.
 * @param[in] payload_read Payload read callback.
 * @param[in] payload_ctx Payload callback context.
 * @param[in,out] input Bounded codec input buffer.
 * @param[in] input_capacity Capacity of @p input.
 * @param[in] offset Payload start offset.
 * @param[in] length Uncompressed chunk length.
 * @return Zero after a complete zlib stream, otherwise one.
 * @retval 0 The chunk was fully compressed and appended.
 * @retval 1 Initialization, payload read, compression, or sink append failed.
 * @pre All pointer bindings are non-NULL and @p input_capacity is non-zero.
 * @pre The requested payload range is valid.
 * @post On success, @p sink points immediately after the chunk stream.
 * @post The compressor state may be reused only after reinitialization.
 * @note Each chunk intentionally starts a fresh zlib history.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_rbkc_pack_chunk(tdefl_compressor*   compressor,
                                    cbs_pack_sink_t*    sink,
                                    ra8_vsource_read_fn payload_read,
                                    void*               payload_ctx,
                                    uint8_t*            input,
                                    uint32_t            input_capacity,
                                    uint32_t            offset,
                                    uint32_t            length)
{
  const mz_uint flags = tdefl_create_comp_flags_from_zip_params(MZ_BEST_COMPRESSION,
                                                                (int)k_cbs_codec_window_bits,
                                                                MZ_DEFAULT_STRATEGY);
  if (tdefl_init(compressor, internal_pack_write, sink, (int)flags) != TDEFL_STATUS_OKAY) {
    return 1;
  }
  uint32_t complete = 0U;
  while (complete < length) {
    uint32_t span = length - complete;
    if (span > input_capacity) {
      span = input_capacity;
    }
    if (payload_read(payload_ctx, (uint64_t)offset + complete, input, span) != k_ra8_ok ||
        tdefl_compress_buffer(compressor, input, span, TDEFL_NO_FLUSH) < TDEFL_STATUS_OKAY) {
      return 1;
    }
    complete += span;
  }
  const tdefl_status status = tdefl_compress_buffer(compressor, nullptr, 0U, TDEFL_FINISH);
  return ((status == TDEFL_STATUS_DONE) && !sink->failed) ? 0 : 1;
}

/**
 * @brief Stream an RBKC container into the injected scratch transaction.
 * @details Writes the fixed header and offset table incrementally, overlays
 *          tdefl state on the future cache backing, then erases that backing.
 * @param[in] payload_read Deterministic payload reader.
 * @param[in] payload_ctx Payload reader context.
 * @param[in] blob_bytes Logical source length.
 * @param[in] block_bytes RBKC chunk size.
 * @param[in,out] config Scratch, cache overlay, and diagnostics.
 * @param[in,out] input Bounded codec input buffer.
 * @param[in] input_capacity Capacity of @p input.
 * @param[out] output_length Receives the container byte length.
 * @return Zero after complete construction, otherwise one.
 * @retval 0 Header, table, and all compressed chunks were written.
 * @retval 1 Alignment, capacity, read, compression, or scratch I/O failed.
 * @pre All bindings are non-NULL and sizes are non-zero.
 * @pre Cache backing satisfies tdefl size and alignment requirements.
 * @post On success, scratch size and @p output_length match the container end.
 * @post On success, the full cache backing is zeroed before cache binding.
 * @note The container and offset table are never materialized in RAM.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_rbkc_pack(ra8_vsource_read_fn payload_read,
                              void*               payload_ctx,
                              uint32_t            blob_bytes,
                              uint32_t            block_bytes,
                              cb_sweep_config_t*  config,
                              uint8_t*            input,
                              uint32_t            input_capacity,
                              uint64_t*           output_length)
{
  if ((sizeof(tdefl_compressor) > config->cache_capacity) ||
      (((uintptr_t)config->cache_backing % alignof(tdefl_compressor)) != 0U)) {
    config->workspace_required = sizeof(tdefl_compressor);
    return 1;
  }
  const uint32_t count          = (blob_bytes + block_bytes - 1U) / block_bytes;
  const uint64_t table_bytes    = ((uint64_t)count + 1U) * k_ra8_book_container_entry_len;
  const uint64_t payload_offset = (uint64_t)k_ra8_book_container_header_len + table_bytes;
  uint8_t        header[k_ra8_book_container_header_len] = {};
  internal_rbkc_header(header, block_bytes, blob_bytes, count);
  if (config->scratch->write(config->scratch->ctx, 0U, header, sizeof(header)) != k_cb_io_ok) {
    return 1;
  }
  uint64_t relative = 0U;
  if (config->scratch->write(config->scratch->ctx,
                             k_ra8_book_container_header_len,
                             &relative,
                             sizeof(relative)) != k_cb_io_ok) {
    return 1;
  }
  tdefl_compressor* compressor = (tdefl_compressor*)config->cache_backing;
  cbs_pack_sink_t   sink       = {.scratch = config->scratch, .offset = payload_offset};
  for (uint32_t chunk = 0U; chunk < count; ++chunk) {
    const uint32_t offset = chunk * block_bytes;
    uint32_t       length = blob_bytes - offset;
    if (length > block_bytes) {
      length = block_bytes;
    }
    if (internal_rbkc_pack_chunk(compressor,
                                 &sink,
                                 payload_read,
                                 payload_ctx,
                                 input,
                                 input_capacity,
                                 offset,
                                 length) != 0) {
      return 1;
    }
    relative                    = sink.offset - payload_offset;
    const uint64_t table_offset = (uint64_t)k_ra8_book_container_header_len +
                                  (((uint64_t)chunk + 1U) * k_ra8_book_container_entry_len);
    if (config->scratch->write(config->scratch->ctx, table_offset, &relative, sizeof(relative)) !=
        k_cb_io_ok) {
      return 1;
    }
  }
  *output_length        = sink.offset;
  config->scratch->size = sink.offset;
  memset(config->cache_backing, 0, config->cache_capacity);
  return 0;
}

/**
 * @brief Read and validate one compressed chunk's relative offset pair.
 * @details Fetches adjacent table entries through the injected scratch seam
 *          and checks monotonicity plus the physical container bound.
 * @param[in,out] rb Bound RBKC reader state.
 * @param[in] chunk Chunk-table index.
 * @param[out] begin Receives the relative compressed start.
 * @param[out] end Receives the relative compressed end.
 * @return Repository error code.
 * @retval k_ra8_ok The offset pair is ordered and in range.
 * @retval k_ra8_err_invalid_size A read or geometry validation failed.
 * @pre @p rb, @p begin, and @p end are non-NULL.
 * @pre @p chunk is less than `rb->chunk_count`.
 * @post On success, `*begin <= *end` within the scratch payload.
 * @post Reader state and scratch bytes are not modified.
 * @note Offsets are relative to `rb->payload_offset`.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_rbkc_offsets(cbs_rbkc_t* rb, uint32_t chunk, uint64_t* begin, uint64_t* end)
{
  const uint64_t table_offset =
    (uint64_t)k_ra8_book_container_header_len + ((uint64_t)chunk * k_ra8_book_container_entry_len);
  if ((rb->scratch->read(rb->scratch->ctx, table_offset, begin, sizeof(*begin)) != k_cb_io_ok) ||
      (rb->scratch->read(rb->scratch->ctx,
                         table_offset + k_ra8_book_container_entry_len,
                         end,
                         sizeof(*end)) != k_cb_io_ok) ||
      (*end < *begin) || ((rb->payload_offset + *end) > rb->scratch->size)) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

/**
 * @brief Inflate one exact RBKC chunk from scratch into caller storage.
 * @details Pulls bounded compressed grains, drives tinfl in non-wrapping mode,
 *          and rejects stalled, truncated, extra-input, or wrong-size streams.
 * @param[in,out] rb Bound RBKC reader and traffic counter.
 * @param[in] begin Relative compressed start.
 * @param[in] end Relative compressed end.
 * @param[out] output Destination buffer.
 * @param[in] output_length Exact expected uncompressed length.
 * @return Repository error code.
 * @retval k_ra8_ok Exactly one stream produced the requested bytes.
 * @retval k_ra8_err_invalid_size Scratch I/O or stream geometry failed.
 * @pre @p rb and @p output are non-NULL and `begin < end`.
 * @pre @p output is writable for @p output_length bytes.
 * @post On success, all compressed bytes and output bytes are consumed exactly.
 * @post `rb->source_bytes` includes every compressed grain read.
 * @note The inflater and input workspace are caller-owned and reused per chunk.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_rbkc_inflate(cbs_rbkc_t* rb,
                                       uint64_t    begin,
                                       uint64_t    end,
                                       uint8_t*    output,
                                       uint32_t    output_length)
{
  tinfl_init(rb->inflater);
  uint64_t     source   = begin;
  uint32_t     produced = 0U;
  tinfl_status status   = TINFL_STATUS_NEEDS_MORE_INPUT;
  while (status != TINFL_STATUS_DONE) {
    uint32_t input_length = (uint32_t)(end - source);
    if (input_length > rb->input_capacity) {
      input_length = rb->input_capacity;
    }
    if ((input_length == 0U) || (rb->scratch->read(rb->scratch->ctx,
                                                   rb->payload_offset + source,
                                                   rb->input,
                                                   input_length) != k_cb_io_ok)) {
      return k_ra8_err_invalid_size;
    }
    rb->source_bytes += input_length;
    size_t          consumed  = input_length;
    size_t          available = output_length - produced;
    const mz_uint32 flags =
      (mz_uint32)(TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF |
                  (((source + input_length) < end) ? TINFL_FLAG_HAS_MORE_INPUT : 0U));
    status = tinfl_decompress(rb->inflater,
                              rb->input,
                              &consumed,
                              output,
                              &output[produced],
                              &available,
                              flags);
    source += consumed;
    produced += (uint32_t)available;
    if ((status < TINFL_STATUS_DONE) || (consumed == 0U)) {
      return k_ra8_err_invalid_size;
    }
  }
  return ((source == end) && (produced == output_length)) ? k_ra8_ok : k_ra8_err_invalid_size;
}

/**
 * @brief Serve one aligned logical chunk through the RBKC backend seam.
 * @details Validates the vsource request geometry, fetches its table offsets,
 *          and inflates exactly the requested logical chunk.
 * @param[in] ctx Bound ::cbs_rbkc_t.
 * @param[in] offset Chunk-aligned logical byte offset.
 * @param[out] buffer Destination buffer.
 * @param[in] length Exact logical chunk length.
 * @return Repository error code.
 * @retval k_ra8_ok The chunk was decoded exactly.
 * @retval k_ra8_err_null_ptr A required binding is NULL.
 * @retval k_ra8_err_out_of_range The offset is outside or misaligned.
 * @retval k_ra8_err_invalid_size The length, table, or stream is invalid.
 * @pre @p buffer is writable for @p length bytes.
 * @pre @p ctx remains bound until backend teardown.
 * @post On success, @p buffer holds the requested payload bytes.
 * @post Scratch bytes and logical geometry are not modified.
 * @note This is the exact ::ra8_vsource_read_fn bound into the measured cache.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_rbkc_read(void* ctx, uint64_t offset, uint8_t* buffer, uint32_t length)
{
  cbs_rbkc_t* rb = (cbs_rbkc_t*)ctx;
  if ((rb == nullptr) || (buffer == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((offset >= rb->total_bytes) || ((offset % rb->chunk_bytes) != 0U)) {
    return k_ra8_err_out_of_range;
  }
  const uint32_t chunk    = (uint32_t)(offset / rb->chunk_bytes);
  uint32_t       expected = (uint32_t)(rb->total_bytes - offset);
  if (expected > rb->chunk_bytes) {
    expected = rb->chunk_bytes;
  }
  if ((chunk >= rb->chunk_count) || (length != expected)) {
    return k_ra8_err_invalid_size;
  }
  uint64_t        begin  = 0U;
  uint64_t        end    = 0U;
  const ra8_err_t status = internal_rbkc_offsets(rb, chunk, &begin, &end);
  return (status == k_ra8_ok) ? internal_rbkc_inflate(rb, begin, end, buffer, length) : status;
}

/**
 * @brief Take one aligned setup region from the shared small workspace.
 * @details Advances `workspace_used` on success and always updates the exact
 *          required-capacity diagnostic for the attempted partition.
 * @param[in,out] config Sweep workspace state.
 * @param[in] bytes Requested byte count before alignment.
 * @return Borrowed region pointer, or NULL on insufficient capacity.
 * @retval NULL The aligned request does not fit.
 * @retval other Pointer to the reserved region.
 * @pre @p config and `config->workspace` are non-NULL.
 * @pre The aligned addition fits in `size_t` by fixed sweep geometry.
 * @post On success, `workspace_used` advances by the aligned span.
 * @post On failure, `workspace_required` exposes the attempted end offset.
 * @note Returned storage remains caller-owned.
 * @since 0.1.0
 */
RA8_INTERNAL
static void* internal_setup_take(cb_sweep_config_t* config, size_t bytes)
{
  const size_t alignment = alignof(max_align_t);
  const size_t span      = (bytes + alignment - 1U) & ~(alignment - 1U);
  if ((config->workspace_used > config->workspace_capacity) ||
      (span > (config->workspace_capacity - config->workspace_used))) {
    config->workspace_required = config->workspace_used + span;
    return nullptr;
  }
  void* result = &config->workspace[config->workspace_used];
  config->workspace_used += span;
  config->workspace_required = config->workspace_used;
  return result;
}

/**
 * @brief Construct and bind the streamed RBKC-z9 benchmark backend.
 * @details Temporarily reserves codec input, builds the scratch container,
 *          rolls back to the setup floor, then binds inflater reader state.
 * @param[in,out] be Backend record to bind.
 * @param[in] payload_read Deterministic payload reader.
 * @param[in] payload_ctx Payload reader context.
 * @param[in] blob_bytes Logical source length.
 * @param[in] block_bytes Swept chunk size.
 * @param[in,out] config Scratch and workspace composition.
 * @return Zero after a complete bind, otherwise one.
 * @retval 0 @p be publishes the RBKC read seam and traffic counter.
 * @retval 1 A binding, construction, or workspace check failed.
 * @pre Required pointers are non-NULL and @p block_bytes is non-zero.
 * @pre Scratch and cache backing meet the packer's contracts.
 * @post On success, @p be remains valid until teardown or workspace reuse.
 * @post Setup-only tdefl state no longer occupies the cache backing.
 * @note Codec workspace is phase-overlaid but never dynamically allocated.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_rbkc_setup(cbs_backend_t*      be,
                               ra8_vsource_read_fn payload_read,
                               void*               payload_ctx,
                               uint32_t            blob_bytes,
                               uint32_t            block_bytes,
                               cb_sweep_config_t*  config)
{
  if ((be == nullptr) || (payload_read == nullptr) || (config == nullptr) ||
      (config->scratch == nullptr) || (block_bytes == 0U)) {
    return 1;
  }
  uint8_t* input       = (uint8_t*)internal_setup_take(config, k_cbs_codec_input_bytes);
  uint64_t file_length = 0U;
  if ((input == nullptr) || (internal_rbkc_pack(payload_read,
                                                payload_ctx,
                                                blob_bytes,
                                                block_bytes,
                                                config,
                                                input,
                                                (uint32_t)k_cbs_codec_input_bytes,
                                                &file_length) != 0)) {
    return 1;
  }
  config->workspace_used = config->workspace_floor;
  cbs_rbkc_t*         rb = (cbs_rbkc_t*)internal_setup_take(config, sizeof(cbs_rbkc_t));
  tinfl_decompressor* inflater =
    (tinfl_decompressor*)internal_setup_take(config, sizeof(tinfl_decompressor));
  input = (uint8_t*)internal_setup_take(config, k_cbs_codec_input_bytes);
  if ((rb == nullptr) || (inflater == nullptr) || (input == nullptr)) {
    return 1;
  }
  const uint32_t count = (blob_bytes + block_bytes - 1U) / block_bytes;
  *rb = (cbs_rbkc_t){.scratch        = config->scratch,
                     .inflater       = inflater,
                     .input          = input,
                     .input_capacity = k_cbs_codec_input_bytes,
                     .chunk_bytes    = block_bytes,
                     .chunk_count    = count,
                     .total_bytes    = blob_bytes,
                     .payload_offset = (uint64_t)k_ra8_book_container_header_len +
                                       (((uint64_t)count + 1U) * k_ra8_book_container_entry_len)};
  be->read          = internal_rbkc_read;
  be->read_ctx      = rb;
  be->backing_bytes = file_length;
  be->src_bytes     = &rb->source_bytes;
  return 0;
}

static cbs_backend_t s_cbs_backends[] = {
  {.name = "mem", .setup = internal_mem_setup, .teardown = internal_backend_teardown},
  {.name = "rbkc-z9", .setup = internal_rbkc_setup, .teardown = internal_backend_teardown},
};

RA8_PRIV
cbs_backend_t* priv_backends(uint32_t* out_count)
{
  if (out_count != nullptr) {
    *out_count = (uint32_t)(sizeof(s_cbs_backends) / sizeof(s_cbs_backends[0]));
  }
  return s_cbs_backends;
}
