/**
 * @file emu_elf_symbols.c
 * @brief Bounded raw-descriptor ELF32 symbol and string streaming
 * @details Section headers, symbols, and names are decoded through small stack
 * views. Consumers retain source offsets rather than pointers into a whole-file
 * allocation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "emu_elf.h"
#include "emu_elf_source_internal.h"

/** @brief Fixed bytes consumed from section and symbol entries. */
typedef enum : uint32_t {
  k_elf_symbol_entry_min     = 16U,  /**< ELF32 symbol bytes consumed.         */
  k_elf_string_chunk         = 256U, /**< Transient streamed-name bytes.       */
  k_elf_sht_symtab           = 2U,   /**< SHT_SYMTAB section type.             */
  k_elf_word_high_shift      = 24U,  /**< Shift of byte three in a word.       */
  k_elf_data_offset          = 5U,   /**< ELF identification data-byte offset. */
  k_elf_shentsize_offset     = 46U,  /**< Section-entry-size header offset.    */
  k_elf_section_count_offset = 48U,  /**< Section-count header offset.         */
} emu_elf_symbol_limit_t;

/** @brief Decoded section-header table geometry. */
typedef struct {
  uint32_t offset;     /**< First section-header file offset. */
  uint16_t entry_size; /**< Bytes per section-header entry.   */
  uint16_t count;      /**< Section-header entry count.       */
} emu_elf_section_table_t;

/** @brief Decoded usable symbol/string table pair. */
typedef struct {
  uint32_t symbol_offset; /**< First symbol entry file offset.  */
  uint32_t symbol_count;  /**< Number of whole symbol entries.  */
  uint32_t entry_size;    /**< Bytes per symbol entry.          */
  uint32_t string_offset; /**< Linked string-table file offset. */
  uint32_t string_size;   /**< Linked string-table byte count.  */
} emu_elf_symbol_table_t;

/**
 * @brief Decode one little-endian 16-bit field.
 * @details Combines exact bytes so host byte order is irrelevant.
 * @param[in] bytes Two-byte little-endian field.
 * @return Decoded unsigned value.
 * @retval uint16_t The decoded field.
 * @pre @p bytes is non-null.
 * @pre @p bytes spans at least two readable bytes.
 * @post The input bytes remain unchanged.
 * @post No global state changes.
 * @note Pure and alignment-independent.
 * @since 0.1.0
 */
RA8_INTERNAL static uint16_t internal_symbol_u16(const uint8_t* bytes)
{
  return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8U));
}

