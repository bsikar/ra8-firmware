/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_rar.c
 * @brief Implementation of the clean-room RAR4/RAR5 archive walker (ra8_rar.h).
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * Two block grammars behind one iterator. ::ra8_rar_open matches the signature and
 * records the first block offset; ::ra8_rar_next reads one block header into a
 * fixed stack scratch and decodes it under either the RAR4 fixed-field layout or
 * the RAR5 variable-length-integer (vint) layout, always producing the offset of
 * the next block so the caller walks the archive with a bounded loop.
 * ::ra8_rar_extract_stored streams a STORE member's literal bytes out.
 *
 * The RAR5 vint is little-endian base-128: seven data bits per byte, high bit set
 * means "continue". A file member's data area begins immediately after its whole
 * header (RAR5: `off + <crc+headersize-vint bytes> + headersize`; RAR4:
 * `off + head_size`), which is how a STORE member's bytes are located without a
 * decompressor.
 *
 * Every parser guards each read against the bytes actually available, so a
 * truncated block is rejected (::k_ra8_err_validation_failed) rather than
 * over-read.
 *
 * @since Version 0.1.0
 */
#include "ra8_rar.h"

#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_rar5.h"

/** @brief Log tag for RAR-walker diagnostics. */
static const char* const s_tag_rar = "ra8_rar";

/**
 * @enum rar_vint_bits_t
 * @brief Bit layout of one RAR5 variable-length integer byte.
 * @details Low 7 bits carry data; the high bit is the continuation flag.
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_rar_vint_data = 0x7FU, /**< Data bits mask within one vint byte.    */
  k_rar_vint_cont = 0x80U, /**< Continuation flag within one vint byte. */
  k_rar_vint_bits = 7U,    /**< Data bits contributed per vint byte.    */
} rar_vint_bits_t;

/**
 * @enum rar4_layout_t
 * @brief RAR4 block/file-header field byte offsets (from the block start).
 * @details The classic RAR4 header is fixed-layout; each member names the offset
 *          of a field the parser reads with a little-endian decode.
 * @since Version 0.1.0
 */
typedef enum : uint16_t {
  k_rar4_off_type       = 2U,  /**< HEAD_TYPE byte.                     */
  k_rar4_off_flags      = 3U,  /**< HEAD_FLAGS uint16.                  */
  k_rar4_off_size       = 5U,  /**< HEAD_SIZE uint16.                   */
  k_rar4_off_add        = 7U,  /**< ADD_SIZE / PACK_SIZE uint32.        */
  k_rar4_base_len       = 7U,  /**< Base header length before ADD_SIZE. */
  k_rar4_off_unp        = 11U, /**< UNP_SIZE uint32.                    */
  k_rar4_off_method     = 25U, /**< METHOD byte (0x30 = store).         */
  k_rar4_off_namesz     = 26U, /**< NAME_SIZE uint16.                   */
  k_rar4_off_attr       = 28U, /**< ATTR uint32 (fixed fields end +4).  */
  k_rar4_off_name       = 32U, /**< FILE_NAME start (no LARGE flag).    */
  k_rar4_off_name_large = 40U, /**< FILE_NAME start (LARGE 64-bit).     */
} rar4_layout_t;

/**
 * @enum rar4_bits_t
 * @brief RAR4 header flag bits, block type, and method sentinel.
 * @details Flags select optional fields; the directory test is a masked equality.
 * @since Version 0.1.0
 */
typedef enum : uint16_t {
  k_rar4_flag_long   = 0x8000U, /**< ADD_SIZE present after the base header.  */
  k_rar4_flag_large  = 0x0100U, /**< 64-bit HIGH_PACK/HIGH_UNP sizes present. */
  k_rar4_flag_dirmsk = 0x00E0U, /**< (flags & this) == this -> directory.     */
  k_rar4_type_file   = 0x0074U, /**< File-header block type ('t').            */
  k_rar4_method_stor = 0x0030U, /**< METHOD value for STORE ('0').            */
} rar4_bits_t;

/**
 * @enum rar5_bits_t
 * @brief RAR5 header/file flag bits, method field placement, and type.
 * @details Header flags gate the extra/data-size vints; file flags gate the
 *          fixed mtime/CRC fields; the method occupies bits 7-9 of comp-info.
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_rar5_type_file    = 2U,    /**< File-header block type.               */
  k_rar5_hflag_extra  = 0x01U, /**< Extra-area-size vint present.         */
  k_rar5_hflag_data   = 0x02U, /**< Data-size vint present.               */
  k_rar5_fflag_dir    = 0x01U, /**< Member is a directory.                */
  k_rar5_fflag_mtime  = 0x02U, /**< 4-byte mtime field present.           */
  k_rar5_fflag_crc    = 0x04U, /**< 4-byte data-CRC field present.        */
  k_rar5_method_shift = 7U,    /**< Method occupies comp-info bits 7..9.  */
  k_rar5_method_mask  = 0x07U, /**< 3-bit method mask after the shift.    */
  k_rar5_fixed_field  = 4U,    /**< Fixed mtime / CRC field width, bytes. */
} rar5_bits_t;

