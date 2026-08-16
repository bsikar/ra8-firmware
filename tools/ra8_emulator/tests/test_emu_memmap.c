/**
 * @file test_emu_memmap.c
 * @brief Focused shared-aperture memory and Unicorn-view contract tests
 * @details Exercises the observable contract of the aliased memory backing:
 * exact geometry, lazy commitment, Secure/Non-secure and cross-engine
 * coherence, execution from a shared aperture, late-attach visibility, binding
 * lifetime, and faulting exactly where the silicon faults.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#include "emu_memmap.h"

/** @brief Test-only addresses, canaries, and Thumb instructions. */
typedef enum : uint64_t {
  /** @brief SRAM instruction address. */
  k_test_sram_code = 0x22000100U,
  /** @brief Secure SRAM data address. */
  k_test_sram_data = 0x22000200U,
  /** @brief NS alias of SRAM offset 0x100200. */
  k_test_ns_sram_data = 0x32100200U,
  /** @brief Physical SRAM2 backing address. */
  k_test_sram2_data = 0x22100200U,
  /** @brief Secure SDRAM data address. */
  k_test_sdram_data = 0x68000200U,
  /** @brief Non-secure SDRAM alias. */
  k_test_ns_sdram_data = 0x78000200U,
  /** @brief Secure OSPI instruction address. */
  k_test_ospi_code = 0x80000100U,
  /** @brief Non-secure OSPI alias. */
  k_test_ns_ospi_code = 0x90000100U,
  /** @brief The phantom data-flash window #397 removed; must stay unmapped. */
  k_test_phantom_flash = 0x27000000U,
  /** @brief Top-of-space probe; no host bookkeeping may live in the guest map. */
  k_test_top_of_space = 0xFFFFF000U,
  /** @brief Exact three-aperture logical total. */
  k_test_logical_bytes = 136134656U,
  /** @brief Thumb `str r0, [r1]`. */
  k_test_thumb_str_r0r1 = 0x6008U,
  /** @brief Thumb `strb r0, [r1]`. */
  k_test_thumb_strb_r0r1 = 0x7008U,
  /** @brief Thumb `strh r0, [r1]`. */
  k_test_thumb_strh_r0r1 = 0x8008U,
  /** @brief Thumb `ldr r0, [r1]`. */
  k_test_thumb_ldr_r0r1 = 0x6808U,
} test_constant_t;

/** @brief Aperture indices inside a workspace, mirrored from the region order. */
typedef enum : size_t {
  k_test_backing_sram  = 0U, /**< On-chip SRAM aperture.   */
  k_test_backing_sdram = 1U, /**< External SDRAM aperture. */
  k_test_backing_ospi  = 2U, /**< OSPI XIP aperture.       */
} test_backing_t;

/** @brief Sizes used by the multi-page and residency probes. */
typedef enum : size_t {
  k_test_sdram_pages    = 16384U, /**< 64 MiB SDRAM aperture, in 4 KiB pages. */
  k_test_multipage_size = 8192U,  /**< Two-page host transfer under test.     */
} test_size_t;

/** @brief Page-residency vector large enough for the widest aperture. */
static unsigned char s_test_residency[k_test_sdram_pages];

/** @brief Payload for the multi-page host transfer probe. */
static uint8_t s_test_multipage[k_test_multipage_size];

/** @brief Return the failing source line without process output. */
#define TEST_CHECK(condition)                                                                      \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      return __LINE__;                                                                             \
    }                                                                                              \
  } while (false)

/** @brief Peripheral read stub required by emu_memmap attachment. */
uint64_t mmio_read(uc_engine* uc, uint64_t offset, unsigned size, void* user)
{
  (void)uc;
  (void)offset;
  (void)size;
  (void)user;
  return 0U;
}

/** @brief Peripheral write stub required by emu_memmap attachment. */
void mmio_write(uc_engine* uc, uint64_t offset, unsigned size, uint64_t value, void* user)
{
  (void)uc;
  (void)offset;
  (void)size;
  (void)value;
  (void)user;
}