/**
 * @brief Decode one little-endian 32-bit field without alignment assumptions.
 * @details Combines exact bytes so host byte order is irrelevant.
 * @param[in] bytes Four-byte little-endian field.
 * @return Decoded unsigned value.
 * @retval uint32_t The decoded field.
 * @pre @p bytes is non-null.
 * @pre @p bytes spans at least four readable bytes.
 * @post The input bytes remain unchanged.
 * @post No global state changes.
 * @note Pure and alignment-independent.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_symbol_u32(const uint8_t* bytes)
{
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) | ((uint32_t)bytes[2] << 16U) |
         ((uint32_t)bytes[3] << k_elf_word_high_shift);
}

/**
 * @brief Read one fixed-size source object into supplied scratch.
 * @details Delegates one exact positioned read and publishes only success.
 * @param[in] elf Open source.
 * @param[in] offset Source byte offset.
 * @param[out] bytes Destination scratch.
 * @param[in] length Exact object length.
 * @return Whether the complete object was read.
 * @retval true Every requested byte was read.
 * @retval false Validation, EOF, capacity, or host I/O failed.
 * @pre @p bytes spans @p length writable bytes.
 * @pre @p elf remains open during the read.
 * @post Success initializes all destination bytes.
 * @post Failure publishes no view.
 * @note Thin local predicate over ::priv_emu_elf_read.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_symbol_read(const emu_elf_source_t* elf, uint64_t offset, uint8_t* bytes, size_t length)
{
  emu_elf_view_t view = {};
  return priv_emu_elf_read(elf, offset, length, bytes, length, &view).status == k_emu_elf_io_ok;
}

/**
 * @brief Decode and validate section-header table geometry.
 * @details Rejects wrong magic, class, byte order, and out-of-range tables.
 * @param[in] elf Open source.
 * @param[out] table Receives bounded geometry.
 * @return Whether the fixed ELF header and complete table are usable.
 * @retval true Complete validated geometry was published.
 * @retval false The fixed header or table geometry is unusable.
 * @pre @p table is non-null.
 * @pre @p elf remains open.
 * @post Success initializes @p table.
 * @post Failure performs no out-of-range table read.
 * @note Uses only 52 bytes of stack scratch.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_section_table(const emu_elf_source_t*  elf,
                                                emu_elf_section_table_t* table)
{
  uint8_t bytes[k_elf_ehdr_size] = {};
  if (!internal_symbol_read(elf, 0U, bytes, sizeof(bytes)) ||
      (memcmp(bytes,
              "\x7F"
              "ELF",
              4U) != 0) ||
      (bytes[4] != 1U) || (bytes[k_elf_data_offset] != 1U)) {
    return false;
  }
  const emu_elf_section_table_t decoded = {
    .offset     = internal_symbol_u32(&bytes[32]),
    .entry_size = internal_symbol_u16(&bytes[k_elf_shentsize_offset]),
    .count      = internal_symbol_u16(&bytes[k_elf_section_count_offset]),
  };
  const uint64_t table_bytes = (uint64_t)decoded.entry_size * decoded.count;
  if ((decoded.offset == 0U) || (decoded.entry_size < k_elf_shentsize_min) ||
      ((uint64_t)decoded.offset > elf->length) || (table_bytes > (elf->length - decoded.offset))) {
    return false;
  }
  *table = decoded;
  return true;
}

/**
 * @brief Read one section header from validated table geometry.
 * @details Computes the entry offset in 64 bits and reads only the fixed prefix.
 * @param[in] elf Open source.
 * @param[in] sections Validated section table.
 * @param[in] index Entry index below `sections->count`.
 * @param[out] bytes Forty-byte section-header scratch.
 * @return Whether the exact section header was read.
 * @retval true The fixed section header was read completely.
 * @retval false The positioned read failed.
 * @pre All pointers are non-null and @p index is in range.
 * @pre @p bytes spans ::k_elf_shentsize_min bytes.
 * @post Success initializes all consumed fields.
 * @post Failure does not publish a section view.
 * @note Entry padding beyond forty bytes is never read.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_section_read(const emu_elf_source_t*        elf,
                                               const emu_elf_section_table_t* sections,
                                               uint32_t                       index,
                                               uint8_t bytes[k_elf_shentsize_min])
{
  const uint64_t offset = (uint64_t)sections->offset + ((uint64_t)index * sections->entry_size);
  return internal_symbol_read(elf, offset, bytes, k_elf_shentsize_min);
}

/**
 * @brief Decode one SHT_SYMTAB and its linked string-table bounds.
 * @details Validates both source ranges and the linked section index before publication.
 * @param[in] elf Open source.
 * @param[in] sections Validated section table.
 * @param[in] bytes Candidate section-header bytes.
 * @param[out] table Receives usable paired-table geometry.
 * @return True only for a wholly bounded SHT_SYMTAB pair.
 * @retval true A usable symbol/string pair was published.
 * @retval false The candidate or its linked table is unusable.
 * @pre All pointers are non-null.
 * @pre @p bytes contains one exact section header.
 * @post Success initializes @p table with nonzero entry size.
 * @post Failure leaves @p table untouched.
 * @note Malformed linked tables are skipped, never partially walked.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_symbol_table(const emu_elf_source_t*        elf,
                                               const emu_elf_section_table_t* sections,
                                               const uint8_t           bytes[k_elf_shentsize_min],
                                               emu_elf_symbol_table_t* table)
{
  const uint32_t type       = internal_symbol_u32(&bytes[4]);
  const uint32_t symbol_off = internal_symbol_u32(&bytes[16]);
  const uint32_t symbol_sz  = internal_symbol_u32(&bytes[k_elf_sh_size_off]);
  const uint32_t link       = internal_symbol_u32(&bytes[k_elf_sh_link_off]);
  const uint32_t entry_sz   = internal_symbol_u32(&bytes[k_elf_sh_entsize_off]);
  if ((type != k_elf_sht_symtab) || (entry_sz < k_elf_symbol_entry_min) ||
      (link >= sections->count) || ((uint64_t)symbol_off > elf->length) ||
      ((uint64_t)symbol_sz > (elf->length - symbol_off))) {
    return false;
  }
  uint8_t strings[k_elf_shentsize_min] = {};
  if (!internal_section_read(elf, sections, link, strings)) {
    return false;
  }
  const uint32_t string_off = internal_symbol_u32(&strings[16]);
  const uint32_t string_sz  = internal_symbol_u32(&strings[k_elf_sh_size_off]);
  if (((uint64_t)string_off > elf->length) || ((uint64_t)string_sz > (elf->length - string_off))) {
    return false;
  }
  *table = (emu_elf_symbol_table_t){
    .symbol_offset = symbol_off,
    .symbol_count  = symbol_sz / entry_sz,
    .entry_size    = entry_sz,
    .string_offset = string_off,
    .string_size   = string_sz,
  };
  return true;
}

/**
 * @brief Walk every entry of one validated symbol table.
 * @details Reads only fixed entry prefixes and skips invalid string offsets.
 * @param[in] elf Open source.
 * @param[in] table Validated paired-table geometry.
 * @param[in] fn Caller symbol consumer.
 * @param[in,out] ctx Opaque callback context.
 * @param[in,out] visited Running delivered-entry count.
 * @return True to continue to another table, false after callback stop/read fault.
 * @retval true Every usable entry was delivered.
 * @retval false A read failed or the callback stopped the walk.
 * @pre All pointers are non-null.
 * @pre Table ranges are wholly source-bounded.
 * @post @p visited advances only for delivered entries.
 * @post No symbol source bytes are retained after a callback returns.
 * @note Reads sixteen bytes per symbol regardless of padded entry size.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_symbol_walk(const emu_elf_source_t*       elf,
                                              const emu_elf_symbol_table_t* table,
                                              emu_elf_symbol_fn             fn,
                                              void*                         ctx,
                                              uint32_t*                     visited)
{
  for (uint32_t index = 0U; index < table->symbol_count; index++) {
    uint8_t        bytes[k_elf_symbol_entry_min] = {};
    const uint64_t offset = (uint64_t)table->symbol_offset + ((uint64_t)index * table->entry_size);
    if (!internal_symbol_read(elf, offset, bytes, sizeof(bytes))) {
      return false;
    }
    const uint32_t relative_name = internal_symbol_u32(bytes);
    if (relative_name >= table->string_size) {
      continue;
    }
    const emu_elf_symbol_t symbol = {
      .name_offset = (uint64_t)table->string_offset + relative_name,
      .value       = internal_symbol_u32(&bytes[4]),
      .size        = internal_symbol_u32(&bytes[8]),
      .info        = bytes[k_elf_sym_info_off],
    };
    (*visited)++;
    if (!fn(&symbol, ctx)) {
      return false;
    }
  }
  return true;
}

uint32_t elf_foreach_symbol(const emu_elf_source_t* elf, emu_elf_symbol_fn fn, void* ctx)
{
  if ((elf == nullptr) || (fn == nullptr)) {
    return 0U;
  }
  emu_elf_section_table_t sections = {};
  if (!internal_section_table(elf, &sections)) {
    return 0U;
  }
  uint32_t visited = 0U;
  for (uint32_t index = 0U; index < sections.count; index++) {
    uint8_t                bytes[k_elf_shentsize_min] = {};
    emu_elf_symbol_table_t table                      = {};
    if (!internal_section_read(elf, &sections, index, bytes) ||
        !internal_symbol_table(elf, &sections, bytes, &table)) {
      continue;
    }
    if (!internal_symbol_walk(elf, &table, fn, ctx, &visited)) {
      break;
    }
  }
  return visited;
}

bool elf_string_foreach(const emu_elf_source_t* elf,
                        uint64_t                offset,
                        emu_elf_string_fn       fn,
                        void*                   ctx)
{
  if ((elf == nullptr) || (fn == nullptr) || ((uint64_t)offset >= elf->length)) {
    return false;
  }
  uint8_t  bytes[k_elf_string_chunk];
  uint64_t cursor = offset;
  while (cursor < elf->length) {
    const uint64_t remaining = elf->length - cursor;
    const size_t   chunk     = (remaining < sizeof(bytes)) ? (size_t)remaining : sizeof(bytes);
    if (!internal_symbol_read(elf, cursor, bytes, chunk)) {
      return false;
    }
    const uint8_t* const end    = (const uint8_t*)memchr(bytes, 0, chunk);
    const size_t         length = (end == nullptr) ? chunk : (size_t)(end - bytes);
    if ((length > 0U) && !fn((const char*)bytes, length, ctx)) {
      return false;
    }
    if (end != nullptr) {
      return true;
    }
    cursor += chunk;
  }
  return false;
}

/** @brief Incremental exact string comparison state. */
typedef struct {
  const char* expected; /**< Expected NUL-terminated name.  */
  size_t      length;   /**< Expected length excluding NUL. */
  size_t      compared; /**< Bytes compared so far.         */
  bool        equal;    /**< Sticky exact-prefix equality.  */
} emu_elf_name_match_t;