/**
 * @enum rar_word_t
 * @brief Byte widths and the 32-bit high-dword shift used by the decoders.
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_rar_w16   = 2U,  /**< uint16 width.                      */
  k_rar_w32   = 4U,  /**< uint32 width.                      */
  k_rar_hi_sh = 32U, /**< High-dword shift for 64-bit sizes. */
} rar_word_t;

/**
 * @brief Decode a little-endian uint16 from two unaligned bytes.
 * @details memcpy-based so the source may sit at any alignment; the whole
 *          firmware is little-endian, so the bytes map straight to the value.
 * @param[in] p Pointer to the two bytes (non-NULL, caller-bounded).
 * @return The decoded value.
 * @retval 0 When both bytes are zero.
 * @pre @p p addresses at least two readable bytes.
 * @pre The host is little-endian (target and test host both are).
 * @post No state is modified (pure read).
 * @post The result is a pure function of the two bytes.
 * @note Thread-safe: pure read.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static uint16_t s_le16(const uint8_t* p)
{
  uint16_t v = 0U;
  (void)memcpy(&v, p, sizeof(v));
  return v;
}

/**
 * @brief Decode a little-endian uint32 from four unaligned bytes.
 * @details memcpy-based so the source may sit at any alignment; the whole
 *          firmware is little-endian, so the bytes map straight to the value.
 * @param[in] p Pointer to the four bytes (non-NULL, caller-bounded).
 * @return The decoded value.
 * @retval 0 When all four bytes are zero.
 * @pre @p p addresses at least four readable bytes.
 * @pre The host is little-endian (target and test host both are).
 * @post No state is modified (pure read).
 * @post The result is a pure function of the four bytes.
 * @note Thread-safe: pure read.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static uint32_t s_le32(const uint8_t* p)
{
  uint32_t v = 0U;
  (void)memcpy(&v, p, sizeof(v));
  return v;
}

/**
 * @brief Decode one RAR5 vint, advancing @p pos past its bytes.
 * @details Accumulates 7 data bits per byte, little-endian, stopping at the first
 *          byte whose continuation flag is clear. Fails on truncation or a value
 *          longer than ::k_ra8_rar_vint_max bytes. The value is built in two 32-bit
 *          halves and assembled by memcpy rather than with a 64-bit variable shift:
 *          on Cortex-M a `uint64_t << reg` lowers to a libgcc `__ashldi3` call, so
 *          32-bit shifts keep this leaner (and side-step the emulator's 64-bit
 *          shift seam under ra8_emulator).
 * @param[in]     buf Header scratch bytes.
 * @param[in]     len Valid bytes in @p buf.
 * @param[in,out] pos Cursor into @p buf; advanced past the vint on success.
 * @param[out]    val Receives the decoded value.
 * @return Whether a complete vint was decoded.
 * @retval true  A terminated vint fit within @p len and the byte budget.
 * @retval false Truncated (ran off @p len) or overlong.
 * @pre @p pos <= @p len on entry.
 * @pre @p val is writable.
 * @post On true, `*pos` advanced by 1..k_ra8_rar_vint_max and `*val` set.
 * @post On false, `*val` is left indeterminate and must not be used.
 * @note Thread-safe: reads only its arguments.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static bool s_vint(const uint8_t* buf, size_t len, size_t* pos, uint64_t* val)
{
  uint32_t       words[2] = {0U, 0U}; /* [0] = low 32 bits, [1] = high 32 bits */
  uint32_t       shift    = 0U;
  const uint32_t hi_bit   = (uint32_t)k_rar_hi_sh;                             /* 32         */
  const uint32_t straddle = (uint32_t)k_rar_hi_sh - (uint32_t)k_rar_vint_bits; /* 25         */
  for (uint16_t i = 0U; i < (uint16_t)k_ra8_rar_vint_max; ++i) {               /* <=10 bytes */
    if (*pos >= len) {
      return false;
    }
    const uint8_t b = buf[*pos];
    *pos += 1U;
    const uint32_t d = (uint32_t)(b & (uint8_t)k_rar_vint_data);
    if (shift < hi_bit) {
      words[0] |= (uint32_t)(d << shift);
      if (shift > straddle) { /* this 7-bit group straddles the 32-bit boundary */
        words[1] |= (uint32_t)(d >> (hi_bit - shift));
      }
    } else {
      words[1] |= (uint32_t)(d << (shift - hi_bit));
    }
    if ((b & (uint8_t)k_rar_vint_cont) == 0U) {
      (void)memcpy(val, words, sizeof(*val)); /* LE host + target: word[0] low */
      return true;
    }
    shift += (uint32_t)k_rar_vint_bits;
  }
  return false;
}

