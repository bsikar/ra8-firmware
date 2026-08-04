/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_book.c
 * @brief Implementation of the `.rabook` blob validator.
 *
 * @details
 * The only non-inline part of `ra8_book` is integrity/bounds validation. Walking
 * a validated blob is pure offset arithmetic and lives entirely in the header.
 *
 * @since Version 0.1.0
 */
#include "ra8_book.h"

#include <string.h>

#include "ra8_attributes.h"
#include "ra8_book_internal.h"
#include "ra8_check.h"

/** @brief Log tag for `ra8_book` validation diagnostics. */
static const char* const s_tag_book = "ra8_book";

/**
 * @enum ra8_book_crc_const_t
 * @brief Constants for the reflected CRC-32/ISO-HDLC used in the trailer.
 * @details Matches Python `zlib.crc32`, so a blob produced by
 *          `tools/epub_compile` verifies bit-for-bit on device.
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_book_crc_init = 0xFFFFFFFFU, /**< CRC seed and final XOR mask. */
  k_ra8_book_crc_poly = 0xEDB88320U, /**< Reflected polynomial.        */
} ra8_book_crc_const_t;

/**
 * @enum ra8_book_crc_table_size_t
 * @brief Size of the byte-indexed CRC-32 lookup table.
 * @since Version 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_book_crc_table_len = 256U, /**< One precomputed remainder per byte value. */
} ra8_book_crc_table_size_t;

/**
 * @enum ra8_book_crc_byte_t
 * @brief Byte-folding constants for the table-driven CRC-32 step.
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_book_crc_byte_mask = 0xFFU, /**< Low-byte index into the table.        */
  k_ra8_book_crc_byte_bits = 8U,    /**< Shift to fold the next input byte in. */
} ra8_book_crc_byte_t;

/**
 * @var s_ra8_book_crc_table
 * @brief Precomputed CRC-32 remainders for the reflected ::k_ra8_book_crc_poly.
 * @details Entry @c n is the bitwise CRC of the single byte @c n, so the running
 *          CRC folds one input byte per iteration with a lookup + shift instead
 *          of eight bit operations. Matches Python `zlib.crc32` exactly (check
 *          value 0xCBF43926 over "123456789"), so on-device verification stays
 *          bit-for-bit identical to the `tools/epub_compile` output.
 * @note Immutable, so it needs no run-time initialisation and is thread-safe.
 * @since Version 0.1.0
 */
