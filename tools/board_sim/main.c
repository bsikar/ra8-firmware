/**
 * @file main.c
 * @brief RA8D2 board emulator (stage 1) -- boot a real .elf on a CPU emulator
 *
 * @details
 * Loads an EK-RA8D2 firmware ELF into an emulated Cortex-M memory map (Unicorn,
 * QEMU's CPU core as a library) and boots it from the vector table, with the
 * RA8D2 peripheral space modelled as logged MMIO. Stage 1 answers the
 * feasibility question: does the M85-built firmware actually execute on an
 * emulated M-profile core, or does it hit Armv8.1-M instructions the core
 * cannot decode?
 *
 * Unicorn 2.x tops out at Cortex-M33 (Armv8-M); the RA8D2 is M85 (Armv8.1-M).
 * If GCC emitted v8.1-M-only opcodes (e.g. low-overhead loops), the invalid-
 * instruction trap below reports exactly where and what.
 *
 *   board_sim <firmware.elf>
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <capstone/capstone.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unicorn/unicorn.h>

/* RA8D2 memory map (EK board) -- from the linker script / HUM R01UH1065EJ. */
typedef struct {
  const char* name;
  uint64_t    base;
  uint64_t    size;
} mem_region_t;

static const mem_region_t k_regions[] = {
  {"ITCM", 0x00000000UL, 0x00010000UL}, /* 64 KiB tightly-coupled code   */
  {"MRAM", 0x02000000UL, 0x00100000UL}, /* 1 MiB code flash + vectors    */
  {"OFS", 0x0300A000UL, 0x00001000UL},  /* option-setting flash          */
  {"DTCM", 0x20000000UL, 0x00010000UL}, /* 64 KiB tightly-coupled data   */
  {"SRAM", 0x22000000UL, 0x00100000UL}, /* 1 MiB on-chip SRAM            */
  {"DATA_FLASH", 0x27000000UL, 0x00004000UL},
  {"SDRAM", 0x68000000UL, 0x04000000UL}, /* 64 MiB external SDRAM         */
  {"PPB", 0xE0000000UL, 0x00100000UL},   /* ARM private peripheral bus    */
};

/* Renesas peripheral space -- modelled as logged MMIO (returns all-ones so
 * "wait for ready bit" polls fall through instead of spinning forever). */
typedef enum : uint64_t {
  k_periph_base = 0x40000000UL,
  k_periph_size = 0x10000000UL, /* 0x40000000-0x4FFFFFFF: all Renesas peripherals */
} periph_map_t;

typedef enum : uint32_t {
  k_run_max_insns  = 20000000U, /**< Instruction budget per run.  */
  k_run_timeout_us = 5000000U,  /**< Wall-clock budget (5 s).     */
  k_mmio_log_max   = 40U,       /**< Distinct MMIO addrs to list. */
} sim_budget_t;

static uint32_t s_mmio_reads;
static uint32_t s_mmio_writes;
static uint64_t s_mmio_seen[k_mmio_log_max];
static uint32_t s_mmio_seen_n;

static void mmio_note(uint64_t addr)
{
  for (uint32_t i = 0U; i < s_mmio_seen_n; i++) {
    if (s_mmio_seen[i] == addr) {
      return;
    }
  }
  if (s_mmio_seen_n < (uint32_t)k_mmio_log_max) {
    s_mmio_seen[s_mmio_seen_n] = addr;
    s_mmio_seen_n++;
  }
}

static uint64_t mmio_read(uc_engine* uc, uint64_t offset, unsigned size, void* user)
{
  (void)uc;
  (void)size;
  (void)user;
  s_mmio_reads++;
  mmio_note(k_periph_base + offset);
  return 0xFFFFFFFFULL; /* satisfy ready-bit polls so boot proceeds */
}

static void mmio_write(uc_engine* uc, uint64_t offset, unsigned size, uint64_t value, void* user)
{
  (void)uc;
  (void)size;
  (void)value;
  (void)user;
  s_mmio_writes++;
  mmio_note(k_periph_base + offset);
}

/** @brief Disassemble + report an instruction the core could not decode. */
static bool on_invalid_insn(uc_engine* uc, void* user)
{
  (void)user;
  uint32_t pc = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_PC, &pc);
  uint8_t code[4] = {0};
  (void)uc_mem_read(uc, pc, code, sizeof(code));
  (void)fprintf(stderr,
                "  INVALID INSN @ 0x%08X: bytes %02X %02X %02X %02X\n",
                pc,
                code[0],
                code[1],
                code[2],
                code[3]);

  csh cs;
  if (cs_open(CS_ARCH_ARM, (cs_mode)(CS_MODE_THUMB | CS_MODE_MCLASS), &cs) == CS_ERR_OK) {
    cs_insn*     insn = nullptr;
    const size_t n    = cs_disasm(cs, code, sizeof(code), pc, 1, &insn);
    if (n > 0U) {
      (void)fprintf(stderr, "  disasm: %s %s\n", insn[0].mnemonic, insn[0].op_str);
      cs_free(insn, n);
    } else {
      (void)fprintf(stderr, "  disasm: capstone could not decode it either\n");
    }
    cs_close(&cs);
  }
  return false; /* not handled -> stop emulation with UC_ERR_INSN_INVALID */
}

/** @brief Hook fired on access to unmapped memory (peripheral surface gap). */
static bool
on_unmapped(uc_engine* uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void* user)
{
  (void)uc;
  (void)size;
  (void)value;
  (void)user;
  (void)fprintf(stderr,
                "  UNMAPPED %s @ 0x%08llX (extend the memory/peripheral map)\n",
                (type == UC_MEM_READ_UNMAPPED)    ? "read"
                : (type == UC_MEM_WRITE_UNMAPPED) ? "write"
                                                  : "fetch",
                (unsigned long long)addr);
  return false; /* stop emulation and report */
}