/**
 * @brief Copy a member name into the caller buffer, from scratch or a direct read.
 * @details Uses the already-read header scratch when the name lies within it,
 *          otherwise issues one targeted read of the name bytes. The copy is
 *          clamped to @p name_cap; the returned length is what was copied.
 * @param[in]  rar      Bound archive (for the fallback read).
 * @param[in]  name_abs Absolute offset of the name in the archive.
 * @param[in]  scratch  Header scratch bytes.
 * @param[in]  n        Valid bytes in @p scratch.
 * @param[in]  in_pos   Name offset within @p scratch.
 * @param[in]  name_len Declared name length in bytes.
 * @param[out] name_buf Destination (may be NULL if @p name_cap is 0).
 * @param[in]  name_cap Destination capacity in bytes.
 * @return Bytes actually copied (<= @p name_cap).
 * @retval 0 @p name_buf is NULL, @p name_cap is 0, or a fallback read returned 0.
 * @pre @p scratch holds @p n readable bytes.
 * @pre @p name_buf holds @p name_cap writable bytes (when @p name_cap > 0).
 * @post At most @p name_cap bytes are written to @p name_buf.
 * @post No archive state is modified.
 * @note Not thread-safe (may call the archive reader).
 * @since Version 0.1.0
 */
RA8_INTERNAL
static uint16_t s_copy_name(const ra8_rar_t* rar,
                            uint64_t         name_abs,
                            const uint8_t*   scratch,
                            size_t           n,
                            size_t           in_pos,
                            uint64_t         name_len,
                            char*            name_buf,
                            uint16_t         name_cap)
{
  if (name_buf == nullptr) {
    return 0U;
  }
  if (name_cap == 0U) {
    return 0U;
  }
  const uint16_t want = (name_len > (uint64_t)name_cap) ? name_cap : (uint16_t)name_len;
  if ((in_pos + (size_t)want) <= n) {
    (void)memcpy(name_buf, &scratch[in_pos], (size_t)want);
    return want;
  }
  const size_t got = rar->read(rar->ctx, name_abs, name_buf, (size_t)want);
  return (uint16_t)got;
}

