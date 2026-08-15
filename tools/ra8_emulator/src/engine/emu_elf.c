/**
 * @file emu_elf.c
 * @brief Streaming ELF32 segment, vector-base, and warm-reboot services
 * @details Program headers and PT_LOAD bytes are read from an independently
 * owned raw descriptor through bounded stack scratch. No complete image is
 * allocated, mapped, or retained in process memory.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "emu_elf.h"

#include <stdint.h>
#include <string.h>

#include "emu_elf_source_internal.h"
#include "emu_host_io_internal.h"
#include "emu_memory_access.h"

/** @brief Bounded parser and PT_LOAD transfer dimensions. */
typedef enum : uint32_t {
  k_elf_program_header_min = 32U,   /**< ELF32 program-header bytes consumed. */
  k_elf_stream_scratch     = 4096U, /**< Maximum transient segment bytes.     */
  k_elf_word_high_shift    = 24U,   /**< Shift of byte three in a word.       */
  k_elf_data_offset        = 5U,    /**< ELF identification data-byte offset. */
} emu_elf_stream_limit_t;

/** @brief Decoded program-header table geometry. */
typedef struct {
  uint32_t offset;     /**< First program-header file offset. */
  uint16_t entry_size; /**< Bytes per program-header entry.   */
  uint16_t count;      /**< Program-header entry count.       */
} emu_elf_program_table_t;

/**
 * @brief Decode one little-endian 16-bit ELF field.
 * @details Combines bytes explicitly so host byte order is irrelevant.
 * @param[in] bytes At least two readable bytes.
 * @return Decoded host value.
 * @retval uint16_t The decoded unsigned field.
 * @pre @p bytes is non-null and two-byte bounded.
 * @pre The source field uses ELF little-endian encoding.
 * @post No state changes.
 * @post The input bytes remain unchanged.
 * @note Pure and alignment-independent.
 * @since 0.1.0
 */
RA8_INTERNAL static uint16_t internal_u16(const uint8_t* bytes)
{
  return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8U));
}

