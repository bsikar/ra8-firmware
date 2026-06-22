/**
 * @file ra_book.c
 * @brief Implementation of the `.rabook` blob validator.
 *
 * @details
 * The only non-inline part of `ra_book` is integrity/bounds validation. Walking
 * a validated blob is pure offset arithmetic and lives entirely in the header.
 *
 * @since Version 1.0.0
 */
#include "ra_book.h"

#include "ra_attributes.h"
#include "ra_check.h"

/** @brief Log tag for `ra_book` validation diagnostics. */
static const char* const s_tag_book = "ra_book";

/**
 * @enum ra_book_crc_const_t
 * @brief Constants for the reflected CRC-32/ISO-HDLC used in the trailer.
 * @details Matches Python `zlib.crc32`, so a blob produced by
 *          `tools/epub_compile` verifies bit-for-bit on device.
 * @since Version 1.0.0
 */
typedef enum : uint32_t {
  k_ra_book_crc_init  = 0xFFFFFFFFU, /**< CRC seed and final XOR mask.     */
  k_ra_book_crc_poly  = 0xEDB88320U, /**< Reflected polynomial.            */
} ra_book_crc_const_t;

/**
 * @enum ra_book_crc_bits_t
 * @brief Bit-loop bound for the CRC-32 inner loop.
 * @since Version 1.0.0
 */
typedef enum : uint8_t {
  k_ra_book_crc_bits_per_byte = 8U, /**< Inner-loop iterations per input byte. */
} ra_book_crc_bits_t;

/** @brief Implementation of `ra_book_crc32()` -- bitwise reflected CRC-32. */
RA_NO_RECURSION
static uint32_t ra_book_crc32(const uint8_t* data, size_t len) {
  uint32_t crc = k_ra_book_crc_init;
  for (size_t i = 0U; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0U; bit < k_ra_book_crc_bits_per_byte; ++bit) {
      uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
      crc = (crc >> 1U) ^ (k_ra_book_crc_poly & mask);
    }
  }
  return crc ^ k_ra_book_crc_init;
}

/**
 * @brief Implementation of `ra_book_table_fits()` -- overflow-safe extent check.
 * @details Returns true when `[off, off + count*elem)` lies within `total`,
 *          computed in 64-bit to avoid 32-bit wraparound on hostile inputs.
 */
static bool ra_book_table_fits(uint32_t off, uint32_t count, uint32_t elem, uint32_t total) {
  uint64_t end = (uint64_t)off + (uint64_t)count * (uint64_t)elem;
  return (off <= total) && (end <= (uint64_t)total);
}

ra_err_t ra_book_validate(const void* base, size_t size) {
  RA_CHECK_NULL_PTR(base, s_tag_book, "validate: null base");

  if (size < sizeof(ra_book_header_t)) {
    return k_ra_err_invalid_size;
  }

  const ra_book_header_t* hdr = (const ra_book_header_t*)base;

  const char expect[8] = {'R', 'A', 'B', 'O', 'O', 'K', '1', '\0'};
  for (uint8_t i = 0U; i < (uint8_t)sizeof(expect); ++i) {
    if (hdr->magic[i] != expect[i]) {
      return k_ra_err_invalid_arg;
    }
  }
  if (hdr->format_version != k_ra_book_format_version) {
    return k_ra_err_invalid_arg;
  }

  uint32_t total = hdr->total_size;
  if ((total < sizeof(ra_book_header_t)) || ((size_t)total > size)) {
    return k_ra_err_invalid_size;
  }

  const bool tables_ok =
      ra_book_table_fits(hdr->chapter_off, hdr->chapter_count, sizeof(ra_book_chapter_t), total) &&
      ra_book_table_fits(hdr->node_off, hdr->node_count, sizeof(ra_book_node_t), total) &&
      ra_book_table_fits(hdr->attr_off, hdr->attr_count, sizeof(ra_book_attr_t), total) &&
      ra_book_table_fits(hdr->stylesheet_off, hdr->stylesheet_count,
                         sizeof(ra_book_stylesheet_t), total) &&
      ra_book_table_fits(hdr->image_off, hdr->image_count, sizeof(ra_book_image_t), total) &&
      ra_book_table_fits(hdr->string_off, hdr->string_size, 1U, total) &&
      ra_book_table_fits(hdr->image_pool_off, hdr->image_pool_size, 1U, total);
  if (!tables_ok) {
    return k_ra_err_invalid_size;
  }

  const uint8_t* body = (const uint8_t*)base + sizeof(ra_book_header_t);
  uint32_t body_len = total - (uint32_t)sizeof(ra_book_header_t);
  if (ra_book_crc32(body, body_len) != hdr->crc32) {
    return k_ra_err_range_check_failed;
  }

  return k_ra_ok;
}