/**
 * @brief Decode the RAR4 file-specific fields and fill @p out.
 * @details Reads PACK/UNP sizes (with the optional 64-bit high dwords), the
 *          method, and the name; computes the data-area and next-block offsets
 *          from `head_size`. Non-fatal on a short scratch: returns false so the
 *          caller reports a malformed block.
 * @param[in]  rar     Bound archive.
 * @param[in]  off     Block offset.
 * @param[in]  scratch Header scratch bytes.
 * @param[in]  n       Valid bytes in @p scratch.
 * @param[in]  flags   HEAD_FLAGS of this block.
 * @param[in]  hsize   HEAD_SIZE of this block.
 * @param[out] nbuf    Name buffer.
 * @param[in]  ncap    Name buffer capacity.
 * @param[out] out     Entry to fill (file fields, data_off, next_off).
 * @return Whether the file fields fit @p n and were decoded.
 * @retval true  Fields decoded; @p out is a valid file entry.
 * @retval false Truncated fixed fields.
 * @pre @p scratch holds @p n bytes; @p hsize is this block's header size.
 * @pre @p out is writable.
 * @post On true, `out->next_off = off + hsize + pack_size` and `is_file == 1`.
 * @post On false @p out is not relied upon by the caller.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static bool s_rar4_file(const ra8_rar_t* rar,
                        uint64_t         off,
                        const uint8_t*   scratch,
                        size_t           n,
                        uint16_t         flags,
                        uint16_t         hsize,
                        char*            nbuf,
                        uint16_t         ncap,
                        ra8_rar_entry_t* out)
{
  const bool   large   = (flags & (uint16_t)k_rar4_flag_large) != 0U;
  const size_t needfix = large ? (size_t)k_rar4_off_name_large : (size_t)k_rar4_off_name;
  if (n < needfix) {
    return false;
  }
  uint64_t       pack   = (uint64_t)s_le32(&scratch[k_rar4_off_add]);
  uint64_t       unp    = (uint64_t)s_le32(&scratch[k_rar4_off_unp]);
  const uint8_t  method = scratch[k_rar4_off_method];
  const uint16_t namesz = s_le16(&scratch[k_rar4_off_namesz]);
  size_t         pos    = (size_t)k_rar4_off_name;
  if (large) {
    pack |= (uint64_t)s_le32(&scratch[k_rar4_off_name]) << (uint32_t)k_rar_hi_sh;
    unp |= (uint64_t)s_le32(&scratch[(size_t)k_rar4_off_name + (size_t)k_rar_w32])
           << (uint32_t)k_rar_hi_sh;
    pos = (size_t)k_rar4_off_name_large;
  }
  out->is_file = 1U;
  out->is_dir  = ((flags & (uint16_t)k_rar4_flag_dirmsk) == (uint16_t)k_rar4_flag_dirmsk) ? 1U : 0U;
  out->method  = (method == (uint8_t)k_rar4_method_stor) ? (uint8_t)k_ra8_rar_method_store
                                                         : (uint8_t)k_ra8_rar_method_compressed;
  out->pack_size = pack;
  out->unp_size  = unp;
  out->data_off  = off + (uint64_t)hsize;
  out->next_off  = off + (uint64_t)hsize + pack;
  out->name_len =
    s_copy_name(rar, off + (uint64_t)pos, scratch, n, pos, (uint64_t)namesz, nbuf, ncap);
  return true;
}

/**
 * @brief Walk one RAR4 block at @p off.
 * @details Reads the fixed 7-byte base header (plus ADD_SIZE when the LONG flag is
 *          set), then dispatches file blocks to ::s_rar4_file and treats every
 *          other block type as a skip (data-less unless LONG). Guarantees a
 *          strictly advancing `next_off`.
 * @param[in]  rar  Bound archive.
 * @param[in]  off  Block offset (`< rar->size`).
 * @param[out] nbuf Name buffer.
 * @param[in]  ncap Name buffer capacity.
 * @param[out] out  Entry to fill.
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    Block decoded.
 * @retval k_ra8_err_validation_failed Truncated / non-advancing block.
 * @pre @p rar is bound; @p off < rar->size.
 * @pre @p out is writable.
 * @post On k_ra8_ok, `out->next_off > off`.
 * @post On error @p out is left as the caller zeroed it.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
s_rar4_block(const ra8_rar_t* rar, uint64_t off, char* nbuf, uint16_t ncap, ra8_rar_entry_t* out)
{
  uint8_t      scratch[k_ra8_rar_hdr_scratch] = {};
  const size_t n                              = rar->read(rar->ctx, off, scratch, sizeof(scratch));
  if (n < (size_t)k_rar4_base_len) {
    return k_ra8_err_validation_failed;
  }
  const uint8_t  type  = scratch[k_rar4_off_type];
  const uint16_t flags = s_le16(&scratch[k_rar4_off_flags]);
  const uint16_t hsize = s_le16(&scratch[k_rar4_off_size]);
  if (hsize < (uint16_t)k_rar4_base_len) {
    return k_ra8_err_validation_failed;
  }
  const bool islong = (flags & (uint16_t)k_rar4_flag_long) != 0U;
  uint64_t   add    = 0U;
  if (islong) {
    if (n >= ((size_t)k_rar4_off_add + (size_t)k_rar_w32)) {
      add = (uint64_t)s_le32(&scratch[k_rar4_off_add]);
    }
  }
  if (type == (uint8_t)k_rar4_type_file) {
    if (!s_rar4_file(rar, off, scratch, n, flags, hsize, nbuf, ncap, out)) {
      return k_ra8_err_validation_failed;
    }
  } else {
    out->next_off = off + (uint64_t)hsize + add;
  }
  if (out->next_off <= off) {
    return k_ra8_err_validation_failed;
  }
  return k_ra8_ok;
}

/**
 * @brief Decode the RAR5 file-header vint stream, advancing @p pos past it.
 * @details Reads the file flags, unpacked size, and attributes; skips the
 *          optional fixed mtime / data-CRC fields the file flags gate; then reads
 *          the compression-info, host-OS, and name-size vints. The attributes and
 *          host-OS vints are decoded only to advance @p pos and are then discarded.
 *          A truncated stream (any vint running off @p n) fails without over-read.
 * @param[in]     scratch Header scratch bytes.
 * @param[in]     n       Valid bytes in @p scratch.
 * @param[in,out] pos     Cursor into @p scratch; advanced to the name bytes.
 * @param[out]    fflags  Receives the file flags.
 * @param[out]    unp     Receives the unpacked size.
 * @param[out]    cinfo   Receives the compression-info word (carries the method).
 * @param[out]    namesz  Receives the declared name length in bytes.
 * @return Whether every field decoded within @p n.
 * @retval true  All vints decoded; `*pos` points at the name bytes.
 * @retval false A vint ran off @p n (truncated header).
 * @pre @p scratch holds @p n readable bytes; `*pos <= n` on entry.
 * @pre @p fflags, @p unp, @p cinfo, and @p namesz are writable.
 * @post On true, `*pos` addresses the name and the four outputs are set.
 * @post On false the outputs are indeterminate and must not be used.
 * @note Not thread-safe; reads only its arguments.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static bool s_rar5_file_vints(const uint8_t* scratch,
                              size_t         n,
                              size_t*        pos,
                              uint64_t*      fflags,
                              uint64_t*      unp,
                              uint64_t*      cinfo,
                              uint64_t*      namesz)
{
  uint64_t attr   = 0U;
  uint64_t hostos = 0U;
  if (!s_vint(scratch, n, pos, fflags)) {
    return false;
  }
  if (!s_vint(scratch, n, pos, unp)) {
    return false;
  }
  if (!s_vint(scratch, n, pos, &attr)) {
    return false;
  }
  if ((*fflags & (uint64_t)k_rar5_fflag_mtime) != 0U) {
    *pos += (size_t)k_rar5_fixed_field;
  }
  if ((*fflags & (uint64_t)k_rar5_fflag_crc) != 0U) {
    *pos += (size_t)k_rar5_fixed_field;
  }
  if (!s_vint(scratch, n, pos, cinfo)) {
    return false;
  }
  if (!s_vint(scratch, n, pos, &hostos)) {
    return false;
  }
  if (!s_vint(scratch, n, pos, namesz)) {
    return false;
  }
  (void)attr;   /* decoded to advance the vint stream; value unused here */
  (void)hostos; /* decoded to advance the vint stream; value unused here */
  return true;
}