/**
 * @brief Count how many pages of one aperture are resident in host memory.
 * @details Asks the kernel, through mincore, which pages of the aperture have
 * a present mapping. A freshly acquired anonymous aperture has none, which is
 * how the test proves the emulator does not pay 130 MiB of host memory for
 * 130 MiB of logical guest memory.
 * @param[in] backing Aperture whose residency is measured.
 * @param[in] pages Number of leading pages to measure.
 * @return The resident page count, or the page count plus one on error.
 * @retval value The operation-specific resident page count.
 * @pre @p backing holds a live page-aligned host mapping.
 * @pre @p pages does not exceed the aperture length or the vector capacity.
 * @post No aperture, workspace, or engine state changes.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t internal_test_resident_pages(const emu_memmap_backing_t* backing,
                                                        size_t                      pages)
{
  (void)memset(s_test_residency, 0, sizeof(s_test_residency));
  if (mincore(backing->host, pages * (size_t)k_page_size, s_test_residency) != 0) {
    return pages + 1U;
  }
  size_t resident = 0U;
  for (size_t index = 0U; index < pages; index++) {
    if ((s_test_residency[index] & 1U) != 0U) {
      resident++;
    }
  }
  return resident;
}

/**
 * @brief Open and configure one Cortex-M engine.
 * @details Open and configure one Cortex-M engine; this step is contained
 * within the emu memmap test fixture and uses bounded caller or module-owned
 * storage.
 * @param[in,out] engine Engine state or storage updated in place by the operation.
 * @return The test engine open result produced by the emu memmap test fixture.
 * @retval true The test engine open condition holds or completed successfully; false otherwise.
 * @pre @p engine is non-null and holds no live engine.
 * @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to @p engine.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_test_engine_open(uc_engine** engine)
{
  if (uc_open(UC_ARCH_ARM, (uc_mode)(UC_MODE_THUMB | UC_MODE_MCLASS), engine) != UC_ERR_OK) {
    return false;
  }
  return uc_ctl_set_cpu_model(*engine, UC_CPU_ARM_CORTEX_M33) == UC_ERR_OK;
}

/**
 * @brief Read one word directly from a Unicorn-owned view.
 * @details Read one word directly from a Unicorn-owned view; this step is
 * contained within the emu memmap test fixture and uses bounded caller or
 * module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read.
 * @param[in] address Guest address involved in the operation.
 * @return The test view word result produced by the emu memmap test fixture.
 * @retval value The operation-specific test view word value.
 * @pre @p uc is a live engine with @p address mapped.
 * @pre The call executes on the emulator's single owning thread.
 * @post No engine or aperture state changes.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_test_view_word(uc_engine* uc, uint64_t address)
{
  uint32_t value = 0U;
  (void)uc_mem_read(uc, address, &value, sizeof(value));
  return value;
}

/**
 * @brief Read one word directly from the authoritative host aperture.
 * @details Read one word directly from the authoritative host aperture; this
 * step is contained within the emu memmap test fixture and uses bounded caller
 * or module-owned storage.
 * @param[in] workspace Caller-owned workspace used by the operation.
 * @param[in] backing Aperture holding the authoritative bytes.
 * @param[in] offset Byte offset at which the word starts.
 * @return The test backing word result produced by the emu memmap test fixture.
 * @retval value The operation-specific test backing word value.
 * @pre @p workspace is open and @p backing is a valid aperture index.
 * @pre @p offset plus four bytes lies inside the aperture.
 * @post No aperture, workspace, or engine state changes.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t
internal_test_backing_word(const emu_memmap_workspace_t* workspace, size_t backing, size_t offset)
{
  uint32_t value = 0U;
  (void)memcpy(&value, &workspace->backings[backing].host[offset], sizeof(value));
  return value;
}

/**
 * @brief Run one previously installed Thumb store instruction.
 * @details Run one previously installed Thumb store instruction; this step is
 * contained within the emu memmap test fixture and uses bounded caller or
 * module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is updated.
 * @param[in] code Guest address the instruction was installed at.
 * @param[in] target Guest address the store writes to.
 * @param[in] value Register payload the store writes.
 * @return The test run store result produced by the emu memmap test fixture.
 * @retval value The operation-specific test run store value.
 * @pre The instruction at @p code was installed before this call.
 * @pre The call executes on the emulator's single owning thread.
 * @post Success leaves the stored value in every view of @p target.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uc_err
internal_test_run_store(uc_engine* uc, uint64_t code, uint64_t target, uint32_t value)
{
  const uint32_t address = (uint32_t)target;
  (void)uc_reg_write(uc, UC_ARM_REG_R0, &value);
  (void)uc_reg_write(uc, UC_ARM_REG_R1, &address);
  return uc_emu_start(uc, code | 1U, code + sizeof(uint16_t), 0U, 1U);
}

/**
 * @brief Install and execute one Thumb store from the mapped code address.
 * @details Install and execute one Thumb store from the mapped code address;
 * this step is contained within the emu memmap test fixture and uses bounded
 * caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is updated.
 * @param[in] code Guest address the instruction is installed at.
 * @param[in] target Guest address the store writes to.
 * @param[in] value Register payload the store writes.
 * @return The test guest store result produced by the emu memmap test fixture.
 * @retval value The operation-specific test guest store value.
 * @pre @p code lies in an executable mapped region.
 * @pre The call executes on the emulator's single owning thread.
 * @post Success leaves the stored value in every view of @p target.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uc_err
internal_test_guest_store(uc_engine* uc, uint64_t code, uint64_t target, uint32_t value)
{
  const uint16_t instruction = (uint16_t)k_test_thumb_str_r0r1;
  if (emu_mem_write(uc, code, &instruction, sizeof(instruction)) != UC_ERR_OK) {
    return UC_ERR_WRITE_UNMAPPED;
  }
  return internal_test_run_store(uc, code, target, value);
}

/**
 * @brief Prove the exact geometry and that apertures commit lazily.
 * @details The apertures span 130 MiB of logical guest memory. This proves the
 * emulator does not pay that in host memory: nothing is resident after the open
 * or after an engine attaches, and exactly the one page a write lands on
 * becomes resident.
 * @return The test geometry and lazy backing result produced by the fixture.
 * @retval value Zero on success, else the failing source line.
 * @pre No workspace is open in this process.
 * @pre The call executes on the emulator's single owning thread.
 * @post Every workspace and engine opened here is released again.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_test_geometry_and_lazy_backing(void)
{
  const emu_memmap_result_t requirements = emu_memmap_requirements();
  TEST_CHECK(requirements.status == k_emu_memmap_ok);
  TEST_CHECK(requirements.logical_backing_bytes == k_test_logical_bytes);
  TEST_CHECK(emu_memmap_open(nullptr).status == k_emu_memmap_invalid);

  emu_memmap_workspace_t workspace = {};
  TEST_CHECK(emu_memmap_open(&workspace).status == k_emu_memmap_ok);
  uint64_t total = 0U;
  for (size_t index = 0U; index < k_emu_memmap_backing_count; index++) {
    TEST_CHECK(workspace.backings[index].host != nullptr);
    TEST_CHECK((((uintptr_t)workspace.backings[index].host) % (uintptr_t)k_page_size) == 0U);
    total += workspace.backings[index].size;
  }
  TEST_CHECK(total == k_test_logical_bytes);
  TEST_CHECK(internal_test_resident_pages(&workspace.backings[k_test_backing_sdram],
                                          k_test_sdram_pages) == 0U);

  uc_engine* uc = nullptr;
  TEST_CHECK(internal_test_engine_open(&uc));
  TEST_CHECK(emu_memmap_attach(&workspace, uc).status == k_emu_memmap_ok);
  TEST_CHECK(internal_test_resident_pages(&workspace.backings[k_test_backing_sdram],
                                          k_test_sdram_pages) == 0U);

  const uint32_t value = 0x0BADF00DU;
  TEST_CHECK(emu_mem_write(uc, k_test_sdram_data, &value, sizeof(value)) == UC_ERR_OK);
  TEST_CHECK(internal_test_resident_pages(&workspace.backings[k_test_backing_sdram],
                                          k_test_sdram_pages) == 1U);

  TEST_CHECK(emu_memmap_detach(&workspace, uc));
  TEST_CHECK(uc_close(uc) == UC_ERR_OK);
  TEST_CHECK(emu_memmap_close(&workspace));
  TEST_CHECK(emu_memmap_close(&workspace));
  TEST_CHECK(workspace.backings[k_test_backing_sdram].host == nullptr);
  return 0;
}

/**
 * @brief Prove aliases, CPU views, execution, and binding lifetime.
 * @details The Secure aperture, its IDAU bit[28] Non-secure alias, and every
 * attached engine are one state, for host writes and for guest stores alike,
 * including instruction fetch from a shared aperture.
 * @return The test alias execution and lifetime result produced by the fixture.
 * @retval value Zero on success, else the failing source line.
 * @pre No workspace is open in this process.
 * @pre The call executes on the emulator's single owning thread.
 * @post Every workspace and engine opened here is released again.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_test_alias_execution_and_lifetime(void)
{
  emu_memmap_workspace_t workspace = {};
  TEST_CHECK(emu_memmap_open(&workspace).status == k_emu_memmap_ok);
  uc_engine* cpu0 = nullptr;
  uc_engine* cpu1 = nullptr;
  TEST_CHECK(internal_test_engine_open(&cpu0) && internal_test_engine_open(&cpu1));
  TEST_CHECK(emu_memmap_attach(&workspace, cpu0).status == k_emu_memmap_ok);
  TEST_CHECK(emu_memmap_attach(&workspace, cpu1).status == k_emu_memmap_ok);

  const uint32_t secure = 0x11223344U;
  TEST_CHECK(emu_mem_write(cpu0, k_test_sram2_data, &secure, sizeof(secure)) == UC_ERR_OK);
  TEST_CHECK(internal_test_view_word(cpu0, k_test_ns_sram_data) == secure);
  TEST_CHECK(internal_test_view_word(cpu1, k_test_sram2_data) == secure);
  TEST_CHECK(internal_test_backing_word(&workspace, k_test_backing_sram, 0x00100200U) == secure);

  const uint32_t sdram = 0x55667788U;
  TEST_CHECK(emu_mem_write(cpu1, k_test_ns_sdram_data, &sdram, sizeof(sdram)) == UC_ERR_OK);
  TEST_CHECK(internal_test_view_word(cpu0, k_test_sdram_data) == sdram);
  TEST_CHECK(internal_test_view_word(cpu1, k_test_ns_sdram_data) == sdram);
  TEST_CHECK(internal_test_backing_word(&workspace, k_test_backing_sdram, 0x200U) == sdram);

  TEST_CHECK(internal_test_guest_store(cpu0, k_test_sram_code, k_test_sdram_data, 0xAABBCCDDU) ==
             UC_ERR_OK);
  TEST_CHECK(internal_test_view_word(cpu1, k_test_ns_sdram_data) == 0xAABBCCDDU);
  TEST_CHECK(internal_test_guest_store(cpu1, k_test_ospi_code, k_test_sram_data, 0xC001CAFEU) ==
             UC_ERR_OK);
  TEST_CHECK(internal_test_view_word(cpu0, k_test_sram_data) == 0xC001CAFEU);
  TEST_CHECK(internal_test_view_word(cpu1, k_test_ns_ospi_code) == k_test_thumb_str_r0r1);

  /* A live binding holds uc_mem_map_ptr references into the aperture pages, so
   * closing under one would strand that engine on unmapped host memory. */
  TEST_CHECK(!emu_memmap_close(&workspace));
  TEST_CHECK(emu_memmap_detach(&workspace, cpu1));
  TEST_CHECK(uc_close(cpu1) == UC_ERR_OK);
  TEST_CHECK(!emu_memmap_close(&workspace));
  TEST_CHECK(emu_memmap_detach(&workspace, cpu0));
  TEST_CHECK(!emu_memmap_detach(&workspace, cpu0));
  TEST_CHECK(uc_close(cpu0) == UC_ERR_OK);
  TEST_CHECK(emu_memmap_close(&workspace));
  return 0;
}

