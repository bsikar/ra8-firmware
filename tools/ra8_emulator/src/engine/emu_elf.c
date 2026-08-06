/**
 * @file emu_elf.c
 * @brief ELF32 image services implementation (see emu_elf.h)
 *
 * @details
 * File reading, PT_LOAD segment loading, executable-VMA vector-base derivation
 * and .symtab symbol resolution -- moved verbatim out of the ra8_emulator main
 * translation unit. The contracts live on the declarations in emu_elf.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 *
 *

 */

#include "emu_elf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_console.h"
#include "board_net.h"
#include "board_periph.h"
#include "emu_console.h"
#include "emu_exc.h"
#include "emu_memmap.h"
#include "emu_mpu.h"
#include "emu_seams.h"
#include "emu_view.h"
#include "ra8_attributes.h"

RA8_NASA_RULE_3_OK /* host-only emu: elf file buffer */
  uint8_t*
  read_file(const char* path, long* out_len)
{
  FILE* f = fopen(path, "rb");
  if (f == NULL) {
    return nullptr;
  }
  (void)fseek(f, 0, SEEK_END);
  const long len = ftell(f);
  (void)fseek(f, 0, SEEK_SET);
  uint8_t* buf = (uint8_t*)malloc((size_t)len);
  if ((buf != nullptr) && (fread(buf, 1U, (size_t)len, f) != (size_t)len)) {
    free(buf);
    buf = nullptr;
  }
  (void)fclose(f);
  *out_len = len;
  return buf;
}

int load_elf(uc_engine* uc, const uint8_t* elf, long len)
{
  if ((len < (long)k_elf_ehdr_size) ||
      (memcmp(elf,
              "\x7F"
              "ELF",
              4) != 0) ||
      (elf[4] != 1) /* ELFCLASS32 */) {
    (void)fprintf(stderr, "not a 32-bit ELF\n");
    return -1;
  }
  const uint16_t e_machine =
    (uint16_t)(elf[k_elf_e_machine_off] | (elf[k_elf_e_machine_off + 1U] << 8));
  if (e_machine != (uint16_t)k_elf_em_arm) {
    (void)fprintf(stderr, "ELF e_machine %u != ARM(40)\n", e_machine);
    return -1;
  }
  uint32_t phoff = 0U;
  (void)memcpy(&phoff, elf + (uint32_t)k_elf_e_phoff_off, 4);
  uint16_t phentsize =
    (uint16_t)(elf[k_elf_e_phentsize_off] | (elf[k_elf_e_phentsize_off + 1U] << 8));
  uint16_t phnum  = (uint16_t)(elf[k_elf_e_phnum_off] | (elf[k_elf_e_phnum_off + 1U] << 8));
  int      loaded = 0;
  for (uint16_t i = 0U; i < phnum; i++) {
    const uint8_t* ph = elf + phoff + ((size_t)(uint32_t)i * phentsize);
    uint32_t       p_type;
    uint32_t       p_offset;
    uint32_t       p_paddr;
    uint32_t       p_filesz;
    (void)memcpy(&p_type, ph + 0, 4);
    (void)memcpy(&p_offset, ph + 4, 4);
    (void)memcpy(&p_paddr, ph + (uint32_t)k_elf_ph_paddr_off, 4); /* load address (LMA) */
    (void)memcpy(&p_filesz, ph + 16, 4);
    if ((p_type != 1U /* PT_LOAD */) || (p_filesz == 0U)) {
      continue;
    }
    if (uc_mem_write(uc, p_paddr, elf + p_offset, p_filesz) != UC_ERR_OK) {
      (void)fprintf(stderr, "uc_mem_write seg @0x%08X (%u bytes) failed\n", p_paddr, p_filesz);
      return -1;
    }
    (void)fprintf(stderr, "  loaded %u bytes @ 0x%08X\n", p_filesz, p_paddr);
    loaded++;
  }
  return (loaded > 0) ? 0 : -1;
}