/**
 * @brief Decode the RAR5 file-specific fields after the common header vints.
 * @details From @p pos (just past header type/flags/extra/data-size), reads the
 *          file flags, unpacked size, attributes, optional fixed time/CRC, the
 *          compression-info method, host OS, and name; fills the file fields of
 *          @p out. @p data_off / @p dsize / @p next_off were computed by the
 *          caller from the header-size vint.
 * @param[in]  rar      Bound archive.
 * @param[in]  off      Block offset.
 * @param[in]  scratch  Header scratch bytes.
 * @param[in]  n        Valid bytes in @p scratch.
 * @param[in]  pos      Cursor at the first file field.
 * @param[in]  data_off Absolute data-area offset for this block.
 * @param[in]  dsize    Data-area length (packed size).
 * @param[out] nbuf     Name buffer.
 * @param[in]  ncap     Name buffer capacity.
 * @param[out] out      Entry to fill.
 * @return Whether the fields decoded within @p n.
 * @retval true  Fields decoded; @p out is a valid file entry.
 * @retval false Truncated fields.
 * @pre @p scratch holds @p n bytes; @p pos <= @p n.
 * @pre @p out already has `next_off` set by the caller.
 * @post On true, `out->is_file == 1` and file fields are populated.
 * @post On false @p out is not relied upon by the caller.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static bool s_rar5_file(const ra8_rar_t* rar,
                        uint64_t         off,
                        const uint8_t*   scratch,
                        size_t           n,
                        size_t           pos,
                        uint64_t         data_off,
                        uint64_t         dsize,
                        char*            nbuf,
                        uint16_t         ncap,
                        ra8_rar_entry_t* out)
{
  uint64_t fflags = 0U;
  uint64_t unp    = 0U;
  uint64_t cinfo  = 0U;
  uint64_t namesz = 0U;
  if (!s_rar5_file_vints(scratch, n, &pos, &fflags, &unp, &cinfo, &namesz)) {
    return false;
  }
  const uint8_t method =
    (uint8_t)((cinfo >> (uint64_t)k_rar5_method_shift) & (uint64_t)k_rar5_method_mask);
  out->is_file = 1U;
  out->is_dir  = ((fflags & (uint64_t)k_rar5_fflag_dir) != 0U) ? 1U : 0U;
  out->method =
    (method == 0U) ? (uint8_t)k_ra8_rar_method_store : (uint8_t)k_ra8_rar_method_compressed;
  out->unp_size  = unp;
  out->pack_size = dsize;
  out->data_off  = data_off;
  out->name_len  = s_copy_name(rar, off + (uint64_t)pos, scratch, n, pos, namesz, nbuf, ncap);
  return true;
}

/**
 * @brief Walk one RAR5 block at @p off.
 * @details Decodes CRC + header-size vint (which fixes the data-area and
 *          next-block offsets), then the header type/flags and the optional
 *          extra-area-size / data-size vints; file blocks go to ::s_rar5_file, all
 *          others are skipped with a valid `next_off`.
 * @param[in]  rar  Bound archive.
 * @param[in]  off  Block offset (`< rar->size`).
 * @param[out] nbuf Name buffer.
 * @param[in]  ncap Name buffer capacity.
 * @param[out] out  Entry to fill.
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    Block decoded.
 * @retval k_ra8_err_validation_failed Truncated / non-advancing block.
 * @pre @p rar is bound; @p off < rar->size.
 * @pre @p out is writable.
 * @post On k_ra8_ok, `out->next_off > off`.
 * @post On error @p out is left as the caller zeroed it.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
s_rar5_block(const ra8_rar_t* rar, uint64_t off, char* nbuf, uint16_t ncap, ra8_rar_entry_t* out)
{
  uint8_t      scratch[k_ra8_rar_hdr_scratch] = {};
  const size_t n                              = rar->read(rar->ctx, off, scratch, sizeof(scratch));
  size_t       pos   = (size_t)k_rar_w32; /* skip the 4-byte header CRC32 */
  uint64_t     hsize = 0U;
  if (!s_vint(scratch, n, &pos, &hsize)) {
    return k_ra8_err_validation_failed;
  }
  const uint64_t data_off = off + (uint64_t)pos + hsize; /* header data ends here */
  uint64_t       htype    = 0U;
  uint64_t       hflags   = 0U;
  if (!s_vint(scratch, n, &pos, &htype)) {
    return k_ra8_err_validation_failed;
  }
  if (!s_vint(scratch, n, &pos, &hflags)) {
    return k_ra8_err_validation_failed;
  }
  uint64_t extra = 0U;
  if ((hflags & (uint64_t)k_rar5_hflag_extra) != 0U) {
    if (!s_vint(scratch, n, &pos, &extra)) {
      return k_ra8_err_validation_failed;
    }
  }
  (void)extra; /* extra-area length is skipped via the header-size vint */
  uint64_t dsize = 0U;
  if ((hflags & (uint64_t)k_rar5_hflag_data) != 0U) {
    if (!s_vint(scratch, n, &pos, &dsize)) {
      return k_ra8_err_validation_failed;
    }
  }
  out->next_off = data_off + dsize;
  if (htype == (uint64_t)k_rar5_type_file) {
    if (!s_rar5_file(rar, off, scratch, n, pos, data_off, dsize, nbuf, ncap, out)) {
      return k_ra8_err_validation_failed;
    }
  }
  if (out->next_off <= off) {
    return k_ra8_err_validation_failed;
  }
  return k_ra8_ok;
}