static const uint32_t s_ra8_book_crc_table[k_ra8_book_crc_table_len] = {
  0x00000000U, 0x77073096U, 0xEE0E612CU, 0x990951BAU, 0x076DC419U, 0x706AF48FU, 0xE963A535U,
  0x9E6495A3U, 0x0EDB8832U, 0x79DCB8A4U, 0xE0D5E91EU, 0x97D2D988U, 0x09B64C2BU, 0x7EB17CBDU,
  0xE7B82D07U, 0x90BF1D91U, 0x1DB71064U, 0x6AB020F2U, 0xF3B97148U, 0x84BE41DEU, 0x1ADAD47DU,
  0x6DDDE4EBU, 0xF4D4B551U, 0x83D385C7U, 0x136C9856U, 0x646BA8C0U, 0xFD62F97AU, 0x8A65C9ECU,
  0x14015C4FU, 0x63066CD9U, 0xFA0F3D63U, 0x8D080DF5U, 0x3B6E20C8U, 0x4C69105EU, 0xD56041E4U,
  0xA2677172U, 0x3C03E4D1U, 0x4B04D447U, 0xD20D85FDU, 0xA50AB56BU, 0x35B5A8FAU, 0x42B2986CU,
  0xDBBBC9D6U, 0xACBCF940U, 0x32D86CE3U, 0x45DF5C75U, 0xDCD60DCFU, 0xABD13D59U, 0x26D930ACU,
  0x51DE003AU, 0xC8D75180U, 0xBFD06116U, 0x21B4F4B5U, 0x56B3C423U, 0xCFBA9599U, 0xB8BDA50FU,
  0x2802B89EU, 0x5F058808U, 0xC60CD9B2U, 0xB10BE924U, 0x2F6F7C87U, 0x58684C11U, 0xC1611DABU,
  0xB6662D3DU, 0x76DC4190U, 0x01DB7106U, 0x98D220BCU, 0xEFD5102AU, 0x71B18589U, 0x06B6B51FU,
  0x9FBFE4A5U, 0xE8B8D433U, 0x7807C9A2U, 0x0F00F934U, 0x9609A88EU, 0xE10E9818U, 0x7F6A0DBBU,
  0x086D3D2DU, 0x91646C97U, 0xE6635C01U, 0x6B6B51F4U, 0x1C6C6162U, 0x856530D8U, 0xF262004EU,
  0x6C0695EDU, 0x1B01A57BU, 0x8208F4C1U, 0xF50FC457U, 0x65B0D9C6U, 0x12B7E950U, 0x8BBEB8EAU,
  0xFCB9887CU, 0x62DD1DDFU, 0x15DA2D49U, 0x8CD37CF3U, 0xFBD44C65U, 0x4DB26158U, 0x3AB551CEU,
  0xA3BC0074U, 0xD4BB30E2U, 0x4ADFA541U, 0x3DD895D7U, 0xA4D1C46DU, 0xD3D6F4FBU, 0x4369E96AU,
  0x346ED9FCU, 0xAD678846U, 0xDA60B8D0U, 0x44042D73U, 0x33031DE5U, 0xAA0A4C5FU, 0xDD0D7CC9U,
  0x5005713CU, 0x270241AAU, 0xBE0B1010U, 0xC90C2086U, 0x5768B525U, 0x206F85B3U, 0xB966D409U,
  0xCE61E49FU, 0x5EDEF90EU, 0x29D9C998U, 0xB0D09822U, 0xC7D7A8B4U, 0x59B33D17U, 0x2EB40D81U,
  0xB7BD5C3BU, 0xC0BA6CADU, 0xEDB88320U, 0x9ABFB3B6U, 0x03B6E20CU, 0x74B1D29AU, 0xEAD54739U,
  0x9DD277AFU, 0x04DB2615U, 0x73DC1683U, 0xE3630B12U, 0x94643B84U, 0x0D6D6A3EU, 0x7A6A5AA8U,
  0xE40ECF0BU, 0x9309FF9DU, 0x0A00AE27U, 0x7D079EB1U, 0xF00F9344U, 0x8708A3D2U, 0x1E01F268U,
  0x6906C2FEU, 0xF762575DU, 0x806567CBU, 0x196C3671U, 0x6E6B06E7U, 0xFED41B76U, 0x89D32BE0U,
  0x10DA7A5AU, 0x67DD4ACCU, 0xF9B9DF6FU, 0x8EBEEFF9U, 0x17B7BE43U, 0x60B08ED5U, 0xD6D6A3E8U,
  0xA1D1937EU, 0x38D8C2C4U, 0x4FDFF252U, 0xD1BB67F1U, 0xA6BC5767U, 0x3FB506DDU, 0x48B2364BU,
  0xD80D2BDAU, 0xAF0A1B4CU, 0x36034AF6U, 0x41047A60U, 0xDF60EFC3U, 0xA867DF55U, 0x316E8EEFU,
  0x4669BE79U, 0xCB61B38CU, 0xBC66831AU, 0x256FD2A0U, 0x5268E236U, 0xCC0C7795U, 0xBB0B4703U,
  0x220216B9U, 0x5505262FU, 0xC5BA3BBEU, 0xB2BD0B28U, 0x2BB45A92U, 0x5CB36A04U, 0xC2D7FFA7U,
  0xB5D0CF31U, 0x2CD99E8BU, 0x5BDEAE1DU, 0x9B64C2B0U, 0xEC63F226U, 0x756AA39CU, 0x026D930AU,
  0x9C0906A9U, 0xEB0E363FU, 0x72076785U, 0x05005713U, 0x95BF4A82U, 0xE2B87A14U, 0x7BB12BAEU,
  0x0CB61B38U, 0x92D28E9BU, 0xE5D5BE0DU, 0x7CDCEFB7U, 0x0BDBDF21U, 0x86D3D2D4U, 0xF1D4E242U,
  0x68DDB3F8U, 0x1FDA836EU, 0x81BE16CDU, 0xF6B9265BU, 0x6FB077E1U, 0x18B74777U, 0x88085AE6U,
  0xFF0F6A70U, 0x66063BCAU, 0x11010B5CU, 0x8F659EFFU, 0xF862AE69U, 0x616BFFD3U, 0x166CCF45U,
  0xA00AE278U, 0xD70DD2EEU, 0x4E048354U, 0x3903B3C2U, 0xA7672661U, 0xD06016F7U, 0x4969474DU,
  0x3E6E77DBU, 0xAED16A4AU, 0xD9D65ADCU, 0x40DF0B66U, 0x37D83BF0U, 0xA9BCAE53U, 0xDEBB9EC5U,
  0x47B2CF7FU, 0x30B5FFE9U, 0xBDBDF21CU, 0xCABAC28AU, 0x53B39330U, 0x24B4A3A6U, 0xBAD03605U,
  0xCDD70693U, 0x54DE5729U, 0x23D967BFU, 0xB3667A2EU, 0xC4614AB8U, 0x5D681B02U, 0x2A6F2B94U,
  0xB40BBE37U, 0xC30C8EA1U, 0x5A05DF1BU, 0x2D02EF8DU,
};