uint32_t elf_vector_base(const uint8_t* elf, long len)
{
  if ((elf == nullptr) || (len < (long)k_elf_ehdr_size)) {
    return 0U;
  }
  uint32_t phoff = 0U;
  (void)memcpy(&phoff, elf + (uint32_t)k_elf_e_phoff_off, 4);
  const uint16_t phentsize = (uint16_t)(elf[42] | (elf[43] << 8));
  const uint16_t phnum     = (uint16_t)(elf[44] | (elf[45] << 8));
  if (((uint64_t)phoff + ((uint64_t)phnum * (uint64_t)phentsize)) > (uint64_t)len) {
    return 0U;
  }
  uint32_t min_vaddr = 0U;
  bool     found     = false;
  for (uint16_t i = 0U; i < phnum; i++) {
    const uint8_t* ph       = elf + phoff + ((size_t)(uint32_t)i * phentsize);
    uint32_t       p_type   = 0U;
    uint32_t       p_vaddr  = 0U;
    uint32_t       p_filesz = 0U;
    uint32_t       p_flags  = 0U;
    (void)memcpy(&p_type, ph + 0, 4);
    (void)memcpy(&p_vaddr, ph + (uint32_t)k_elf_ph_vaddr_off, 4);
    (void)memcpy(&p_filesz, ph + (uint32_t)k_elf_ph_filesz_off, 4);
    (void)memcpy(&p_flags, ph + (uint32_t)k_elf_ph_flags_off, 4);
    if ((p_type != (uint32_t)k_elf_pt_load) || (p_filesz == 0U) ||
        ((p_flags & (uint32_t)k_elf_pf_x) == 0U)) {
      continue;
    }
    if (!found || (p_vaddr < min_vaddr)) {
      min_vaddr = p_vaddr;
      found     = true;
    }
  }
  return found ? min_vaddr : 0U;
}

uint32_t elf_foreach_exec_segment(const uint8_t* elf, long len, elf_exec_segment_fn fn, void* ctx)
{
  if ((elf == nullptr) || (fn == nullptr) || (len < (long)k_elf_ehdr_size)) {
    return 0U;
  }
  uint32_t phoff = 0U;
  (void)memcpy(&phoff, elf + (uint32_t)k_elf_e_phoff_off, 4);
  const uint16_t phentsize = (uint16_t)(elf[42] | (elf[43] << 8));
  const uint16_t phnum     = (uint16_t)(elf[44] | (elf[45] << 8));
  if (((uint64_t)phoff + ((uint64_t)phnum * (uint64_t)phentsize)) > (uint64_t)len) {
    return 0U; /* truncated program-header table -- refuse to walk past the file. */
  }

  uint32_t visited = 0U;
  for (uint16_t i = 0U; i < phnum; i++) {
    const uint8_t* ph       = elf + phoff + ((size_t)(uint32_t)i * phentsize);
    uint32_t       p_type   = 0U;
    uint32_t       p_offset = 0U;
    uint32_t       p_vaddr  = 0U;
    uint32_t       p_filesz = 0U;
    uint32_t       p_flags  = 0U;
    (void)memcpy(&p_type, ph + 0, 4);
    (void)memcpy(&p_offset, ph + (uint32_t)k_elf_ph_offset_off, 4);
    (void)memcpy(&p_vaddr, ph + (uint32_t)k_elf_ph_vaddr_off, 4);
    (void)memcpy(&p_filesz, ph + (uint32_t)k_elf_ph_filesz_off, 4);
    (void)memcpy(&p_flags, ph + (uint32_t)k_elf_ph_flags_off, 4);
    const bool executable = ((p_flags & (uint32_t)k_elf_pf_x) != 0U);
    if ((p_type != (uint32_t)k_elf_pt_load) || (p_filesz == 0U) || !executable) {
      continue;
    }
    if (((uint64_t)p_offset + (uint64_t)p_filesz) > (uint64_t)len) {
      continue; /* truncated / out-of-file segment -- skip. */
    }
    const elf_exec_segment_t seg = {.bytes = elf + p_offset, .vaddr = p_vaddr, .filesz = p_filesz};
    visited++;
    if (!fn(&seg, ctx)) {
      break;
    }
  }
  return visited;
}

/**
 * @struct elf_symtab_t
 * @brief Where one SHT_SYMTAB section and its string table live in the image.
 *
 * @details
 * Decoded once from a section header so the symbol scan below works from plain
 * offsets instead of re-reading header fields inside its loop.
 *
 * @invariant `entsize` is non-zero, so `count` is always well defined.
 * @see elf_symtab_decode  Fills this in from a section header.
 * @see elf_symtab_find    Consumes it.
 */