/**
 * @brief Compare one streamed source-name chunk with the expected suffix.
 * @details Advances the match only while the complete chunk remains equal.
 * @param[in] bytes Non-empty transient name bytes.
 * @param[in] length Chunk length.
 * @param[in,out] opaque ::emu_elf_name_match_t comparison state.
 * @return Whether string streaming should continue.
 * @retval true The chunk matched the expected suffix.
 * @retval false The chunk exceeded or differed from the expected suffix.
 * @pre @p bytes is non-null and spans @p length bytes.
 * @pre @p opaque points to writable comparison state.
 * @post Success advances the compared-byte count by @p length.
 * @post Failure clears the sticky equality flag.
 * @note Pure apart from the caller-owned comparison state.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_name_chunk(const char* bytes, size_t length, void* opaque)
{
  emu_elf_name_match_t* const match = (emu_elf_name_match_t*)opaque;
  if ((length > (match->length - match->compared)) ||
      (memcmp(bytes, &match->expected[match->compared], length) != 0)) {
    match->equal = false;
    return false;
  }
  match->compared += length;
  return true;
}

/** @brief Symbol-address lookup callback state. */
typedef struct {
  const emu_elf_source_t* source;  /**< Open source for name streaming. */
  const char*             name;    /**< Expected symbol name.           */
  size_t                  length;  /**< Expected name length.           */
  uint32_t                address; /**< Resolved Thumb-cleared value.   */
  uint32_t                size;    /**< Resolved symbol size.           */
} emu_elf_lookup_t;

