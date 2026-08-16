/**
 * @file mdl_state_store.c
 * @brief Portable checksummed two-generation persistence for downloader state.
 * @details Encodes, validates, publishes, and recovers bounded state generations through fw_fs.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mdl_state.h"
#include "mdl_state_internal.h"
#include "ra8_attributes.h"

/** @brief Canonical journal-envelope and bounded-I/O constants. */
typedef enum : uint32_t {
  k_state_magic_bytes                = 8U,                 /**< Journal magic width.          */
  k_state_header_bytes               = 40U,                /**< Canonical envelope width.     */
  k_state_header_crc_span            = 36U,                /**< Header bytes authenticated.   */
  k_state_envelope_v1                = 1U,                 /**< Envelope schema version.      */
  k_state_alt_suffix                 = 4U,                 /**< Bytes in `.alt`.              */
  k_state_io_call_max                = 8U * 1024U * 1024U, /**< Retry ceiling.                */
  k_state_crc32_polynomial           = 0xEDB88320UL,       /**< Reflected CRC-32 polynomial.  */
  k_state_be32_high_shift            = 24U,                /**< High-byte shift for uint32.   */
  k_state_be64_high_shift            = 56U,                /**< High-byte shift for uint64.   */
  k_state_header_version_offset      = 8U,                 /**< Envelope-version byte offset. */
  k_state_header_size_offset         = 10U,                /**< Header-size byte offset.      */
  k_state_header_reserved_offset     = 12U,                /**< Reserved field byte offset.   */
  k_state_header_sequence_offset     = 16U,                /**< Sequence field byte offset.   */
  k_state_header_payload_size_offset = 24U,                /**< Payload-size byte offset.     */
  k_state_header_payload_crc_offset  = 32U,                /**< Payload-CRC byte offset.      */
  k_state_header_crc_offset          = 36U,                /**< Header-CRC byte offset.       */
} mdl_state_store_limit_t;

/** @brief Canonical journal magic, encoded byte-for-byte. */
static const uint8_t s_state_magic[k_state_magic_bytes] = {'M', 'D', 'L', 'S', 'T', 'J', 'N', 'L'};

/** @brief Decoded canonical journal envelope. */
typedef struct {
  uint64_t sequence;      /**< Monotonic generation sequence. */
  uint64_t payload_bytes; /**< Exact v1/v2 text payload size. */
  uint32_t payload_crc32; /**< CRC-32 of payload bytes.       */
} mdl_state_envelope_t;

/** @brief One physical generation discovered under the logical path. */
typedef struct {
  const char*          path;       /**< Physical path.        */
  mdl_state_envelope_t envelope;   /**< Journal identity.     */
  uint64_t             file_bytes; /**< Observed file extent. */
  ra8_err_t            error;      /**< Scan status.          */
  bool                 exists;     /**< Entry exists.         */
  bool                 valid;      /**< Slot is intact.       */
  bool                 legacy;     /**< Slot is legacy text.  */
} mdl_state_slot_t;

/** @brief Streaming serialization state for one transaction. */
typedef struct {
  fw_fs_transaction_t* transaction;   /**< Active private stage.   */
  uint64_t             payload_bytes; /**< Bytes after envelope.   */
  uint32_t             crc_state;     /**< Unfinalized CRC-32.     */
  uint32_t             calls;         /**< Bounded write attempts. */
} mdl_state_writer_t;

/** @brief Expected stage identity passed to the independent validator. */
typedef struct {
  mdl_storage_t*       storage;  /**< Validation scratch. */
  mdl_state_envelope_t envelope; /**< Expected identity.  */
} mdl_state_validation_t;

/**
 * @brief Update an unfinalized reflected CRC-32 state.
 * @details @param[in] state Prior CRC state. @param[in] bytes Input bytes. @param[in] length Byte count. @return Updated unfinalized state. @retval UINT32_MAX Empty input from the initial state.
 * @pre @p bytes is non-NULL when length is nonzero. @pre Length describes accessible bytes.
 * @post Input remains unchanged. @post Exactly @p length bytes contribute.
 * @note Finalization is a separate bitwise complement. @since 0.1.0
 */
RA8_INTERNAL static uint32_t
internal_mdl_state_crc32_update(uint32_t state, const uint8_t* bytes, uint32_t length)
{
  for (uint32_t i = 0U; i < length; ++i) {
    state ^= bytes[i];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      const uint32_t mask = (uint32_t)(0U - (state & 1U));
      state               = (state >> 1U) ^ ((uint32_t)k_state_crc32_polynomial & mask);
    }
  }
  return state;
}

/**
 * @brief Encode one uint16 in canonical big-endian order.
 * @details @param[out] out Two-byte destination. @param[in] value Value to encode.
 * @pre @p out is non-NULL. @pre Two destination bytes are writable.
 * @post Both bytes are initialized. @post Decoding them recovers @p value.
 * @note Host endianness is irrelevant. @since 0.1.0
 */
RA8_INTERNAL static void internal_mdl_state_put_be16(uint8_t* out, uint16_t value)
{
  out[0] = (uint8_t)(value >> 8U);
  out[1] = (uint8_t)value;
}

/**
 * @brief Encode one uint32 in canonical big-endian order.
 * @details @param[out] out Four-byte destination. @param[in] value Value to encode.
 * @pre @p out is non-NULL. @pre Four destination bytes are writable.
 * @post Every byte is initialized. @post Decoding them recovers @p value.
 * @note Host endianness is irrelevant. @since 0.1.0
 */