typedef struct {
  uint32_t sym_off; /**< File offset of the symbol array.        */
  uint32_t entsize; /**< Bytes per symbol entry (>= 16).         */
  uint32_t count;   /**< Symbol entries in the table.            */
  uint32_t str_off; /**< File offset of the linked string table. */
} elf_symtab_t;

/**
 * @brief Decode a section header into ::elf_symtab_t if it is a usable SYMTAB.
 *
 * @param[in]  elf       Base of the mapped ELF image.
 * @param[in]  sh        Section header to decode.
 * @param[in]  shoff     File offset of the section-header table.
 * @param[in]  shentsize Bytes per section header.
 * @param[in]  shnum     Section-header count, bounding the `sh_link` index.
 * @param[out] out       Receives the decoded table on success.
 *
 * @return True when `sh` is an SHT_SYMTAB whose entry size and string-table
 *         link are both usable; false otherwise (the caller skips the section).
 *
 * @pre `elf` and `out` are non-NULL.
 * @pre `sh` points inside the image (the caller bounds-checks it).
 * @post On true, `out->entsize` is non-zero.
 * @post On false, `*out` is untouched.
 *
 * @note Not thread-safe with respect to the image it reads.
 */
static bool elf_symtab_decode(const uint8_t* elf,
                              const uint8_t* sh,
                              uint32_t       shoff,
                              uint16_t       shentsize,
                              uint16_t       shnum,
                              elf_symtab_t*  out)
{
  uint32_t sh_type = 0U;
  (void)memcpy(&sh_type, sh + 4, 4);
  if (sh_type != 2U /* SHT_SYMTAB */) {
    return false;
  }
  uint32_t sym_off     = 0U;
  uint32_t sym_size    = 0U;
  uint32_t sym_link    = 0U;
  uint32_t sym_entsize = 0U;
  (void)memcpy(&sym_off, sh + 16, 4);
  (void)memcpy(&sym_size, sh + (uint32_t)k_elf_sh_size_off, 4);
  (void)memcpy(&sym_link, sh + (uint32_t)k_elf_sh_link_off, 4);
  (void)memcpy(&sym_entsize, sh + (uint32_t)k_elf_sh_entsize_off, 4);
  if ((sym_entsize < 16U) || (sym_link >= shnum)) {
    return false;
  }
  const uint8_t* strsh   = elf + shoff + ((size_t)(uint32_t)sym_link * shentsize);
  uint32_t       str_off = 0U;
  (void)memcpy(&str_off, strsh + 16, 4);

  out->sym_off = sym_off;
  out->entsize = sym_entsize;
  out->count   = sym_size / sym_entsize;
  out->str_off = str_off;
  return true;
}

/**
 * @brief Find `name` in one decoded symbol table.
 *
 * @param[in]  elf      Base of the mapped ELF image.
 * @param[in]  len      Image length in bytes, bounding every read.
 * @param[in]  name     Symbol name to match.
 * @param[in]  nlen     `strlen(name) + 1`, so the NUL is compared too.
 * @param[in]  st       Table to search.
 * @param[out] size_out Receives `st_size` on a hit; may be NULL.
 *
 * @return The symbol's value with the Thumb bit cleared, or 0 when absent.
 *
 * @pre `elf`, `name` and `st` are non-NULL.
 * @pre `st->entsize` is non-zero.
 * @post `*size_out` is written only on a hit.
 * @post Every read stays inside the first `len` bytes of the image.
 *
 * @note Not thread-safe with respect to the image it reads.
 */