/**
 * @brief Implementation of `ra8_book_crc32()` -- table-driven reflected CRC-32.
 *
 * @details
 * Computes a CRC-32/ISO-HDLC (reflected polynomial 0xEDB88320) over the byte
 * array [@p data, @p data + @p len) using the precomputed table
 * @ref s_ra8_book_crc_table. The algorithm seeds the accumulator with
 * @ref k_ra8_book_crc_init, folds one byte per iteration by XOR-indexing the
 * table and right-shifting the accumulator, then XORs the final value with
 * @ref k_ra8_book_crc_init again to produce the standard CRC-32 result. The
 * check value over "123456789" is 0xCBF43926, matching Python `zlib.crc32`.
 *
 * @param[in] data  Pointer to the byte array to checksum; must not be NULL.
 * @param[in] len   Number of bytes to process; 0 produces 0x00000000.
 *
 * @return uint32_t CRC-32 of the input byte array.
 * @retval 0x00000000  Returned when @p len is 0 (empty input).
 * @retval 0xCBF43926  Check value for the ASCII string "123456789".
 *
 * @pre @p data is not NULL when @p len is greater than 0.
 * @pre @p len does not exceed the size of the allocation pointed to by @p data.
 * @post The returned value equals the CRC-32/ISO-HDLC of the input bytes.
 * @post Neither @p data nor any external state is modified.
 *
 * @note Not thread-safe if the read range overlaps a concurrent write; the
 *       CRC table itself is immutable and requires no synchronisation.
 *
 * @since Version 0.1.0
 */
RA8_INTERNAL
RA8_NO_RECURSION
static uint32_t ra8_book_crc32(const uint8_t* data, size_t len)
{
  uint32_t crc = k_ra8_book_crc_init;
  for (size_t i = 0U; i < len; ++i) {
    const uint8_t idx = (uint8_t)((crc ^ (uint32_t)data[i]) & (uint32_t)k_ra8_book_crc_byte_mask);
    crc               = s_ra8_book_crc_table[idx] ^ (crc >> (uint32_t)k_ra8_book_crc_byte_bits);
  }
  return crc ^ k_ra8_book_crc_init;
}