/**
 * @brief Prove two simultaneous backing instances do not share bytes.
 * @details Two workspaces own distinct apertures, so a write through one is
 * invisible to the other even at the identical guest address.
 * @return The test independent instances result produced by the fixture.
 * @retval value Zero on success, else the failing source line.
 * @pre No workspace is open in this process.
 * @pre The call executes on the emulator's single owning thread.
 * @post Every workspace and engine opened here is released again.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_test_independent_instances(void)
{
  emu_memmap_workspace_t first  = {};
  emu_memmap_workspace_t second = {};
  TEST_CHECK(emu_memmap_open(&first).status == k_emu_memmap_ok);
  TEST_CHECK(emu_memmap_open(&second).status == k_emu_memmap_ok);
  uc_engine* first_uc  = nullptr;
  uc_engine* second_uc = nullptr;
  TEST_CHECK(internal_test_engine_open(&first_uc) && internal_test_engine_open(&second_uc));
  TEST_CHECK(emu_memmap_attach(&first, first_uc).status == k_emu_memmap_ok);
  TEST_CHECK(emu_memmap_attach(&second, second_uc).status == k_emu_memmap_ok);
  const uint32_t first_value  = 0x11111111U;
  const uint32_t second_value = 0x22222222U;
  TEST_CHECK(emu_mem_write(first_uc, k_test_sdram_data, &first_value, 4U) == UC_ERR_OK);
  TEST_CHECK(emu_mem_write(second_uc, k_test_sdram_data, &second_value, 4U) == UC_ERR_OK);
  TEST_CHECK(internal_test_view_word(first_uc, k_test_sdram_data) == first_value);
  TEST_CHECK(internal_test_view_word(second_uc, k_test_sdram_data) == second_value);
  TEST_CHECK(first.backings[k_test_backing_sdram].host !=
             second.backings[k_test_backing_sdram].host);
  TEST_CHECK(emu_memmap_detach(&first, first_uc) && emu_memmap_detach(&second, second_uc));
  TEST_CHECK((uc_close(first_uc) == UC_ERR_OK) && (uc_close(second_uc) == UC_ERR_OK));
  TEST_CHECK(emu_memmap_close(&first) && emu_memmap_close(&second));
  return 0;
}

/**
 * @brief Prove every store width and an unbounded host transfer stay coherent.
 * @details Byte, halfword and word guest stores land at the exact aperture
 * offsets, and a host transfer spanning two pages is published whole to every
 * view -- there is no per-transfer size ceiling.
 * @return The test guest widths and multipage result produced by the fixture.
 * @retval value Zero on success, else the failing source line.
 * @pre No workspace is open in this process.
 * @pre The call executes on the emulator's single owning thread.
 * @post Every workspace and engine opened here is released again.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_test_guest_widths_and_multipage(void)
{
  emu_memmap_workspace_t workspace = {};
  TEST_CHECK(emu_memmap_open(&workspace).status == k_emu_memmap_ok);
  uc_engine* uc = nullptr;
  TEST_CHECK(internal_test_engine_open(&uc));
  TEST_CHECK(emu_memmap_attach(&workspace, uc).status == k_emu_memmap_ok);

  const uint16_t stores[]  = {(uint16_t)k_test_thumb_strb_r0r1,
                              (uint16_t)k_test_thumb_strh_r0r1,
                              (uint16_t)k_test_thumb_str_r0r1};
  const uint32_t values[]  = {0x000000A5U, 0x0000BEEFU, 0xC001CAFEU};
  const uint64_t targets[] = {k_test_sdram_data, k_test_sdram_data + 2U, k_test_sdram_data + 4U};
  for (size_t index = 0U; index < 3U; index++) {
    const uint64_t code = k_test_sram_code + (index * sizeof(uint16_t));
    TEST_CHECK(emu_mem_write(uc, code, &stores[index], sizeof(stores[index])) == UC_ERR_OK);
    TEST_CHECK(internal_test_run_store(uc, code, targets[index], values[index]) == UC_ERR_OK);
  }
  const uint8_t* const committed = &workspace.backings[k_test_backing_sdram].host[0x200];
  TEST_CHECK((committed[0] == 0xA5U) && (committed[1] == 0U));
  TEST_CHECK((committed[2] == 0xEFU) && (committed[3] == 0xBEU));
  TEST_CHECK(internal_test_backing_word(&workspace, k_test_backing_sdram, 0x204U) == 0xC001CAFEU);

  (void)memset(s_test_multipage, 0xCC, sizeof(s_test_multipage));
  TEST_CHECK(emu_mem_write(uc, k_test_sdram_data, s_test_multipage, sizeof(s_test_multipage)) ==
             UC_ERR_OK);
  TEST_CHECK(internal_test_view_word(uc, k_test_sdram_data) == 0xCCCCCCCCU);
  TEST_CHECK(internal_test_view_word(uc, k_test_ns_sdram_data + sizeof(s_test_multipage) - 4U) ==
             0xCCCCCCCCU);
  TEST_CHECK(internal_test_backing_word(&workspace,
                                        k_test_backing_sdram,
                                        0x200U + sizeof(s_test_multipage) - 4U) == 0xCCCCCCCCU);

  TEST_CHECK(emu_mem_write(uc, k_test_sdram_data, nullptr, 4U) == UC_ERR_ARG);
  TEST_CHECK(emu_mem_write(uc, k_test_sdram_data, s_test_multipage, 0U) == UC_ERR_ARG);
  TEST_CHECK(emu_memmap_detach(&workspace, uc));
  TEST_CHECK(uc_close(uc) == UC_ERR_OK);
  TEST_CHECK(emu_memmap_close(&workspace));
  return 0;
}

/**
 * @brief Prove the guest map contains only windows the silicon decodes.
 * @details The phantom 0x27000000 data-flash window #397 removed must fault
 * exactly as a J-Link write to it does on an EK-RA8D2, and no host bookkeeping
 * page may be reachable from firmware.
 * @return The test unmapped windows result produced by the fixture.
 * @retval value Zero on success, else the failing source line.
 * @pre No workspace is open in this process.
 * @pre The call executes on the emulator's single owning thread.
 * @post Every workspace and engine opened here is released again.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_test_unmapped_windows(void)
{
  emu_memmap_workspace_t workspace = {};
  TEST_CHECK(emu_memmap_open(&workspace).status == k_emu_memmap_ok);
  uc_engine* uc = nullptr;
  TEST_CHECK(internal_test_engine_open(&uc));
  TEST_CHECK(emu_memmap_attach(&workspace, uc).status == k_emu_memmap_ok);

  const uint16_t load = (uint16_t)k_test_thumb_ldr_r0r1;
  TEST_CHECK(emu_mem_write(uc, k_test_sram_code, &load, sizeof(load)) == UC_ERR_OK);
  const uint64_t probes[] = {k_test_top_of_space, k_test_phantom_flash};
  for (size_t index = 0U; index < 2U; index++) {
    const uint32_t target = (uint32_t)probes[index];
    (void)uc_reg_write(uc, UC_ARM_REG_R1, &target);
    TEST_CHECK(uc_emu_start(uc, k_test_sram_code | 1U, k_test_sram_code + 2U, 0U, 1U) != UC_ERR_OK);
    uint32_t word = 0U;
    TEST_CHECK(emu_mem_read(uc, probes[index], &word, sizeof(word)) != UC_ERR_OK);
    TEST_CHECK(emu_mem_write(uc, probes[index], &word, sizeof(word)) != UC_ERR_OK);
  }
  TEST_CHECK(emu_memmap_detach(&workspace, uc));
  TEST_CHECK(uc_close(uc) == UC_ERR_OK);
  TEST_CHECK(emu_memmap_close(&workspace));
  return 0;
}

/**
 * @brief Prove an engine attached late sees every earlier aperture write.
 * @details A second engine binds the same host pages, so it observes SRAM,
 * SDRAM and OSPI bytes written before it existed, through both the Secure and
 * the Non-secure view, with no replay step to get wrong.
 * @return The test late attach coherence result produced by the fixture.
 * @retval value Zero on success, else the failing source line.
 * @pre No workspace is open in this process.
 * @pre The call executes on the emulator's single owning thread.
 * @post Every workspace and engine opened here is released again.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_test_late_attach_coherence(void)
{
  emu_memmap_workspace_t workspace = {};
  TEST_CHECK(emu_memmap_open(&workspace).status == k_emu_memmap_ok);
  uc_engine* cpu0 = nullptr;
  TEST_CHECK(internal_test_engine_open(&cpu0));
  TEST_CHECK(emu_memmap_attach(&workspace, cpu0).status == k_emu_memmap_ok);

  const uint32_t sram_value  = 0x31415926U;
  const uint32_t sdram_value = 0x27182818U;
  const uint32_t ospi_value  = 0x16180339U;
  TEST_CHECK(emu_mem_write(cpu0, k_test_sram2_data, &sram_value, 4U) == UC_ERR_OK);
  TEST_CHECK(emu_mem_write(cpu0, k_test_sdram_data, &sdram_value, 4U) == UC_ERR_OK);
  TEST_CHECK(emu_mem_write(cpu0, k_test_ospi_code, &ospi_value, 4U) == UC_ERR_OK);

  uc_engine* cpu1 = nullptr;
  TEST_CHECK(internal_test_engine_open(&cpu1));
  TEST_CHECK(emu_memmap_attach(&workspace, cpu1).status == k_emu_memmap_ok);
  TEST_CHECK(internal_test_view_word(cpu1, k_test_sram2_data) == sram_value);
  TEST_CHECK(internal_test_view_word(cpu1, k_test_ns_sram_data) == sram_value);
  TEST_CHECK(internal_test_view_word(cpu1, k_test_sdram_data) == sdram_value);
  TEST_CHECK(internal_test_view_word(cpu1, k_test_ns_sdram_data) == sdram_value);
  TEST_CHECK(internal_test_view_word(cpu1, k_test_ospi_code) == ospi_value);
  TEST_CHECK(internal_test_view_word(cpu1, k_test_ns_ospi_code) == ospi_value);

  /* A third engine has no binding slot; the refusal must not disturb the two
   * that are live. */
  uc_engine* extra = nullptr;
  TEST_CHECK(internal_test_engine_open(&extra));
  TEST_CHECK(emu_memmap_attach(&workspace, extra).status != k_emu_memmap_ok);
  TEST_CHECK(uc_close(extra) == UC_ERR_OK);
  TEST_CHECK(internal_test_view_word(cpu0, k_test_sdram_data) == sdram_value);

  TEST_CHECK(emu_memmap_detach(&workspace, cpu1) && emu_memmap_detach(&workspace, cpu0));
  TEST_CHECK((uc_close(cpu1) == UC_ERR_OK) && (uc_close(cpu0) == UC_ERR_OK));
  TEST_CHECK(emu_memmap_close(&workspace));
  return 0;
}