RA8_INTERNAL static void internal_mdl_state_put_be32(uint8_t* out, uint32_t value)
{
  out[0] = (uint8_t)(value >> (uint32_t)k_state_be32_high_shift);
  out[1] = (uint8_t)(value >> 16U);
  out[2] = (uint8_t)(value >> 8U);
  out[3] = (uint8_t)value;
}

/**
 * @brief Encode one uint64 in canonical big-endian order.
 * @details @param[out] out Eight-byte destination. @param[in] value Value to encode.
 * @pre @p out is non-NULL. @pre Eight destination bytes are writable.
 * @post Every byte is initialized. @post Decoding them recovers @p value.
 * @note Uses only defined unsigned shifts. @since 0.1.0
 */
RA8_INTERNAL static void internal_mdl_state_put_be64(uint8_t* out, uint64_t value)
{
  for (uint8_t i = 0U; i < 8U; ++i) {
    out[i] = (uint8_t)(value >> ((uint32_t)k_state_be64_high_shift - (8U * i)));
  }
}

/**
 * @brief Decode one canonical big-endian uint16.
 * @details @param[in] in Two-byte source. @return Decoded value. @retval 0 Both bytes encode zero.
 * @pre @p in is non-NULL. @pre Two source bytes are readable.
 * @post Source remains unchanged. @post Result is independent of host endianness.
 * @note Input alignment is unrestricted. @since 0.1.0
 */
RA8_INTERNAL static uint16_t internal_mdl_state_get_be16(const uint8_t* in)
{
  return (uint16_t)(((uint16_t)in[0] << 8U) | (uint16_t)in[1]);
}

/**
 * @brief Decode one canonical big-endian uint32.
 * @details @param[in] in Four-byte source. @return Decoded value. @retval 0 All bytes encode zero.
 * @pre @p in is non-NULL. @pre Four source bytes are readable.
 * @post Source remains unchanged. @post Result is independent of host endianness.
 * @note Input alignment is unrestricted. @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_mdl_state_get_be32(const uint8_t* in)
{
  return ((uint32_t)in[0] << (uint32_t)k_state_be32_high_shift) | ((uint32_t)in[1] << 16U) |
         ((uint32_t)in[2] << 8U) | (uint32_t)in[3];
}

/**
 * @brief Decode one canonical big-endian uint64.
 * @details @param[in] in Eight-byte source. @return Decoded value. @retval 0 All bytes encode zero.
 * @pre @p in is non-NULL. @pre Eight source bytes are readable.
 * @post Source remains unchanged. @post Result is independent of host endianness.
 * @note Input alignment is unrestricted. @since 0.1.0
 */
RA8_INTERNAL static uint64_t internal_mdl_state_get_be64(const uint8_t* in)
{
  uint64_t value = 0U;
  for (uint8_t i = 0U; i < 8U; ++i) {
    value = (value << 8U) | (uint64_t)in[i];
  }
  return value;
}

/**
 * @brief Encode one self-checking canonical envelope.
 * @details @param[in] envelope Decoded identity. @param[out] out Header bytes.
 * @pre Both pointers are non-NULL. @pre Output spans the canonical header size.
 * @post Reserved bytes are zero. @post Header CRC authenticates every preceding field.
 * @note Encoding is canonical big-endian. @since 0.1.0
 */
RA8_INTERNAL static void internal_mdl_state_encode_header(const mdl_state_envelope_t* envelope,
                                                          uint8_t*                    out)
{
  memset(out, 0, (size_t)k_state_header_bytes);
  memcpy(out, s_state_magic, sizeof(s_state_magic));
  internal_mdl_state_put_be16(&out[k_state_header_version_offset], (uint16_t)k_state_envelope_v1);
  internal_mdl_state_put_be16(&out[k_state_header_size_offset], (uint16_t)k_state_header_bytes);
  internal_mdl_state_put_be64(&out[k_state_header_sequence_offset], envelope->sequence);
  internal_mdl_state_put_be64(&out[k_state_header_payload_size_offset], envelope->payload_bytes);
  internal_mdl_state_put_be32(&out[k_state_header_payload_crc_offset], envelope->payload_crc32);
  const uint32_t crc =
    ~internal_mdl_state_crc32_update(UINT32_MAX, out, (uint32_t)k_state_header_crc_span);
  internal_mdl_state_put_be32(&out[k_state_header_crc_offset], crc);
}

/**
 * @brief Decode and authenticate one canonical envelope.
 * @details @param[in] bytes Header bytes. @param[out] out Decoded identity. @return Authentication result. @retval false Magic, schema, reserved field, CRC, or sequence is invalid.
 * @pre Both pointers are non-NULL. @pre Input spans the canonical header size.
 * @post Success initializes every output field. @post Zero sequence remains forbidden.
 * @note Exact file extent is validated separately. @since 0.1.0
 */
RA8_INTERNAL static bool internal_mdl_state_decode_header(const uint8_t*        bytes,
                                                          mdl_state_envelope_t* out)
{
  const uint32_t expected = internal_mdl_state_get_be32(&bytes[k_state_header_crc_offset]);
  const uint32_t actual =
    ~internal_mdl_state_crc32_update(UINT32_MAX, bytes, (uint32_t)k_state_header_crc_span);
  if ((memcmp(bytes, s_state_magic, sizeof(s_state_magic)) != 0) ||
      (internal_mdl_state_get_be16(&bytes[k_state_header_version_offset]) !=
       (uint16_t)k_state_envelope_v1) ||
      (internal_mdl_state_get_be16(&bytes[k_state_header_size_offset]) !=
       (uint16_t)k_state_header_bytes) ||
      (internal_mdl_state_get_be32(&bytes[k_state_header_reserved_offset]) != 0U) ||
      (actual != expected)) {
    return false;
  }
  out->sequence      = internal_mdl_state_get_be64(&bytes[k_state_header_sequence_offset]);
  out->payload_bytes = internal_mdl_state_get_be64(&bytes[k_state_header_payload_size_offset]);
  out->payload_crc32 = internal_mdl_state_get_be32(&bytes[k_state_header_payload_crc_offset]);
  return out->sequence != 0U;
}