/**
 * @brief Implementation of `ra8_book_table_fits()` -- overflow-safe extent check.
 *
 * @details
 * Returns true when the half-open byte range [@p off, @p off + @p count *
 * @p elem) lies entirely within a blob of @p total bytes. Both the start
 * offset and the computed end are promoted to 64-bit before comparison so that
 * no 32-bit arithmetic can wrap on adversarially crafted blob fields, even when
 * @p count and @p elem together would overflow a 32-bit product.
 *
 * @param[in] off    Byte offset of the table's first element within the blob.
 * @param[in] count  Number of elements in the table.
 * @param[in] elem   Size in bytes of one table element.
 * @param[in] total  Total byte length of the blob (value from the header).
 *
 * @return bool Whether the described table fits inside the blob.
 * @retval true   The range [@p off, @p off + @p count * @p elem) is within @p total.
 * @retval false  The range overflows or exceeds @p total bytes.
 *
 * @pre @p total reflects the actual allocation backing the blob pointer.
 * @pre @p elem is non-zero; passing zero causes the range to collapse to @p off.
 * @post No memory is read or written; the result is a pure arithmetic predicate.
 * @post Returns false for any input combination that would overflow a 32-bit sum.
 *
 * @note Pure function with no shared state; thread-safe.
 *
 * @since Version 0.1.0
 */
RA8_INTERNAL
static bool ra8_book_table_fits(uint32_t off, uint32_t count, uint32_t elem, uint32_t total)
{
  uint64_t end = (uint64_t)off + ((uint64_t)count * (uint64_t)elem);
  return (off <= total) && (end <= (uint64_t)total);
}

/**
 * @brief Whether every image descriptor declares a pixel format this build knows.
 *
 * @details
 * Rejects a blob carrying a @ref ra8_book_image_t::pixel_format newer than
 * @ref k_ra8_book_pixfmt_gray8 -- the same fail-closed stance ra8_book_validate()
 * takes on an unknown header feature bit: a depth this firmware cannot unpack must
 * be refused, not fed to the wrong blit and mis-rendered. Every pre-field blob
 * zero-filled the byte (== @ref k_ra8_book_pixfmt_gray4), so this never rejects an
 * older gray4 book. Called only after the image table's bounds are validated, so
 * ra8_book_images() is in range for all @p hdr->image_count entries.
 *
 * @param[in] base Blob base already bounds-checked by the caller (non-NULL).
 * @param[in] hdr  Header view of @p base whose image table was already validated.
 *
 * @return Whether every image's declared pixel format is known to this build.
 * @retval true  Every descriptor's `pixel_format` is <= k_ra8_book_pixfmt_gray8.
 * @retval false At least one descriptor names a depth this firmware cannot decode.
 *
 * @pre The image table [image_off, image_off + image_count * sizeof(image)) fits.
 * @pre @p base and @p hdr are non-NULL and describe the same blob.
 * @post No memory is written (pure read over the immutable image table).
 * @post The result depends only on the descriptors, not on any pool bytes.
 *
 * @note Thread-safe: read-only over immutable data.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static bool ra8_book_image_pixfmts_known(const void* base, const ra8_book_header_t* hdr)
{
  const ra8_book_image_t* imgs  = ra8_book_images(base);
  bool                    known = true;
  for (uint32_t i = 0U; i < hdr->image_count; ++i) { /* bound: validated image_count */
    if (imgs[i].pixel_format > (uint8_t)k_ra8_book_pixfmt_gray8) {
      known = false;
    }
  }
  return known;
}