static uint32_t elf_symtab_find(const uint8_t*      elf,
                                long                len,
                                const char*         name,
                                size_t              nlen,
                                const elf_symtab_t* st,
                                uint32_t*           size_out)
{
  for (uint32_t s = 0U; s < st->count; s++) {
    const uint8_t* sym = elf + st->sym_off + ((size_t)s * st->entsize);
    if (((size_t)(sym - elf) + 16U) > (size_t)len) {
      break;
    }
    uint32_t st_name  = 0U;
    uint32_t st_value = 0U;
    uint32_t st_size  = 0U;
    (void)memcpy(&st_name, sym + 0, 4);
    (void)memcpy(&st_value, sym + 4, 4);
    (void)memcpy(&st_size, sym + 8, 4);
    const size_t pos = (size_t)st->str_off + (size_t)st_name;
    if ((st_name == 0U) || ((pos + nlen) > (size_t)len)) {
      continue;
    }
    if (memcmp(elf + pos, name, nlen) == 0) {
      if (size_out != nullptr) {
        *size_out = st_size;
      }
      return st_value & ~1U; /* clear the Thumb bit for the hook address. */
    }
  }
  return 0U;
}

uint32_t elf_sym_addr(const uint8_t* elf, long len, const char* name, uint32_t* size_out)
{
  if (size_out != nullptr) {
    *size_out = 0U;
  }
  if (len < (long)k_elf_ehdr_size) {
    return 0U;
  }
  uint32_t shoff = 0U;
  (void)memcpy(&shoff, elf + 32, 4);
  const uint16_t shentsize = (uint16_t)(elf[46] | (elf[47] << 8));
  const uint16_t shnum     = (uint16_t)(elf[48] | (elf[49] << 8));
  const size_t   nlen      = strlen(name) + 1U;
  if ((shoff == 0U) || (shentsize < (uint32_t)k_elf_shentsize_min)) {
    return 0U;
  }
  for (uint16_t i = 0U; i < shnum; i++) {
    const uint8_t* sh = elf + shoff + ((size_t)(uint32_t)i * shentsize);
    if (((size_t)(sh - elf) + (size_t)k_elf_shentsize_min) > (size_t)len) {
      break;
    }
    elf_symtab_t st = {};
    if (!elf_symtab_decode(elf, sh, shoff, shentsize, shnum, &st)) {
      continue;
    }
    const uint32_t hit = elf_symtab_find(elf, len, name, nlen, &st, size_out);
    if (hit != 0U) {
      return hit;
    }
  }
  return 0U;
}

/** @brief Implementation of `warm_reboot()` -- PT_LOAD re-write + model resets. */
uint32_t warm_reboot(uc_engine* uc, const uint8_t* elf, long len, bool trace)
{
  /* 1. Restore the code + .data initial image from the ELF PT_LOAD segments.
   * The same elf/len loaded successfully at startup, so a failure here means the
   * engine's memory writes started failing -- report and return PC=0 so the
   * caller ends the run rather than execute a stale image. */
  if (load_elf(uc, elf, len) != 0) {
    (void)fprintf(stderr, "ra8_emulator: warm_reboot: load_elf failed -- ending run\n");
    return 0U;
  }

  /* 2. Reset the peripheral + network models (RSTSRn / VBATT backup survive). */
  board_periph_init(trace);
  board_net_init(trace);

  /* 3. Clear host-side exception / scheduler bookkeeping. */
  emu_exc_reset();
  emu_mpu_clear_fault();
  emu_div0_clear_fault();
  emu_div0_disarm(); /* a warm reboot re-loads the image, un-patching sites. */
  /* Clear the multi-channel console store + the in-flight ITM line, and reset
   * the tabbed-console view so the rebooted firmware starts with an empty
   * console on the ALL tab (the SCI model's own reset clears its line buffers). */
  board_console_reset();
  emu_console_reset();
  emu_view_reset_console();

  /* 4. Re-read the Cortex-M reset vector (SP = vectors[0], PC = vectors[1]). */
  uint32_t sp = 0U;
  uint32_t pc = 0U;
  (void)uc_mem_read(uc, emu_memmap_mram_base() + 0U, &sp, 4);
  (void)uc_mem_read(uc, emu_memmap_mram_base() + 4U, &pc, 4);
  pc |= 1U; /* Thumb */
  uint32_t xpsr = (uint32_t)k_xpsr_t_bit;
  (void)uc_reg_write(uc, UC_ARM_REG_SP, &sp);
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &pc);
  (void)uc_reg_write(uc, UC_ARM_REG_XPSR, &xpsr);
  (void)fprintf(stderr, "ra8_emulator: warm reboot -- reset SP=0x%08X PC=0x%08X\n", sp, pc);
  return pc;
}