/**
 * @brief Read an exact byte count while rejecting zero progress.
 * @details @param[in,out] file Open stream. @param[out] out Destination. @param[in] length Byte count. @param[in,out] calls Attempt counter. @return Portable I/O status. @retval k_ra8_err_invalid_state Premature zero progress.
 * @pre All pointers are non-NULL. @pre Destination capacity is at least @p length.
 * @post Success initializes exactly @p length bytes. @post Attempts never exceed the global cap.
 * @note Short successful reads are retried. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_mdl_state_read_all(fw_fs_file_t* file, uint8_t* out, uint32_t length, uint32_t* calls)
{
  uint32_t offset = 0U;
  while (offset < length) {
    if (*calls >= (uint32_t)k_state_io_call_max) {
      return k_ra8_err_invalid_size;
    }
    uint32_t        count = 0U;
    const ra8_err_t err   = fw_fs_read(file, out + offset, length - offset, &count);
    ++(*calls);
    if (err != k_ra8_ok) {
      return err;
    }
    if (count == 0U) {
      return k_ra8_err_invalid_state;
    }
    offset += count;
  }
  return k_ra8_ok;
}

/**
 * @brief Hash one exact payload extent and reject early EOF.
 * @details @param[in,out] storage I/O scratch binding. @param[in,out] file Open stream. @param[in] length Exact extent. @param[out] out_crc Final CRC. @return Portable I/O status. @retval k_ra8_err_invalid_state Early zero progress.
 * @pre All pointers are non-NULL. @pre Storage scratch has positive capacity.
 * @post Success hashes exactly @p length bytes. @post Reads never cross that extent.
 * @note Work and attempts are bounded. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_state_hash_payload(mdl_storage_t* storage,
                                                              fw_fs_file_t*  file,
                                                              uint64_t       length,
                                                              uint32_t*      out_crc)
{
  uint64_t remaining = length;
  uint32_t state     = UINT32_MAX;
  uint32_t calls     = 0U;
  while (remaining > 0U) {
    if (calls >= (uint32_t)k_state_io_call_max) {
      return k_ra8_err_invalid_size;
    }
    const uint32_t  wanted = (remaining < (uint64_t)storage->io_buffer_bytes)
                               ? (uint32_t)remaining
                               : storage->io_buffer_bytes;
    uint32_t        count  = 0U;
    const ra8_err_t err    = fw_fs_read(file, storage->io_buffer, wanted, &count);
    ++calls;
    if (err != k_ra8_ok) {
      return err;
    }
    if (count == 0U) {
      return k_ra8_err_invalid_state;
    }
    state = internal_mdl_state_crc32_update(state, storage->io_buffer, count);
    remaining -= count;
  }
  *out_crc = ~state;
  return k_ra8_ok;
}

/**
 * @brief Validate an open journal envelope, extent, and checksum.
 * @details @param[in,out] storage Scratch binding. @param[in,out] file Open stream. @param[out] out Envelope. @return Validation status. @retval k_ra8_err_invalid_state Any canonical identity mismatch.
 * @pre All pointers are non-NULL. @pre File is open for reading.
 * @post Success leaves the stream after the payload. @post Trailing and missing bytes are rejected.
 * @note Payload parsing is deliberately separate. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_state_validate_open(mdl_storage_t*        storage,
                                                               fw_fs_file_t*         file,
                                                               mdl_state_envelope_t* out)
{
  uint64_t  file_bytes = 0U;
  ra8_err_t err        = fw_fs_file_size(file, &file_bytes);
  if ((err != k_ra8_ok) || (file_bytes < (uint64_t)k_state_header_bytes)) {
    return (err == k_ra8_ok) ? k_ra8_err_invalid_state : err;
  }
  err = fw_fs_seek(file, 0U);
  uint8_t  header[k_state_header_bytes];
  uint32_t calls = 0U;
  if (err == k_ra8_ok) {
    err = internal_mdl_state_read_all(file, header, sizeof(header), &calls);
  }
  if ((err != k_ra8_ok) || !internal_mdl_state_decode_header(header, out)) {
    return (err == k_ra8_ok) ? k_ra8_err_invalid_state : err;
  }
  if ((out->payload_bytes > (UINT64_MAX - (uint64_t)k_state_header_bytes)) ||
      (file_bytes != ((uint64_t)k_state_header_bytes + out->payload_bytes))) {
    return k_ra8_err_invalid_state;
  }
  uint32_t crc = 0U;
  err          = internal_mdl_state_hash_payload(storage, file, out->payload_bytes, &crc);
  if (err != k_ra8_ok) {
    return err;
  }
  return (crc == out->payload_crc32) ? k_ra8_ok : k_ra8_err_invalid_state;
}

/**
 * @brief Derive and validate both physical generation paths.
 * @details @param[in] storage Filesystem binding. @param[in] path Logical base. @param[out] base Base path. @param[out] alternate `.alt` path. @return Canonical path status. @retval k_ra8_err_invalid_size Suffix would exceed capacity.
 * @pre Output buffers are distinct and full-capacity. @pre Storage has all required scratch bindings.
 * @post Success initializes two valid paths. @post Failure performs no filesystem mutation.
 * @note No host path API is used. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_state_paths(const mdl_storage_t* storage,
                                                       const char*          path,
                                                       char*                base,
                                                       char*                alternate)
{
  if ((storage == nullptr) || (storage->fs == nullptr) || (path == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  if ((storage->file_workspace == nullptr) || (storage->transaction_workspace == nullptr) ||
      (storage->io_buffer == nullptr) || (storage->io_buffer_bytes == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  ra8_err_t err = fw_fs_path_validate(&storage->fs->caps, path);
  if (err != k_ra8_ok) {
    return err;
  }
  const size_t length = strlen(path);
  if ((length == 0U) || (length + (size_t)k_state_alt_suffix >= (size_t)k_fw_fs_path_cap)) {
    return k_ra8_err_invalid_size;
  }
  memcpy(base, path, length + 1U);
  memcpy(alternate, path, length);
  memcpy(&alternate[length], ".alt", (size_t)k_state_alt_suffix + 1U);
  return fw_fs_path_validate(&storage->fs->caps, alternate);
}

/**
 * @brief Scan one physical generation without parsing its payload.
 * @details @param[in,out] storage Scratch binding. @param[in] path Physical path. @param[in] allow_legacy Whether text is accepted. @param[out] out Slot result.
 * @pre All pointers are non-NULL. @pre @p path passed portable validation.
 * @post Output records existence, identity, and exact error. @post Every opened stream is closed.
 * @note Legacy recognition is restricted to the base slot. @since 0.1.0
 */