/**
 * @brief Compare one symbol name and stop on an exact hit.
 * @details Streams the referenced name and publishes address/size only on equality.
 * @param[in] symbol Bounds-checked symbol with an absolute name offset.
 * @param[in,out] opaque ::emu_elf_lookup_t lookup state.
 * @return Whether the symbol walk should continue.
 * @retval true This symbol did not exactly match the requested name.
 * @retval false The symbol matched and the lookup is complete.
 * @pre @p symbol is non-null.
 * @pre @p opaque points to writable lookup state with an open source.
 * @post A match publishes the Thumb-cleared address and symbol size.
 * @post A miss leaves the prior lookup result unchanged.
 * @note Reads name bytes through bounded transient chunks.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_lookup_symbol(const emu_elf_symbol_t* symbol, void* opaque)
{
  emu_elf_lookup_t* const lookup = (emu_elf_lookup_t*)opaque;
  emu_elf_name_match_t match = {.expected = lookup->name, .length = lookup->length, .equal = true};
  const bool           complete =
    elf_string_foreach(lookup->source, symbol->name_offset, internal_name_chunk, &match);
  if (!complete || !match.equal || (match.compared != match.length)) {
    return true;
  }
  lookup->address = symbol->value & ~1U;
  lookup->size    = symbol->size;
  return false;
}

uint32_t elf_sym_addr(const emu_elf_source_t* elf, const char* name, uint32_t* size_out)
{
  if (size_out != nullptr) {
    *size_out = 0U;
  }
  if ((elf == nullptr) || (name == nullptr)) {
    return 0U;
  }
  emu_elf_lookup_t lookup = {.source = elf, .name = name, .length = strlen(name)};
  (void)elf_foreach_symbol(elf, internal_lookup_symbol, &lookup);
  if ((lookup.address != 0U) && (size_out != nullptr)) {
    *size_out = lookup.size;
  }
  return lookup.address;
}