/**
 * @brief Identify the RAR generation from the leading signature bytes.
 * @details Tests the RAR5 8-byte signature first, then the RAR4 7-byte marker.
 *          RAR4 marker "Rar!\x1A\x07\x00" and RAR5 signature "Rar!\x1A\x07\x01\x00"
 *          share their first six bytes and diverge at byte 6, so each needs its own
 *          constant (a RAR4 match against the RAR5 bytes would fail at that byte).
 *          The signatures are written as string literals -- the escapes carry the
 *          control bytes and the implicit terminating NUL supplies each marker's
 *          final 0x00 -- so the reader holds no raw-byte magic constant.
 * @param[in]     hdr Leading header bytes read from the archive.
 * @param[in]     got Valid bytes in @p hdr (>= ::k_ra8_rar_sig4_len).
 * @param[in,out] rar Archive whose `version` / `first_off` are set on a match.
 * @return Nothing; the outcome is reported through @p rar.
 * @pre @p hdr holds @p got readable bytes; `got >= k_ra8_rar_sig4_len`.
 * @pre @p rar was zeroed (`version == k_ra8_rar_ver_none`) on entry.
 * @post On a match `rar->version` is RAR4/RAR5 and `rar->first_off` is its length.
 * @post On no match @p rar is unchanged (`version == k_ra8_rar_ver_none`).
 * @note Not thread-safe; pure byte compare.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void s_rar_match_signature(const uint8_t* hdr, size_t got, ra8_rar_t* rar)
{
  static const char k_sig5[] = "Rar!\x1A\x07\x01"; /* + implicit NUL = 8-byte RAR5 sig    */
  static const char k_sig4[] = "Rar!\x1A\x07";     /* + implicit NUL = 7-byte RAR4 marker */
  static_assert(sizeof(k_sig5) == (size_t)k_ra8_rar_sig5_len, "RAR5 signature length");
  static_assert(sizeof(k_sig4) == (size_t)k_ra8_rar_sig4_len, "RAR4 marker length");
  if (got >= (size_t)k_ra8_rar_sig5_len) {
    if (memcmp(hdr, k_sig5, (size_t)k_ra8_rar_sig5_len) == 0) {
      rar->version   = k_ra8_rar_ver_5;
      rar->first_off = (uint64_t)k_ra8_rar_sig5_len;
      return;
    }
  }
  if (memcmp(hdr, k_sig4, (size_t)k_ra8_rar_sig4_len) == 0) {
    rar->version   = k_ra8_rar_ver_4;
    rar->first_off = (uint64_t)k_ra8_rar_sig4_len;
  }
}