RA8_INTERNAL static void internal_mdl_state_scan_slot(mdl_storage_t*    storage,
                                                      const char*       path,
                                                      bool              allow_legacy,
                                                      mdl_state_slot_t* out)
{
  *out              = (mdl_state_slot_t){.path = path, .error = k_ra8_ok};
  fw_fs_stat_t stat = {};
  out->error        = fw_fs_stat(&storage->fs->names, path, &stat);
  if ((out->error != k_ra8_ok) || !stat.exists) {
    return;
  }
  out->exists     = true;
  out->file_bytes = stat.size_bytes;
  if (stat.type != k_fw_fs_node_file) {
    out->error = k_ra8_err_invalid_state;
    return;
  }
  fw_fs_file_t file = {};
  out->error        = fw_fs_open(&storage->fs->streams,
                                 path,
                                 k_fw_fs_open_read,
                                 &file,
                                 storage->file_workspace,
                                 storage->file_workspace_bytes);
  if (out->error == k_ra8_ok) {
    uint8_t    prefix[k_state_magic_bytes] = {};
    uint32_t   calls                       = 0U;
    const bool too_short                   = stat.size_bytes < sizeof(prefix);
    out->error =
      too_short ? k_ra8_ok : internal_mdl_state_read_all(&file, prefix, sizeof(prefix), &calls);
    const bool legacy_magic = (out->error == k_ra8_ok) &&
                              (too_short || (memcmp(prefix, s_state_magic, sizeof(prefix)) != 0));
    if (legacy_magic && allow_legacy) {
      out->legacy = true;
      out->valid  = true;
    } else if (legacy_magic) {
      out->error = k_ra8_err_invalid_state;
    } else if (out->error == k_ra8_ok) {
      out->error = internal_mdl_state_validate_open(storage, &file, &out->envelope);
      out->valid = out->error == k_ra8_ok;
    }
  }
  const ra8_err_t closed = file.is_open ? fw_fs_close(&file) : k_ra8_ok;
  if (closed != k_ra8_ok) {
    out->error = closed;
    out->valid = false;
  }
}

/**
 * @brief Check whether two slots describe the same generation.
 * @details @param[in] a First slot. @param[in] b Second slot. @return Identity comparison. @retval false Sequence, extent, or checksum differs.
 * @pre Both pointers are non-NULL. @pre Both envelopes were authenticated.
 * @post Inputs remain unchanged. @post Equal sequence alone is never sufficient.
 * @note Detects ambiguous divergent ties. @since 0.1.0
 */
RA8_INTERNAL static bool internal_mdl_state_same_generation(const mdl_state_slot_t* a,
                                                            const mdl_state_slot_t* b)
{
  return a->envelope.sequence == b->envelope.sequence &&
         a->envelope.payload_bytes == b->envelope.payload_bytes &&
         a->envelope.payload_crc32 == b->envelope.payload_crc32;
}