/**
 * @brief Whether the header's magic field equals the "RABOOK1" tag.
 *
 * @details
 * Compares all 8 magic bytes (7 chars + NUL) against the fixed "RABOOK1"
 * signature. Factored out of ra8_book_validate() so that function stays within
 * the readability-function-size / NASA Rule 4 statement budget; the check is a
 * pure read over the caller-provided header.
 *
 * @param[in] hdr Header view of a blob whose size was already bounds-checked.
 *
 * @return Whether every magic byte matches the "RABOOK1" signature.
 * @retval true  All 8 magic bytes match.
 * @retval false At least one magic byte differs.
 *
 * @pre @p hdr is non-NULL and points at a blob of at least sizeof(header).
 * @pre The magic[] array is fully in range (guaranteed by the size precheck).
 * @post No memory is written (pure read over the header).
 * @post The result depends only on hdr->magic, not on any body bytes.
 *
 * @note Thread-safe: read-only over immutable data.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static bool ra8_book_magic_ok(const ra8_book_header_t* hdr)
{
  const char expect[8] = {'R', 'A', 'B', 'O', 'O', 'K', '1', '\0'};
  bool       ok        = true;
  for (uint8_t i = 0U; i < (uint8_t)sizeof(expect); ++i) {
    if (hdr->magic[i] != expect[i]) {
      ok = false;
    }
  }
  return ok;
}

ra8_err_t ra8_book_validate(const void* base, size_t size)
{
  RA8_CHECK_NULL_PTR(base, s_tag_book, "validate: null base");

  if (size < sizeof(ra8_book_header_t)) {
    return k_ra8_err_invalid_size;
  }

  const ra8_book_header_t* hdr = (const ra8_book_header_t*)base;

  if (!ra8_book_magic_ok(hdr)) {
    return k_ra8_err_invalid_arg;
  }
  if (hdr->format_version != k_ra8_book_format_version) {
    return k_ra8_err_invalid_arg;
  }
  /* Unknown feature bits mean the blob relies on a presentation semantic this
   * firmware does not implement (e.g. a reading-order mode newer than
   * ra8_book_flag_t) -- refuse it rather than silently mis-render it. */
  if ((hdr->flags & ~(uint32_t)k_ra8_book_flag_mask_known) != 0U) {
    return k_ra8_err_invalid_arg;
  }

  uint32_t total = hdr->total_size;
  if ((total < sizeof(ra8_book_header_t)) || ((size_t)total > size)) {
    return k_ra8_err_invalid_size;
  }

  const bool tables_ok =
    ra8_book_table_fits(hdr->chapter_off, hdr->chapter_count, sizeof(ra8_book_chapter_t), total) &&
    ra8_book_table_fits(hdr->node_off, hdr->node_count, sizeof(ra8_book_node_t), total) &&
    ra8_book_table_fits(hdr->attr_off, hdr->attr_count, sizeof(ra8_book_attr_t), total) &&
    ra8_book_table_fits(hdr->stylesheet_off,
                        hdr->stylesheet_count,
                        sizeof(ra8_book_stylesheet_t),
                        total) &&
    ra8_book_table_fits(hdr->image_off, hdr->image_count, sizeof(ra8_book_image_t), total) &&
    ra8_book_table_fits(hdr->string_off, hdr->string_size, 1U, total) &&
    ra8_book_table_fits(hdr->image_pool_off, hdr->image_pool_size, 1U, total);
  if (!tables_ok) {
    return k_ra8_err_invalid_size;
  }

  /* Every image descriptor must name a pixel depth this build can unpack; an
   * unknown depth is refused (fail-closed) rather than fed to the wrong blit. */
  if (!ra8_book_image_pixfmts_known(base, hdr)) {
    return k_ra8_err_invalid_arg;
  }

  const uint8_t* body     = (const uint8_t*)base + sizeof(ra8_book_header_t);
  uint32_t       body_len = total - (uint32_t)sizeof(ra8_book_header_t);
  if (ra8_book_crc32(body, body_len) != hdr->crc32) {
    return k_ra8_err_range_check_failed;
  }

  return k_ra8_ok;
}