static uint8_t* read_file(const char* path, long* out_len)
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

/** @brief Load ELF32 PT_LOAD segments into emulated memory at their LMA. */
static int load_elf(uc_engine* uc, const uint8_t* elf, long len)
{
  if ((len < 52) ||
      (memcmp(elf,
              "\x7F"
              "ELF",
              4) != 0) ||
      (elf[4] != 1) /* ELFCLASS32 */) {
    (void)fprintf(stderr, "not a 32-bit ELF\n");
    return -1;
  }
  const uint16_t e_machine = (uint16_t)(elf[18] | (elf[19] << 8));
  if (e_machine != 40 /* EM_ARM */) {
    (void)fprintf(stderr, "ELF e_machine %u != ARM(40)\n", e_machine);
    return -1;
  }
  uint32_t phoff = 0U;
  (void)memcpy(&phoff, elf + 28, 4);
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
    (void)memcpy(&p_paddr, ph + 12, 4); /* load address (LMA) */
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

int main(int argc, char** argv)
{
  if (argc < 2) {
    (void)fprintf(stderr, "usage: board_sim <firmware.elf>\n");
    return 2;
  }
  const char* elf_path = argv[1];

  uc_engine* uc = nullptr;
  if (uc_open(UC_ARCH_ARM, (uc_mode)(UC_MODE_THUMB | UC_MODE_MCLASS), &uc) != UC_ERR_OK) {
    (void)fprintf(stderr, "uc_open failed\n");
    return 1;
  }
  /* Closest emulated core to the M85: M33 (Armv8-M). */
  (void)uc_ctl_set_cpu_model(uc, UC_CPU_ARM_CORTEX_M33);

  for (size_t i = 0U; i < (sizeof(k_regions) / sizeof(k_regions[0])); i++) {
    if (uc_mem_map(uc, k_regions[i].base, (size_t)k_regions[i].size, UC_PROT_ALL) != UC_ERR_OK) {
      (void)fprintf(stderr,
                    "map %s @0x%08llX failed\n",
                    k_regions[i].name,
                    (unsigned long long)k_regions[i].base);
      return 1;
    }
  }
  if (uc_mmio_map(uc,
                  (uint64_t)k_periph_base,
                  (size_t)k_periph_size,
                  mmio_read,
                  nullptr,
                  mmio_write,
                  nullptr) != UC_ERR_OK) {
    (void)fprintf(stderr, "mmio_map failed\n");
    return 1;
  }

  long           elf_len = 0;
  uint8_t* const elf     = read_file(elf_path, &elf_len);
  if (elf == nullptr) {
    (void)fprintf(stderr, "cannot read %s\n", elf_path);
    return 1;
  }
  (void)fprintf(stderr, "board_sim: loading %s (%ld bytes)\n", elf_path, elf_len);
  if (load_elf(uc, elf, elf_len) != 0) {
    free(elf);
    return 1;
  }
  free(elf);

  /* Cortex-M reset: SP = vectors[0], PC = vectors[1] (Thumb, clear bit0). */
  uint32_t sp = 0U;
  uint32_t pc = 0U;
  (void)uc_mem_read(uc, k_regions[1].base + 0U, &sp, 4); /* MRAM[0] */
  (void)uc_mem_read(uc, k_regions[1].base + 4U, &pc, 4); /* MRAM[4] (Thumb: bit0=1) */
  /* M-profile is always Thumb (EPSR.T must be 1). Keep the reset vector's bit0
   * and set the xPSR Thumb bit so Unicorn enters Thumb, not ARM, decoding. */
  pc |= 1U;
  uint32_t xpsr = (uint32_t)(1UL << 24); /* xPSR.T */
  (void)uc_reg_write(uc, UC_ARM_REG_SP, &sp);
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &pc);
  (void)uc_reg_write(uc, UC_ARM_REG_XPSR, &xpsr);
  (void)fprintf(stderr,
                "board_sim: reset SP=0x%08X PC=0x%08X -- running (<= %u insns / %u us)\n",
                sp,
                pc,
                (unsigned)k_run_max_insns,
                (unsigned)k_run_timeout_us);

  uc_hook h_invalid;
  uc_hook h_unmapped;
  (void)uc_hook_add(uc, &h_invalid, UC_HOOK_INSN_INVALID, (void*)on_invalid_insn, nullptr, 1, 0);
  (void)uc_hook_add(uc, &h_unmapped, UC_HOOK_MEM_UNMAPPED, (void*)on_unmapped, nullptr, 1, 0);

  const uc_err err = uc_emu_start(uc, pc, 0, (uint64_t)k_run_timeout_us, (size_t)k_run_max_insns);

  uint32_t final_pc = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_PC, &final_pc);
  (void)fprintf(stderr, "\nboard_sim: stopped -- %s\n", uc_strerror(err));
  (void)fprintf(stderr, "  final PC   : 0x%08X\n", final_pc);
  (void)fprintf(stderr,
                "  MMIO reads : %u   writes: %u   distinct addrs: %u\n",
                s_mmio_reads,
                s_mmio_writes,
                s_mmio_seen_n);
  for (uint32_t i = 0U; i < s_mmio_seen_n; i++) {
    (void)fprintf(stderr, "    0x%08llX\n", (unsigned long long)s_mmio_seen[i]);
  }
  if (err == UC_ERR_OK) {
    (void)fprintf(stderr,
                  "  => firmware EXECUTED to the instruction/time budget (no invalid opcode).\n");
  }
  (void)uc_close(uc);
  return 0;
}