ra8_err_t ra8_rar_open(ra8_rar_t* rar, ra8_rar_read_fn read, void* ctx, uint64_t size)
{
  RA8_CHECK_NULL_PTR(rar, s_tag_rar, "open: null rar");
  RA8_CHECK_NULL_PTR((const void*)read, s_tag_rar, "open: null read");
  *rar = (ra8_rar_t){};
  if (size < (uint64_t)k_ra8_rar_sig4_len) {
    return k_ra8_err_invalid_size;
  }
  uint8_t      hdr[k_ra8_rar_sig5_len] = {};
  const size_t got                     = read(ctx, 0U, hdr, sizeof(hdr));
  if (got < (size_t)k_ra8_rar_sig4_len) {
    return k_ra8_err_invalid_size;
  }
  s_rar_match_signature(hdr, got, rar);
  if (rar->version == k_ra8_rar_ver_none) {
    return k_ra8_err_not_supported;
  }
  rar->read = read;
  rar->ctx  = ctx;
  rar->size = size;
  return k_ra8_ok;
}

ra8_err_t ra8_rar_next(const ra8_rar_t* rar,
                       uint64_t         off,
                       char*            name_buf,
                       uint16_t         name_cap,
                       ra8_rar_entry_t* out)
{
  RA8_CHECK_NULL_PTR(rar, s_tag_rar, "next: null rar");
  RA8_CHECK_NULL_PTR(out, s_tag_rar, "next: null out");
  *out = (ra8_rar_entry_t){};
  if (rar->version == k_ra8_rar_ver_none) {
    return k_ra8_err_invalid_state;
  }
  if (off >= rar->size) {
    return k_ra8_err_invalid_arg;
  }
  if (rar->version == k_ra8_rar_ver_5) {
    return s_rar5_block(rar, off, name_buf, name_cap, out);
  }
  return s_rar4_block(rar, off, name_buf, name_cap, out);
}

/**
 * @brief Validate that @p ent is an extractable STORE member fitting @p cap.
 * @details Rejects, in order: an unbound archive, a non-file or directory entry,
 *          a compressed (non-STORE) member, a member larger than @p cap, and a
 *          member whose data area overruns the archive. Mirrors the guard order
 *          ::ra8_rar_extract_stored needs before it streams the member bytes.
 * @param[in] rar Bound archive (its `version` / `size` gate the checks).
 * @param[in] ent Candidate entry from ::ra8_rar_next.
 * @param[in] cap Destination-buffer capacity in bytes.
 * @return ra8_err_t status.
 * @retval k_ra8_ok                The entry is a STORE member that fits @p cap.
 * @retval k_ra8_err_invalid_state The archive was never opened.
 * @retval k_ra8_err_not_supported A non-file, directory, or compressed member.
 * @retval k_ra8_err_no_mem        The member is larger than @p cap.
 * @retval k_ra8_err_invalid_size  The member data area overruns the archive.
 * @pre @p rar was bound by ::ra8_rar_open.
 * @pre @p ent came from ::ra8_rar_next on @p rar.
 * @post No state is modified (pure validation).
 * @post On k_ra8_ok, `ent->data_off + ent->unp_size <= rar->size`.
 * @note Not thread-safe; pure read of its arguments.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_rar_check_stored(const ra8_rar_t* rar, const ra8_rar_entry_t* ent, size_t cap)
{
  if (rar->version == k_ra8_rar_ver_none) {
    return k_ra8_err_invalid_state;
  }
  if (ent->is_file == 0U) {
    return k_ra8_err_not_supported;
  }
  if (ent->is_dir != 0U) {
    return k_ra8_err_not_supported;
  }
  if (ent->method != (uint8_t)k_ra8_rar_method_store) {
    return k_ra8_err_not_supported;
  }
  if (ent->unp_size > (uint64_t)cap) {
    return k_ra8_err_no_mem;
  }
  if ((ent->data_off + ent->unp_size) > rar->size) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

/**
 * @brief Stream exactly @p need bytes of a STORE member into @p buf.
 * @details Issues repeated backing reads from @p data_off until @p need bytes are
 *          copied or a read returns 0 (a short archive). Bounded: @p need is fixed
 *          and every pass copies at least one byte or breaks.
 * @param[in]  rar      Bound archive (its `read` / `ctx` perform the I/O).
 * @param[in]  data_off Absolute offset of the member data area.
 * @param[in]  need     Bytes to copy (the member's unpacked size).
 * @param[out] buf      Destination (at least @p need writable bytes).
 * @return Whether all @p need bytes were copied.
 * @retval true  Exactly @p need bytes were read into @p buf.
 * @retval false A backing read returned 0 before @p need bytes (short archive).
 * @pre @p rar was bound by ::ra8_rar_open.
 * @pre @p buf holds at least @p need writable bytes.
 * @post On true, `buf[0..need)` holds the member's bytes.
 * @post On false fewer than @p need bytes were written.
 * @note Not thread-safe (drives the archive reader).
 * @since Version 0.1.0
 */