/**
 * @brief Prove a fresh workspace starts blank after an earlier one is closed.
 * @details Closing releases the apertures, so the next workspace cannot inherit
 * the previous run's bytes -- the property a persistent authoritative store
 * would silently break.
 * @return The test backing lifetime result produced by the fixture.
 * @retval value Zero on success, else the failing source line.
 * @pre No workspace is open in this process.
 * @pre The call executes on the emulator's single owning thread.
 * @post Every workspace and engine opened here is released again.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_test_backing_lifetime(void)
{
  emu_memmap_workspace_t workspace = {};
  TEST_CHECK(emu_memmap_open(&workspace).status == k_emu_memmap_ok);
  uc_engine* uc = nullptr;
  TEST_CHECK(internal_test_engine_open(&uc));
  TEST_CHECK(emu_memmap_attach(&workspace, uc).status == k_emu_memmap_ok);
  const uint32_t value = 0x5A5AA5A5U;
  TEST_CHECK(emu_mem_write(uc, k_test_sdram_data, &value, 4U) == UC_ERR_OK);
  TEST_CHECK(emu_mem_write(uc, k_test_sram2_data, &value, 4U) == UC_ERR_OK);
  TEST_CHECK(emu_memmap_detach(&workspace, uc));
  TEST_CHECK(uc_close(uc) == UC_ERR_OK);
  TEST_CHECK(emu_memmap_close(&workspace));

  TEST_CHECK(emu_memmap_open(&workspace).status == k_emu_memmap_ok);
  TEST_CHECK(internal_test_engine_open(&uc));
  TEST_CHECK(emu_memmap_attach(&workspace, uc).status == k_emu_memmap_ok);
  TEST_CHECK(internal_test_view_word(uc, k_test_sdram_data) == 0U);
  TEST_CHECK(internal_test_view_word(uc, k_test_sram2_data) == 0U);
  TEST_CHECK(internal_test_view_word(uc, k_test_ns_sram_data) == 0U);
  TEST_CHECK(emu_memmap_detach(&workspace, uc));
  TEST_CHECK(uc_close(uc) == UC_ERR_OK);
  TEST_CHECK(emu_memmap_close(&workspace));
  return 0;
}

/**
 * @brief Prove a faulting guest store changes nothing and leaves a usable engine.
 * @details A store to the unmapped phantom flash faults, no aperture byte
 * moves, and the same engine still completes a legitimate store afterwards.
 * @return The test guest store faults result produced by the fixture.
 * @retval value Zero on success, else the failing source line.
 * @pre No workspace is open in this process.
 * @pre The call executes on the emulator's single owning thread.
 * @post Every workspace and engine opened here is released again.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_test_guest_store_faults(void)
{
  emu_memmap_workspace_t workspace = {};
  TEST_CHECK(emu_memmap_open(&workspace).status == k_emu_memmap_ok);
  uc_engine* uc = nullptr;
  TEST_CHECK(internal_test_engine_open(&uc));
  TEST_CHECK(emu_memmap_attach(&workspace, uc).status == k_emu_memmap_ok);
  const uint32_t settled = 0x13579BDFU;
  TEST_CHECK(emu_mem_write(uc, k_test_sram_data, &settled, 4U) == UC_ERR_OK);

  TEST_CHECK(internal_test_guest_store(uc, k_test_sram_code, k_test_phantom_flash, 0x2468ACE0U) ==
             UC_ERR_WRITE_UNMAPPED);
  TEST_CHECK(internal_test_backing_word(&workspace, k_test_backing_sram, 0x200U) == settled);
  TEST_CHECK(internal_test_view_word(uc, k_test_sram_data) == settled);

  TEST_CHECK(internal_test_run_store(uc, k_test_sram_code, k_test_sdram_data, 0x2468ACE0U) ==
             UC_ERR_OK);
  TEST_CHECK(internal_test_backing_word(&workspace, k_test_backing_sdram, 0x200U) == 0x2468ACE0U);
  TEST_CHECK(emu_memmap_detach(&workspace, uc));
  TEST_CHECK(uc_close(uc) == UC_ERR_OK);
  TEST_CHECK(emu_memmap_close(&workspace));
  return 0;
}

/** @brief Run the complete shared-aperture memory contract matrix. */
int main(void)
{
  int result = internal_test_geometry_and_lazy_backing();
  if (result == 0) {
    result = internal_test_alias_execution_and_lifetime();
  }
  if (result == 0) {
    result = internal_test_independent_instances();
  }
  if (result == 0) {
    result = internal_test_guest_widths_and_multipage();
  }
  if (result == 0) {
    result = internal_test_unmapped_windows();
  }
  if (result == 0) {
    result = internal_test_late_attach_coherence();
  }
  if (result == 0) {
    result = internal_test_backing_lifetime();
  }
  if (result == 0) {
    result = internal_test_guest_store_faults();
  }
  return result;
}