/**
 * @brief Decode one little-endian 32-bit ELF field.
 * @details Combines bytes explicitly so host byte order is irrelevant.
 * @param[in] bytes At least four readable bytes.
 * @return Decoded host value.
 * @retval uint32_t The decoded unsigned field.
 * @pre @p bytes is non-null and four-byte bounded.
 * @pre The source field uses ELF little-endian encoding.
 * @post No state changes.
 * @post The input bytes remain unchanged.
 * @note Pure and alignment-independent.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_u32(const uint8_t* bytes)
{
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) | ((uint32_t)bytes[2] << 16U) |
         ((uint32_t)bytes[3] << k_elf_word_high_shift);
}

/**
 * @brief Read and validate the ELF32 ARM header and program-table geometry.
 * @details Rejects wrong magic, class, byte order, machine, and table bounds.
 * @param[in] source Open source to inspect.
 * @param[out] table Receives validated program-header geometry.
 * @param[out] machine Receives e_machine when the fixed header was readable.
 * @return Whether the source is ELF32 ARM with a wholly bounded table.
 * @retval true Both the fixed header and complete table geometry are valid.
 * @retval false The source read or any validation failed.
 * @pre @p table and @p machine are non-null.
 * @pre @p source remains open during the exact header read.
 * @post Success initializes both outputs.
 * @post Failure performs no out-of-range read.
 * @note Uses only a 52-byte stack view.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_program_table(const emu_elf_source_t*  source,
                                                emu_elf_program_table_t* table,
                                                uint16_t*                machine)
{
  uint8_t        bytes[k_elf_ehdr_size] = {};
  emu_elf_view_t view                   = {};
  if (priv_emu_elf_read(source, 0U, sizeof(bytes), bytes, sizeof(bytes), &view).status !=
      k_emu_elf_io_ok) {
    return false;
  }
  *machine = internal_u16(&bytes[k_elf_e_machine_off]);
  if ((memcmp(bytes,
              "\x7F"
              "ELF",
              4U) != 0) ||
      (bytes[4] != 1U) || (bytes[k_elf_data_offset] != 1U) ||
      (*machine != (uint16_t)k_elf_em_arm)) {
    return false;
  }
  const emu_elf_program_table_t decoded = {
    .offset     = internal_u32(&bytes[k_elf_e_phoff_off]),
    .entry_size = internal_u16(&bytes[k_elf_e_phentsize_off]),
    .count      = internal_u16(&bytes[k_elf_e_phnum_off]),
  };
  const uint64_t table_bytes = (uint64_t)decoded.entry_size * decoded.count;
  if (((decoded.count != 0U) && (decoded.entry_size < k_elf_program_header_min)) ||
      ((uint64_t)decoded.offset > source->length) ||
      (table_bytes > (source->length - decoded.offset))) {
    return false;
  }
  *table = decoded;
  return true;
}

/**
 * @brief Decode one bounds-checked PT_LOAD entry.
 * @details Reads only the fixed ELF32 header prefix and validates its payload
 * range.
 * @param[in] source Open ELF source.
 * @param[in] table Validated program-table geometry.
 * @param[in] index Entry index below `table->count`.
 * @param[out] segment Receives the decoded PT_LOAD descriptor.
 * @return True only for a non-empty, wholly bounded PT_LOAD entry.
 * @retval true A usable load segment was published.
 * @retval false The entry is unreadable, not loadable, empty, or out of range.
 * @pre All pointers are non-null and @p index is in range.
 * @pre @p source remains open during the exact read.
 * @post Success publishes no borrowed byte pointer.
 * @post Malformed/non-load entries leave @p segment untouched.
 * @note Uses only 32 bytes of stack scratch.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_load_segment(const emu_elf_source_t*        source,
                                               const emu_elf_program_table_t* table,
                                               uint16_t                       index,
                                               elf_exec_segment_t*            segment)
{
  uint8_t        bytes[k_elf_program_header_min] = {};
  emu_elf_view_t view                            = {};
  const uint64_t entry = (uint64_t)table->offset + ((uint64_t)index * table->entry_size);
  if (priv_emu_elf_read(source, entry, sizeof(bytes), bytes, sizeof(bytes), &view).status !=
      k_emu_elf_io_ok) {
    return false;
  }
  const uint32_t type   = internal_u32(bytes);
  const uint32_t offset = internal_u32(&bytes[k_elf_ph_offset_off]);
  const uint32_t filesz = internal_u32(&bytes[k_elf_ph_filesz_off]);
  if ((type != (uint32_t)k_elf_pt_load) || (filesz == 0U) || ((uint64_t)offset > source->length) ||
      ((uint64_t)filesz > (source->length - offset))) {
    return false;
  }
  *segment = (elf_exec_segment_t){
    .source = source,
    .offset = offset,
    .vaddr  = internal_u32(&bytes[k_elf_ph_vaddr_off]),
    .paddr  = internal_u32(&bytes[k_elf_ph_paddr_off]),
    .filesz = filesz,
    .flags  = internal_u32(&bytes[k_elf_ph_flags_off]),
  };
  return true;
}

uint32_t elf_foreach_load_segment(const emu_elf_source_t* elf, elf_exec_segment_fn fn, void* ctx)
{
  if ((elf == nullptr) || (fn == nullptr)) {
    return 0U;
  }
  emu_elf_program_table_t table   = {};
  uint16_t                machine = 0U;
  if (!internal_program_table(elf, &table, &machine)) {
    return 0U;
  }
  uint32_t visited = 0U;
  for (uint16_t index = 0U; index < table.count; index++) {
    elf_exec_segment_t segment = {};
    if (!internal_load_segment(elf, &table, index, &segment)) {
      continue;
    }
    visited++;
    if (!fn(&segment, ctx)) {
      break;
    }
  }
  return visited;
}

/** @brief Context for streaming PT_LOAD bytes into one Unicorn engine. */
typedef struct {
  uc_engine* uc;     /**< Destination engine.              */
  uint32_t   loaded; /**< Completely transferred segments. */
  bool       failed; /**< Sticky read/write failure.       */
} emu_elf_load_ctx_t;

