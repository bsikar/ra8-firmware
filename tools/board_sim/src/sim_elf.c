/**
 * @file sim_elf.c
 * @brief ELF32 image services implementation (see sim_elf.h)
 *
 * @details
 * File reading, PT_LOAD segment loading, executable-VMA vector-base derivation
 * and .symtab symbol resolution -- moved verbatim out of the board_sim main
 * translation unit. The contracts live on the declarations in sim_elf.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include "sim_elf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t* read_file(const char* path, long* out_len)
{
  FILE* f = fopen(path, "rb");
  if (f == nullptr) {
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
  const uint16_t e_machine = (uint16_t)(elf[18] | (elf[19] << 8));
  if (e_machine != (uint16_t)k_elf_em_arm) {
    (void)fprintf(stderr, "ELF e_machine %u != ARM(40)\n", e_machine);
    return -1;
  }
  uint32_t phoff = 0U;
  (void)memcpy(&phoff, elf + (uint32_t)k_elf_e_phoff_off, 4);
  uint16_t phentsize = (uint16_t)(elf[42] | (elf[43] << 8));
  uint16_t phnum     = (uint16_t)(elf[44] | (elf[45] << 8));
  int      loaded    = 0;
  for (uint16_t i = 0U; i < phnum; i++) {
    const uint8_t* ph = elf + phoff + ((uint32_t)i * phentsize);
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
    const uint8_t* ph       = elf + phoff + ((uint32_t)i * phentsize);
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
    const uint8_t* sh = elf + shoff + ((uint32_t)i * shentsize);
    if (((size_t)(sh - elf) + (size_t)k_elf_shentsize_min) > (size_t)len) {
      break;
    }
    uint32_t sh_type = 0U;
    (void)memcpy(&sh_type, sh + 4, 4);
    if (sh_type != 2U /* SHT_SYMTAB */) {
      continue;
    }
    uint32_t sym_off = 0U, sym_size = 0U, sym_link = 0U, sym_entsize = 0U;
    (void)memcpy(&sym_off, sh + 16, 4);
    (void)memcpy(&sym_size, sh + (uint32_t)k_elf_sh_size_off, 4);
    (void)memcpy(&sym_link, sh + (uint32_t)k_elf_sh_link_off, 4);
    (void)memcpy(&sym_entsize, sh + (uint32_t)k_elf_sh_entsize_off, 4);
    if ((sym_entsize < 16U) || (sym_link >= shnum)) {
      continue;
    }
    const uint8_t* strsh   = elf + shoff + ((uint32_t)sym_link * shentsize);
    uint32_t       str_off = 0U;
    (void)memcpy(&str_off, strsh + 16, 4);
    const uint32_t nsym = sym_size / sym_entsize;
    for (uint32_t s = 0U; s < nsym; s++) {
      const uint8_t* sym = elf + sym_off + (s * sym_entsize);
      if (((size_t)(sym - elf) + 16U) > (size_t)len) {
        break;
      }
      uint32_t st_name = 0U, st_value = 0U, st_size = 0U;
      (void)memcpy(&st_name, sym + 0, 4);
      (void)memcpy(&st_value, sym + 4, 4);
      (void)memcpy(&st_size, sym + 8, 4);
      const size_t pos = (size_t)str_off + (size_t)st_name;
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
  }
  return 0U;
}