/** @brief Implementation of `ra8_book_container_header_fields()` -- memcpy field decode. */
ra8_err_t ra8_book_container_header_fields(const uint8_t* hdr,
                                           uint32_t*      out_chunk_bytes,
                                           uint64_t*      out_total,
                                           uint32_t*      out_count)
{
  RA8_CHECK_NULL_PTR(hdr, s_tag_book, "hdr fields: null hdr");
  RA8_CHECK_NULL_PTR(out_chunk_bytes, s_tag_book, "hdr fields: null out");
  const char cmagic[k_ra8_book_container_magic_len] = {'R', 'B', 'K', 'C'};
  for (uint8_t i = 0U; i < k_ra8_book_container_magic_len; ++i) {
    if ((char)hdr[i] != cmagic[i]) {
      return k_ra8_err_invalid_arg;
    }
  }
  uint32_t chunk_bytes = 0U;
  uint64_t total       = 0U;
  uint32_t count       = 0U;
  uint32_t reserved    = 0U;
  memcpy(&chunk_bytes, &hdr[k_ra8_book_cont_off_chunk_bytes], sizeof(chunk_bytes));
  memcpy(&total, &hdr[k_ra8_book_cont_off_total], sizeof(total));
  memcpy(&count, &hdr[k_ra8_book_cont_off_count], sizeof(count));
  memcpy(&reserved, &hdr[k_ra8_book_cont_off_reserved], sizeof(reserved));
  if ((chunk_bytes == 0U) || (total == 0U) || (reserved != 0U)) {
    return k_ra8_err_invalid_arg;
  }
  const uint64_t want_count = (total + (uint64_t)chunk_bytes - 1U) / (uint64_t)chunk_bytes;
  if ((uint64_t)count != want_count) {
    return k_ra8_err_invalid_arg;
  }
  *out_chunk_bytes = chunk_bytes;
  *out_total       = total;
  *out_count       = count;
  return k_ra8_ok;
}

/** @brief Implementation of `ra8_book_container_table_entry()` -- unaligned-safe memcpy load. */
uint64_t ra8_book_container_table_entry(const uint8_t* table, uint32_t idx)
{
  uint64_t entry = 0U;
  memcpy(&entry, &table[(size_t)idx * k_ra8_book_container_entry_len], sizeof(entry));
  return entry;
}

/**
 * @struct ra8_book_container_view_t
 * @brief Parsed, bounds-checked view of a resident "RBKC" container file.
 * @details Populated by `s_container_view()`; every pointer aims into the
 *          caller's file buffer and every extent was verified against
 *          @c file_len, so the inflate loop can trust it without re-checking.
 * @invariant `table` covers `chunk_count + 1` entries; `payload` covers
 *            `payload_len` bytes; both lie within the original file buffer.
 * @since Version 0.1.0
 */
typedef struct {
  const uint8_t* table;       /**< First chunk-table byte (unaligned).    */
  const uint8_t* payload;     /**< First byte of the chunk zlib streams.  */
  uint64_t       total;       /**< Flat-blob inflated total in bytes.     */
  uint64_t       payload_len; /**< Concatenated stream bytes in the file. */
  uint32_t       chunk_bytes; /**< Inflated bytes per chunk (last short). */
  uint32_t       chunk_count; /**< Number of chunks.                      */
} ra8_book_container_view_t;