RA8_INTERNAL
static bool s_rar_read_exact(const ra8_rar_t* rar, uint64_t data_off, uint64_t need, uint8_t* buf)
{
  size_t total = 0U;
  while ((uint64_t)total < need) { /* bound: need fixed; each pass reads >=1 or breaks */
    const size_t chunk = (size_t)(need - (uint64_t)total);
    const size_t r     = rar->read(rar->ctx, data_off + (uint64_t)total, &buf[total], chunk);
    if (r == 0U) {
      break;
    }
    total += r;
  }
  return (uint64_t)total == need;
}

ra8_err_t ra8_rar_extract_stored(const ra8_rar_t*       rar,
                                 const ra8_rar_entry_t* ent,
                                 uint8_t*               buf,
                                 size_t                 cap,
                                 size_t*                got)
{
  RA8_CHECK_NULL_PTR(rar, s_tag_rar, "extract: null rar");
  RA8_CHECK_NULL_PTR(ent, s_tag_rar, "extract: null ent");
  RA8_CHECK_NULL_PTR(buf, s_tag_rar, "extract: null buf");
  RA8_CHECK_NULL_PTR(got, s_tag_rar, "extract: null got");
  *got                   = 0U;
  const ra8_err_t vcheck = s_rar_check_stored(rar, ent, cap);
  if (vcheck != k_ra8_ok) {
    return vcheck;
  }
  if (!s_rar_read_exact(rar, ent->data_off, ent->unp_size, buf)) {
    return k_ra8_err_invalid_size;
  }
  *got = (size_t)ent->unp_size;
  return k_ra8_ok;
}

/**
 * @brief Null-check the required arguments of ::ra8_rar_extract.
 * @details Runs the mandatory guards on the archive, entry, buffer, and count
 *          pointers; the optional decoder scratch is checked later only on the
 *          compressed path. Split out so the entry point stays within the
 *          function-size budget.
 * @param[in] rar Archive pointer.
 * @param[in] ent Entry pointer.
 * @param[in] buf Output buffer pointer.
 * @param[in] got Byte-count output pointer.
 * @return ra8_err_t status.
 * @retval k_ra8_ok           Every required pointer is non-NULL.
 * @retval k_ra8_err_null_ptr Some required pointer was NULL.
 * @pre The caller forwards its pointer arguments unchanged.
 * @pre No argument is dereferenced before this returns k_ra8_ok.
 * @post On k_ra8_ok each checked pointer is safe to dereference.
 * @post No state is modified.
 * @note Thread-safe: reads only its pointer arguments.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_rar_extract_reject_null(const ra8_rar_t*       rar,
                                           const ra8_rar_entry_t* ent,
                                           const uint8_t*         buf,
                                           const size_t*          got)
{
  RA8_CHECK_NULL_PTR(rar, s_tag_rar, "extract: null rar");
  RA8_CHECK_NULL_PTR(ent, s_tag_rar, "extract: null ent");
  RA8_CHECK_NULL_PTR(buf, s_tag_rar, "extract: null buf");
  RA8_CHECK_NULL_PTR(got, s_tag_rar, "extract: null got");
  return k_ra8_ok;
}

ra8_err_t ra8_rar_extract(const ra8_rar_t*       rar,
                          const ra8_rar_entry_t* ent,
                          uint8_t*               buf,
                          size_t                 cap,
                          ra8_rar5_state_t*      st,
                          size_t*                got)
{
  const ra8_err_t nchk = s_rar_extract_reject_null(rar, ent, buf, got);
  if (nchk != k_ra8_ok) {
    return nchk;
  }
  *got = 0U;
  if (rar->version == k_ra8_rar_ver_none) {
    return k_ra8_err_invalid_state;
  }
  if ((ent->is_file == 0U) || (ent->is_dir != 0U)) {
    return k_ra8_err_not_supported;
  }
  if (ent->method == (uint8_t)k_ra8_rar_method_store) {
    return ra8_rar_extract_stored(rar, ent, buf, cap, got);
  }
  if (rar->version != k_ra8_rar_ver_5) {
    return k_ra8_err_not_supported; /* RAR4 legacy codec is out of scope */
  }
  RA8_CHECK_NULL_PTR(st, s_tag_rar, "extract: null st for compressed");
  return ra8_rar5_decompress(rar, ent->data_off, ent->pack_size, buf, cap, ent->unp_size, st, got);
}