/**
 * @brief Order valid generations newest first.
 * @details @param[in] base Base slot. @param[in] alternate Alternate slot. @param[out] newest Newest valid slot. @param[out] older Other valid slot. @return Ordering status. @retval k_ra8_err_invalid_state Divergent equal sequences.
 * @pre All pointers are non-NULL. @pre Slot scan results are complete.
 * @post Outputs are NULL or alias input slots. @post No slot contents are modified.
 * @note Invalid generations are excluded from ordering. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_state_order(const mdl_state_slot_t*  base,
                                                       const mdl_state_slot_t*  alternate,
                                                       const mdl_state_slot_t** newest,
                                                       const mdl_state_slot_t** older)
{
  *newest = nullptr;
  *older  = nullptr;
  if (!base->valid && !alternate->valid) {
    return k_ra8_ok;
  }
  if (!base->valid || !alternate->valid) {
    *newest = base->valid ? base : alternate;
    return k_ra8_ok;
  }
  if (base->envelope.sequence == alternate->envelope.sequence) {
    if (!internal_mdl_state_same_generation(base, alternate)) {
      return k_ra8_err_invalid_state;
    }
    *newest = base;
    *older  = alternate;
    return k_ra8_ok;
  }
  const bool base_newer = base->envelope.sequence > alternate->envelope.sequence;
  *newest               = base_newer ? base : alternate;
  *older                = base_newer ? alternate : base;
  return k_ra8_ok;
}

/**
 * @brief Load and revalidate one previously scanned generation.
 * @details @param[in,out] storage Scratch binding. @param[in] slot Scanned slot. @param[out] st Destination state. @return Parse/I/O status. @retval k_ra8_err_invalid_state Identity changed or payload is corrupt.
 * @pre All pointers are non-NULL. @pre @p slot was valid during scanning.
 * @post Success transactionally replaces @p st. @post Failure resets @p st empty.
 * @note Journal identity is checked again after open. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_mdl_state_load_slot(mdl_storage_t* storage, const mdl_state_slot_t* slot, mdl_state_t* st)
{
  fw_fs_file_t         file     = {};
  ra8_err_t            err      = fw_fs_open(&storage->fs->streams,
                                             slot->path,
                                             k_fw_fs_open_read,
                                             &file,
                                             storage->file_workspace,
                                             storage->file_workspace_bytes);
  mdl_state_envelope_t observed = {};
  if ((err == k_ra8_ok) && !slot->legacy) {
    err = internal_mdl_state_validate_open(storage, &file, &observed);
    if ((err == k_ra8_ok) && ((observed.sequence != slot->envelope.sequence) ||
                              (observed.payload_bytes != slot->envelope.payload_bytes) ||
                              (observed.payload_crc32 != slot->envelope.payload_crc32))) {
      err = k_ra8_err_invalid_state;
    }
  }
  if (err == k_ra8_ok) {
    const uint64_t offset = slot->legacy ? 0U : (uint64_t)k_state_header_bytes;
    const uint64_t length = slot->legacy ? slot->file_bytes : slot->envelope.payload_bytes;
    const uint16_t max_schema =
      slot->legacy ? (uint16_t)k_mdl_state_version_v2 : (uint16_t)k_mdl_state_version;
    err = priv_mdl_state_parse_file(storage, &file, offset, length, max_schema, st);
  }
  const ra8_err_t closed = file.is_open ? fw_fs_close(&file) : k_ra8_ok;
  if (closed != k_ra8_ok) {
    err = closed;
  }
  if (err != k_ra8_ok) {
    mdl_state_init(st);
  }
  return err;
}

/**
 * @brief Write all bytes to an active transaction with bounded retry.
 * @details @param[in,out] writer Active writer. @param[in] bytes Source. @param[in] length Byte count. @return Portable transaction status. @retval k_ra8_err_invalid_state Successful zero progress.
 * @pre Writer and source are non-NULL. @pre Transaction is active and unvalidated.
 * @post Success writes exactly @p length bytes. @post Attempts remain bounded.
 * @note Short successful writes are retried. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_mdl_state_write_all(mdl_state_writer_t* writer, const uint8_t* bytes, uint32_t length)
{
  uint32_t offset = 0U;
  while (offset < length) {
    if (writer->calls >= (uint32_t)k_state_io_call_max) {
      return k_ra8_err_invalid_size;
    }
    uint32_t        count = 0U;
    const ra8_err_t err =
      fw_fs_transaction_write(writer->transaction, bytes + offset, length - offset, &count);
    ++writer->calls;
    if (err != k_ra8_ok) {
      return err;
    }
    if (count == 0U) {
      return k_ra8_err_invalid_state;
    }
    offset += count;
  }
  return k_ra8_ok;
}

/**
 * @brief Append one payload record and update its identity.
 * @details @param[in,out] writer Active writer. @param[in] line Serialized record. @param[in] length Byte count. @return Portable size/I/O status. @retval k_ra8_err_invalid_size Record or aggregate extent overflow.
 * @pre Pointers are non-NULL. @pre @p length describes accessible bytes.
 * @post Success advances byte count and CRC together. @post Failure never reports unwritten bytes as persisted.
 * @note Record terminators are included in the checksum. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_mdl_state_emit(mdl_state_writer_t* writer, const char* line, int length)
{
  if ((length < 0) || ((size_t)length >= (size_t)k_mdl_state_line_max) ||
      (writer->payload_bytes > (UINT64_MAX - (uint64_t)(uint32_t)length))) {
    return k_ra8_err_invalid_size;
  }
  const ra8_err_t err =
    internal_mdl_state_write_all(writer, (const uint8_t*)line, (uint32_t)length);
  if (err != k_ra8_ok) {
    return err;
  }
  writer->crc_state =
    internal_mdl_state_crc32_update(writer->crc_state, (const uint8_t*)line, (uint32_t)length);
  writer->payload_bytes += (uint64_t)(uint32_t)length;
  return k_ra8_ok;
}

/**
 * @brief Serialize one bounded key/value record.
 * @details @param[in,out] writer Active writer. @param[in] type Record type. @param[in] value Validated value. @return Serialization status. @retval k_ra8_err_invalid_size Formatted record exceeds its bound.
 * @pre Writer and value are non-NULL. @pre Value contains no record delimiter.
 * @post Success appends one complete line. @post Writer identity matches emitted bytes.
 * @note Formatting uses fixed stack storage. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_mdl_state_emit_kv(mdl_state_writer_t* writer, char type, const char* value)
{
  char      line[k_mdl_state_line_max];
  const int length = snprintf(line, sizeof(line), "%c\t%s\n", type, value);
  return internal_mdl_state_emit(writer, line, length);
}

/**
 * @brief Serialize every fixed series identity and metadata record.
 * @details @param[in,out] writer Active writer. @param[in] st Valid state. @return Serialization status. @retval k_ra8_ok Every metadata record was emitted.
 * @pre Both pointers are non-NULL. @pre State passed full persistence validation.
 * @post Success emits metadata in canonical order. @post Failure stops at the first rejected write.
 * @note Reading direction is emitted as a numeric record. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_state_emit_metadata(mdl_state_writer_t* writer,
                                                               const mdl_state_t*  st)
{
  static const char types[]  = {'S', 'T', 'N', 'H', 'G', 'D', 'W', 'A', 'O', 'K', 'L'};
  const char* const values[] = {st->series_url,
                                st->series_title,
                                st->site_name,
                                st->site_host,
                                st->config_path,
                                st->summary,
                                st->writer,
                                st->artist,
                                st->cover_url,
                                st->cover_path,
                                st->language};
  ra8_err_t         err      = k_ra8_ok;
  for (size_t i = 0U; (i < sizeof(types)) && (err == k_ra8_ok); ++i) {
    err = internal_mdl_state_emit_kv(writer, types[i], values[i]);
  }
  if (err == k_ra8_ok) {
    char      line[k_mdl_state_line_max];
    const int length = snprintf(line, sizeof(line), "R\t%u\n", (unsigned)st->reading_direction);
    err              = internal_mdl_state_emit(writer, line, length);
  }
  return err;
}

/**
 * @brief Serialize every chapter with exact v3 numeric identity.
 * @details @param[in,out] writer Active writer. @param[in] st Valid state. @return Serialization status. @retval k_ra8_ok Every chapter was emitted.
 * @pre Both pointers are non-NULL. @pre Chapter values are finite and validated.
 * @post Success emits all chapters in state order. @post Binary64 bits use exact lowercase hex.
 * @note Bit extraction uses memcpy. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_state_emit_chapters(mdl_state_writer_t* writer,
                                                               const mdl_state_t*  st)
{
  char      line[k_mdl_state_line_max];
  ra8_err_t err = k_ra8_ok;
  for (uint16_t i = 0U; (i < st->chapter_count) && (err == k_ra8_ok); ++i) {
    const mdl_chapter_rec_t* chapter     = &st->chapters[i];
    uint64_t                 number_bits = 0U;
    memcpy(&number_bits, &chapter->number, sizeof(number_bits));
    const int length = snprintf(line,
                                sizeof(line),
                                "C\t%s\t%u\t%016" PRIx64 "\t%u\t%u\t%u\t%" PRId64 "\t%s\t%s\n",
                                chapter->chapter_id,
                                chapter->number_known ? 1U : 0U,
                                number_bits,
                                chapter->complete ? 1U : 0U,
                                (unsigned)chapter->page_count,
                                (unsigned)chapter->pages_done,
                                chapter->fetched_at,
                                chapter->source_url,
                                chapter->title);
    err              = internal_mdl_state_emit(writer, line, length);
  }
  return err;
}

/**
 * @brief Serialize every page identity and cache record.
 * @details @param[in,out] writer Active writer. @param[in] st Valid state. @return Serialization status. @retval k_ra8_ok Every page was emitted.
 * @pre Both pointers are non-NULL. @pre Stored paths and validators are bounded.
 * @post Success emits all pages in state order. @post Hash fields use fixed-width lowercase hex.
 * @note Cache validators may be empty. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_state_emit_pages(mdl_state_writer_t* writer,
                                                            const mdl_state_t*  st)
{
  char      line[k_mdl_state_line_max];
  ra8_err_t err = k_ra8_ok;
  for (uint32_t i = 0U; (i < st->page_rec_count) && (err == k_ra8_ok); ++i) {
    const mdl_page_rec_t* page = &st->pages[i];
    const int length = snprintf(line,
                                sizeof(line),
                                "P\t%016" PRIx64 "\t%016" PRIx64 "\t%s\t%s\t%s\t%" PRId64 "\t%u\n",
                                page->url_hash,
                                page->content_hash,
                                page->rel_path,
                                page->etag,
                                page->last_modified,
                                page->fetched_at,
                                (unsigned)page->response_status);
    err              = internal_mdl_state_emit(writer, line, length);
  }
  return err;
}

/**
 * @brief Serialize one complete current-schema payload.
 * @details @param[in,out] writer Active writer. @param[in] st Valid state. @return Serialization status. @retval k_ra8_ok Version, metadata, chapters, and pages were emitted.
 * @pre Both pointers are non-NULL. @pre State passed full persistence validation.
 * @post Success emits canonical section order. @post Writer identity covers the entire payload.
 * @note Envelope bytes are not part of payload CRC. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_state_serialize(mdl_state_writer_t* writer,
                                                           const mdl_state_t*  st)
{
  char      line[k_mdl_state_line_max];
  const int length = snprintf(line,
                              sizeof(line),
                              "# media_dl library state v%u\nV\t%u\n",
                              (unsigned)k_mdl_state_version,
                              (unsigned)k_mdl_state_version);
  ra8_err_t err    = internal_mdl_state_emit(writer, line, length);
  if (err == k_ra8_ok) {
    err = internal_mdl_state_emit_metadata(writer, st);
  }
  if (err == k_ra8_ok) {
    err = internal_mdl_state_emit_chapters(writer, st);
  }
  return (err == k_ra8_ok) ? internal_mdl_state_emit_pages(writer, st) : err;
}

/**
 * @brief Independently validate the staged journal before publication.
 * @details @param[in] ctx Expected identity context. @param[in,out] staged Open staged stream. @return Validation status. @retval k_ra8_err_protocol_error Stage differs from expected identity.
 * @pre Both pointers are non-NULL. @pre Stage is private and complete.
 * @post Success authenticates header, extent, and payload. @post No publication occurs here.
 * @note Invoked through the fw_fs transaction contract. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_state_validate_stage(void* ctx, fw_fs_file_t* staged)
{
  if ((ctx == nullptr) || (staged == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  const mdl_state_validation_t* expected = (const mdl_state_validation_t*)ctx;
  mdl_state_envelope_t          observed = {};
  const ra8_err_t err = internal_mdl_state_validate_open(expected->storage, staged, &observed);
  if (err != k_ra8_ok) {
    return err;
  }
  if ((observed.sequence != expected->envelope.sequence) ||
      (observed.payload_bytes != expected->envelope.payload_bytes) ||
      (observed.payload_crc32 != expected->envelope.payload_crc32)) {
    return k_ra8_err_protocol_error;
  }
  return k_ra8_ok;
}

/**
 * @brief Abort an unpublished transaction with cleanup-error precedence.
 * @details @param[in,out] transaction Transaction. @param[in] primary Earlier error. @return Final error. @retval primary Abort succeeded or no transaction was active.
 * @pre @p transaction is non-NULL. @pre Publication has not occurred.
 * @post Active stages are offered to abort. @post Abort failure supersedes the primary error.
 * @note This precedence exposes failed cleanup. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_state_abort(fw_fs_transaction_t* transaction,
                                                       ra8_err_t            primary)
{
  if (!transaction->active) {
    return primary;
  }
  const ra8_err_t aborted = fw_fs_transaction_abort(transaction);
  return (aborted == k_ra8_ok) ? primary : aborted;
}

/**
 * @brief Build and validate a private staged journal.
 * @details @param[in,out] storage Scratch binding. @param[in] target Absent target path. @param[in] sequence New sequence. @param[in] st Valid state. @param[out] transaction Active stage. @param[out] envelope Identity. @return Stage status. @retval k_ra8_ok Stage is independently validated.
 * @pre All pointers are non-NULL. @pre Target is absent and sequence is nonzero.
 * @post Success leaves an active validated transaction. @post Failure leaves publication false.
 * @note Header is backfilled after streaming payload identity. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_state_build_stage(mdl_storage_t*        storage,
                                                             const char*           target,
                                                             uint64_t              sequence,
                                                             const mdl_state_t*    st,
                                                             fw_fs_transaction_t*  transaction,
                                                             mdl_state_envelope_t* envelope)
{
  ra8_err_t          err    = fw_fs_transaction_begin(&storage->fs->transactions,
                                                      target,
                                                      k_fw_fs_txn_create_new,
                                                      transaction,
                                                      storage->transaction_workspace,
                                                      storage->transaction_workspace_bytes);
  mdl_state_writer_t writer = {.transaction   = transaction,
                               .payload_bytes = 0U,
                               .crc_state     = UINT32_MAX,
                               .calls         = 0U};
  if (err == k_ra8_ok) {
    const uint8_t placeholder[k_state_header_bytes] = {};
    err = internal_mdl_state_write_all(&writer, placeholder, sizeof(placeholder));
  }
  if (err == k_ra8_ok) {
    err = internal_mdl_state_serialize(&writer, st);
  }
  *envelope = (mdl_state_envelope_t){.sequence      = sequence,
                                     .payload_bytes = writer.payload_bytes,
                                     .payload_crc32 = ~writer.crc_state};
  uint8_t header[k_state_header_bytes];
  internal_mdl_state_encode_header(envelope, header);
  if (err == k_ra8_ok) {
    err = fw_fs_transaction_seek(transaction, 0U);
  }
  if (err == k_ra8_ok) {
    err = internal_mdl_state_write_all(&writer, header, sizeof(header));
  }
  const mdl_state_validation_t validation = {.storage = storage, .envelope = *envelope};
  return (err == k_ra8_ok) ? fw_fs_transaction_validate(transaction,
                                                        internal_mdl_state_validate_stage,
                                                        (void*)&validation)
                           : err;
}

/**
 * @brief Select the preserved generation, rewrite target, and next sequence.
 * @details @param[in] base Base slot. @param[in] alternate Alternate slot. @param[out] target Slot to replace. @param[out] sequence Next sequence. @return Planning status. @retval k_ra8_err_invalid_size Sequence is exhausted.
 * @pre All pointers are non-NULL. @pre Both slot scans are complete.
 * @post Success never selects the newest slot as target. @post Sequence increments without wrap.
 * @note Existing scan errors propagate exactly. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_state_save_plan(const mdl_state_slot_t*  base,
                                                           const mdl_state_slot_t*  alternate,
                                                           const mdl_state_slot_t** target,
                                                           uint64_t*                sequence)
{
  const mdl_state_slot_t* newest = nullptr;
  const mdl_state_slot_t* older  = nullptr;
  ra8_err_t               err    = internal_mdl_state_order(base, alternate, &newest, &older);
  if (err != k_ra8_ok) {
    return err;
  }
  if (newest == nullptr) {
    if ((base->error != k_ra8_ok) || (alternate->error != k_ra8_ok)) {
      return (base->error != k_ra8_ok) ? base->error : alternate->error;
    }
    *target   = base;
    *sequence = 1U;
    return k_ra8_ok;
  }
  if (newest->envelope.sequence == UINT64_MAX) {
    return k_ra8_err_invalid_size;
  }
  *target = (newest == base) ? alternate : base;
  if ((older != nullptr) && (*target != older)) {
    return k_ra8_err_invalid_state;
  }
  if (!(*target)->exists && ((*target)->error != k_ra8_ok)) {
    return (*target)->error;
  }
  *sequence = newest->envelope.sequence + 1U;
  return k_ra8_ok;
}

ra8_err_t mdl_state_probe(mdl_storage_t* storage, const char* path, bool* out_exists)
{
  if (out_exists == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  *out_exists = false;
  char      base[k_fw_fs_path_cap];
  char      alternate[k_fw_fs_path_cap];
  ra8_err_t err = internal_mdl_state_paths(storage, path, base, alternate);
  if (err != k_ra8_ok) {
    return err;
  }
  const char* paths[] = {base, alternate};
  for (size_t i = 0U; i < 2U; ++i) {
    fw_fs_stat_t stat = {};
    err               = fw_fs_stat(&storage->fs->names, paths[i], &stat);
    if (err != k_ra8_ok) {
      return err;
    }
    if (stat.exists && (stat.type != k_fw_fs_node_file)) {
      return k_ra8_err_invalid_state;
    }
    *out_exists = *out_exists || stat.exists;
  }
  return k_ra8_ok;
}

/**
 * @brief Load the newest usable state with an explicit legacy policy.
 * @details Authenticates journal generations, optionally admits legacy base
 *          text, and retries the older valid generation after a decode error.
 * @param[in,out] storage Initialized exclusive storage binding.
 * @param[in] path Canonical logical state path.
 * @param[out] st State initialized on every return.
 * @param[in] allow_legacy Whether unenveloped base text may be selected.
 * @return Canonical path, stream, authentication, or decode status.
 * @pre Required pointers are non-NULL and storage workspaces are exclusively owned.
 * @post @p st is valid on every return and empty when loading fails.
 * @note The alternate peer is always required to carry an authenticated envelope.
 * @since 0.1.0

 * @retval k_ra8_ok The operation completed successfully.
 * @retval other The originating validation, storage, stream, or network error.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @post Documented outputs and the return value describe the same outcome.
 */