/**
 * @brief Stream one PT_LOAD segment into Unicorn in bounded chunks.
 * @details Alternates exact positioned reads and Unicorn writes until complete.
 * @param[in] segment Bounds-checked load segment.
 * @param[in,out] opaque ::emu_elf_load_ctx_t destination state.
 * @return True to continue, false on the first transfer failure.
 * @retval true Every source byte was written successfully.
 * @retval false A source read or Unicorn write failed.
 * @pre Both pointers are non-null and the engine target is mapped.
 * @pre @p segment source remains open.
 * @post Success writes every segment byte at its LMA.
 * @post Failure sets the sticky context flag and emits one diagnostic.
 * @note Stack scratch is fixed at 4096 bytes regardless of segment size.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_stream_segment(const elf_exec_segment_t* segment, void* opaque)
{
  emu_elf_load_ctx_t* const ctx = (emu_elf_load_ctx_t*)opaque;
  uint8_t                   scratch[k_elf_stream_scratch];
  uint32_t                  done = 0U;
  while (done < segment->filesz) {
    const uint32_t            remain = segment->filesz - done;
    const size_t              chunk = (remain < sizeof(scratch)) ? (size_t)remain : sizeof(scratch);
    emu_elf_view_t            view  = {};
    const emu_elf_io_result_t read  = priv_emu_elf_read(segment->source,
                                                        (uint64_t)segment->offset + done,
                                                        chunk,
                                                        scratch,
                                                        sizeof(scratch),
                                                        &view);
    if ((read.status != k_emu_elf_io_ok) ||
        (emu_mem_write(ctx->uc, (uint64_t)segment->paddr + done, view.bytes, chunk) != UC_ERR_OK)) {
      (void)priv_emu_io_errf("uc_mem_write seg @0x%08X (%u bytes) failed\n",
                             segment->paddr,
                             segment->filesz);
      ctx->failed = true;
      return false;
    }
    done += (uint32_t)chunk;
  }
  (void)priv_emu_io_errf("  loaded %u bytes @ 0x%08X\n", segment->filesz, segment->paddr);
  ctx->loaded++;
  return true;
}

int load_elf(uc_engine* uc, const emu_elf_source_t* elf)
{
  emu_elf_program_table_t table   = {};
  uint16_t                machine = 0U;
  if (!internal_program_table(elf, &table, &machine)) {
    if ((machine == 0U) || (machine == (uint16_t)k_elf_em_arm)) {
      (void)priv_emu_io_errf("not a 32-bit ELF\n");
    } else {
      (void)priv_emu_io_errf("ELF e_machine %u != ARM(40)\n", machine);
    }
    return -1;
  }
  emu_elf_load_ctx_t ctx = {.uc = uc};
  (void)elf_foreach_load_segment(elf, internal_stream_segment, &ctx);
  return (!ctx.failed && (ctx.loaded > 0U)) ? 0 : -1;
}

uint32_t elf_foreach_exec_segment(const emu_elf_source_t* elf, elf_exec_segment_fn fn, void* ctx)
{
  if ((elf == nullptr) || (fn == nullptr)) {
    return 0U;
  }
  emu_elf_program_table_t table   = {};
  uint16_t                machine = 0U;
  if (!internal_program_table(elf, &table, &machine)) {
    return 0U;
  }
  uint32_t visited = 0U;
  for (uint16_t index = 0U; index < table.count; index++) {
    elf_exec_segment_t segment = {};
    if (!internal_load_segment(elf, &table, index, &segment) ||
        ((segment.flags & (uint32_t)k_elf_pf_x) == 0U)) {
      continue;
    }
    visited++;
    if (!fn(&segment, ctx)) {
      break;
    }
  }
  return visited;
}

/** @brief Accumulator for the lowest executable segment VMA. */
typedef struct {
  uint32_t base;  /**< Current lowest VMA.           */
  bool     found; /**< Whether any segment was seen. */
} emu_elf_vector_ctx_t;

/**
 * @brief Fold one executable segment into the lowest-VMA accumulator.
 * @details Replaces the candidate only when this segment has a lower VMA.
 * @param[in] segment Bounds-checked executable segment.
 * @param[in,out] opaque ::emu_elf_vector_ctx_t accumulator.
 * @return Whether the executable-segment walk should continue.
 * @retval true Every valid segment is accepted for comparison.
 * @pre @p segment is non-null.
 * @pre @p opaque points to a writable accumulator.
 * @post The accumulator retains the lowest VMA seen so far.
 * @post No source or emulator state changes.
 * @note Pure apart from the caller-owned accumulator.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_vector_segment(const elf_exec_segment_t* segment, void* opaque)
{
  emu_elf_vector_ctx_t* const ctx = (emu_elf_vector_ctx_t*)opaque;
  if (!ctx->found || (segment->vaddr < ctx->base)) {
    ctx->base  = segment->vaddr;
    ctx->found = true;
  }
  return true;
}

uint32_t elf_vector_base(const emu_elf_source_t* elf)
{
  emu_elf_vector_ctx_t ctx = {};
  (void)elf_foreach_exec_segment(elf, internal_vector_segment, &ctx);
  return ctx.found ? ctx.base : 0U;
}