/**
 * @brief Parse + bounds-check a resident "RBKC" container against its file.
 *
 * @details
 * Decodes the fixed header via `ra8_book_container_header_fields()`, verifies
 * the header plus chunk table fit inside @p file_len, verifies the inflated
 * total fits @p scratch_cap, then walks the chunk table once to require
 * `offset[0] == 0`, strict monotonic growth, and
 * `offset[chunk_count] == payload_len` (the streams exactly tile the rest of
 * the file). On success @p out_view carries pointers the inflate loop can
 * trust without further checks.
 *
 * @param[in]  bytes       First byte of the container file.
 * @param[in]  file_len    Readable length of @p bytes.
 * @param[in]  scratch_cap Caller's inflate scratch capacity in bytes.
 * @param[out] out_view    Receives the validated view.
 *
 * @return ra8_err_t Status code.
 * @retval k_ra8_ok               View populated; geometry fully validated.
 * @retval k_ra8_err_invalid_arg  Bad magic / header fields / chunk-table shape.
 * @retval k_ra8_err_invalid_size File shorter than header + table, or the
 *                               inflated total exceeds @p scratch_cap.
 *
 * @pre @p bytes is non-NULL and points at @p file_len readable bytes.
 * @pre @p out_view is non-NULL and writable.
 * @post On k_ra8_ok every @p out_view extent lies inside the file buffer.
 * @post On any error @p out_view is not fully populated and must not be used.
 *
 * @note Thread-safe: reads only caller memory; no global state.
 *
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_container_view(const uint8_t*             bytes,
                                  size_t                     file_len,
                                  size_t                     scratch_cap,
                                  ra8_book_container_view_t* out_view)
{
  if (file_len < k_ra8_book_container_header_len) {
    return k_ra8_err_invalid_size;
  }
  ra8_err_t err = ra8_book_container_header_fields(bytes,
                                                   &out_view->chunk_bytes,
                                                   &out_view->total,
                                                   &out_view->chunk_count);
  if (err != k_ra8_ok) {
    return err;
  }
  const uint64_t entries     = (uint64_t)out_view->chunk_count + 1U;
  const uint64_t table_bytes = entries * k_ra8_book_container_entry_len;
  if ((uint64_t)file_len < ((uint64_t)k_ra8_book_container_header_len + table_bytes)) {
    return k_ra8_err_invalid_size;
  }
  if ((uint64_t)scratch_cap < out_view->total) {
    return k_ra8_err_invalid_size;
  }
  out_view->table       = &bytes[k_ra8_book_container_header_len];
  out_view->payload     = &out_view->table[table_bytes];
  out_view->payload_len = (uint64_t)file_len - k_ra8_book_container_header_len - table_bytes;
  uint64_t prev         = ra8_book_container_table_entry(out_view->table, 0U);
  if (prev != 0U) {
    return k_ra8_err_invalid_arg;
  }
  for (uint32_t i = 1U; i <= out_view->chunk_count; ++i) { /* bound: validated chunk_count */
    const uint64_t cur = ra8_book_container_table_entry(out_view->table, i);
    if (cur <= prev) {
      return k_ra8_err_invalid_arg;
    }
    prev = cur;
  }
  if (prev != out_view->payload_len) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Inflate every chunk of a validated container view into @p scratch.
 *
 * @details
 * Walks the chunk table in order; chunk `i`'s zlib stream occupies
 * `[payload + offset[i], payload + offset[i+1])` and must inflate to exactly
 * `min(chunk_bytes, total - i * chunk_bytes)` bytes, written at
 * `scratch + i * chunk_bytes`. A produced-length mismatch on any chunk aborts
 * (truncated / corrupt stream). On success @p scratch holds the reassembled
 * flat blob of exactly `view->total` bytes.
 *
 * @param[in]  view    Validated view from `s_container_view()`.
 * @param[in]  inflate Caller decompressor (see @ref ra8_book_inflate_fn).
 * @param[out] scratch Destination for the reassembled blob.
 *
 * @return ra8_err_t Status code.
 * @retval k_ra8_ok               Every chunk inflated to its exact span.
 * @retval k_ra8_err_invalid_size A chunk inflated to the wrong length.
 * @retval k_ra8_err_*            The inflater's own error, returned verbatim.
 *
 * @pre @p view was accepted by `s_container_view()` for this scratch capacity.
 * @pre @p inflate and @p scratch are non-NULL (checked by the caller).
 * @post On k_ra8_ok, `scratch[0..view->total)` is the flat blob.
 * @post On any error @p scratch contents are unspecified.
 *
 * @note Not thread-safe: writes @p scratch.
 *
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_inflate_chunks(const ra8_book_container_view_t* view,
                                  ra8_book_inflate_fn              inflate,
                                  uint8_t*                         scratch)
{
  for (uint32_t i = 0U; i < view->chunk_count; ++i) { /* bound: validated chunk_count */
    const uint64_t off      = ra8_book_container_table_entry(view->table, i);
    const uint64_t next     = ra8_book_container_table_entry(view->table, i + 1U);
    const uint64_t dst_off  = (uint64_t)i * view->chunk_bytes;
    uint64_t       expected = view->total - dst_off;
    if (expected > (uint64_t)view->chunk_bytes) {
      expected = view->chunk_bytes;
    }
    size_t          produced = 0U;
    const ra8_err_t err      = inflate(&view->payload[(size_t)off],
                                       (size_t)(next - off),
                                       &scratch[(size_t)dst_off],
                                       (size_t)expected,
                                       &produced);
    if (err != k_ra8_ok) {
      return err;
    }
    if (produced != (size_t)expected) {
      return k_ra8_err_invalid_size;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Parse, inflate, validate, and publish an already-guarded open.
 *
 * @details
 * The argument-checked body of ra8_book_open(): builds the container view,
 * inflates every chunk into @p scratch, validates the reassembled blob, and
 * publishes the base/size outputs. Split from the entry point so the
 * null-guard macros and the staged error chain each stay within the
 * function-size budget.
 *
 * @param[in]  bytes       Container file bytes (non-NULL, caller-checked).
 * @param[in]  file_len    Readable length of @p bytes.
 * @param[in]  inflate     Caller decompressor (non-NULL, caller-checked).
 * @param[out] scratch     Destination for the reassembled blob.
 * @param[in]  scratch_cap Capacity of @p scratch in bytes.
 * @param[out] out_base    Receives the validated blob base.
 * @param[out] out_size    Receives the inflated blob length.
 *
 * @return ra8_err_t Status code.
 * @retval k_ra8_ok               Blob inflated, validated, and published.
 * @retval k_ra8_err_invalid_arg  Malformed container header / chunk table.
 * @retval k_ra8_err_invalid_size Short file, scratch too small, or a chunk
 *                               inflated to the wrong span.
 * @retval k_ra8_err_range_check_failed Blob CRC mismatch.
 *
 * @pre Every pointer argument was null-checked by the caller.
 * @pre @p scratch addresses at least @p scratch_cap writable bytes.
 * @post On k_ra8_ok, `*out_base == scratch` and `*out_size` is the blob length.
 * @post On any error the outputs are not modified.
 *
 * @note Not thread-safe: writes @p scratch.
 *
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_open_body(const uint8_t*      bytes,
                             size_t              file_len,
                             ra8_book_inflate_fn inflate,
                             void*               scratch,
                             size_t              scratch_cap,
                             const void**        out_base,
                             size_t*             out_size)
{
  ra8_book_container_view_t view = {};
  ra8_err_t                 err  = s_container_view(bytes, file_len, scratch_cap, &view);
  if (err != k_ra8_ok) {
    return err;
  }
  err = s_inflate_chunks(&view, inflate, (uint8_t*)scratch);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_book_validate(scratch, (size_t)view.total);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_base = scratch;
  *out_size = (size_t)view.total;
  return k_ra8_ok;
}

ra8_err_t ra8_book_open(const void*         file,
                        size_t              file_len,
                        ra8_book_inflate_fn inflate,
                        void*               scratch,
                        size_t              scratch_cap,
                        const void**        out_base,
                        size_t*             out_size)
{
  RA8_CHECK_NULL_PTR(file, s_tag_book, "open: null file");
  RA8_CHECK_NULL_PTR((const void*)inflate, s_tag_book, "open: null inflate");
  RA8_CHECK_NULL_PTR(scratch, s_tag_book, "open: null scratch");
  RA8_CHECK_NULL_PTR(out_base, s_tag_book, "open: null out_base");
  RA8_CHECK_NULL_PTR(out_size, s_tag_book, "open: null out_size");

  return s_open_body((const uint8_t*)file,
                     file_len,
                     inflate,
                     scratch,
                     scratch_cap,
                     out_base,
                     out_size);
}