RA8_INTERNAL static ra8_err_t internal_mdl_state_load_mode(mdl_storage_t* storage,
                                                           const char*    path,
                                                           mdl_state_t*   st,
                                                           bool           allow_legacy)
{
  if (st == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  mdl_state_init(st);
  char      base_path[k_fw_fs_path_cap];
  char      alternate_path[k_fw_fs_path_cap];
  ra8_err_t err = internal_mdl_state_paths(storage, path, base_path, alternate_path);
  if (err != k_ra8_ok) {
    return err;
  }
  mdl_state_slot_t base;
  mdl_state_slot_t alternate;
  internal_mdl_state_scan_slot(storage, base_path, allow_legacy, &base);
  internal_mdl_state_scan_slot(storage, alternate_path, false, &alternate);
  const mdl_state_slot_t* newest = nullptr;
  const mdl_state_slot_t* older  = nullptr;
  err                            = internal_mdl_state_order(&base, &alternate, &newest, &older);
  if (err != k_ra8_ok) {
    return err;
  }
  if (newest == nullptr) {
    if (!base.exists && !alternate.exists && (base.error == k_ra8_ok) &&
        (alternate.error == k_ra8_ok)) {
      return k_ra8_ok;
    }
    return (base.error != k_ra8_ok) ? base.error : alternate.error;
  }
  err = internal_mdl_state_load_slot(storage, newest, st);
  if ((err != k_ra8_ok) && (older != nullptr)) {
    err = internal_mdl_state_load_slot(storage, older, st);
  }
  return err;
}

ra8_err_t mdl_state_load(mdl_storage_t* storage, const char* path, mdl_state_t* st)
{
  return internal_mdl_state_load_mode(storage, path, st, true);
}

ra8_err_t mdl_state_load_authenticated(mdl_storage_t* storage, const char* path, mdl_state_t* st)
{
  return internal_mdl_state_load_mode(storage, path, st, false);
}

ra8_err_t
mdl_state_save(mdl_storage_t* storage, const char* path, const mdl_state_t* st, bool* out_published)
{
  if (out_published == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  *out_published = false;
  if ((st == nullptr) || !priv_mdl_state_valid(st)) {
    return (st == nullptr) ? k_ra8_err_invalid_arg : k_ra8_err_invalid_state;
  }
  char      base_path[k_fw_fs_path_cap];
  char      alternate_path[k_fw_fs_path_cap];
  ra8_err_t err = internal_mdl_state_paths(storage, path, base_path, alternate_path);
  if (err != k_ra8_ok) {
    return err;
  }
  mdl_state_slot_t base;
  mdl_state_slot_t alternate;
  internal_mdl_state_scan_slot(storage, base_path, true, &base);
  internal_mdl_state_scan_slot(storage, alternate_path, false, &alternate);
  const mdl_state_slot_t* target   = nullptr;
  uint64_t                sequence = 0U;
  err = internal_mdl_state_save_plan(&base, &alternate, &target, &sequence);
  if (err != k_ra8_ok) {
    return err;
  }
  if (target->exists) {
    err = fw_fs_unlink(&storage->fs->names, target->path);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  fw_fs_transaction_t  transaction = {};
  mdl_state_envelope_t envelope    = {};
  err =
    internal_mdl_state_build_stage(storage, target->path, sequence, st, &transaction, &envelope);
  if (err != k_ra8_ok) {
    return internal_mdl_state_abort(&transaction, err);
  }
  bool published = false;
  err            = fw_fs_transaction_commit(&transaction, &published);
  *out_published = published;
  if (published) {
    return err;
  }
  if (err == k_ra8_ok) {
    err = k_ra8_err_invalid_state;
  }
  return internal_mdl_state_abort(&transaction, err);
}
