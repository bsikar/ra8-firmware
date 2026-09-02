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
  k_test_sdram_pages     = 16384U, /**< 64 MiB SDRAM aperture, in 4 KiB pages. */
  k_test_multipage_size  = 8192U,  /**< Two-page host transfer under test.     */
  k_test_bits_per_byte   = 8U,     /**< Shift step for little-endian assembly. */
  k_test_backing_word_sz = 4U,     /**< Width of the words the probes compare. */
} test_size_t;

/** @brief Return the failing source line without process output. */
#define TEST_CHECK(condition)                                                                      \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      return __LINE__;                                                                             \
    }                                                                                              \
  } while (false)

/** @brief Propagate a nested step's failing source line unchanged. */
#define TEST_STEP(call)                                                                            \
  do {                                                                                             \
    const int step_line = (call);                                                                  \
    if (step_line != 0) {                                                                          \
      return step_line;                                                                            \
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
  /** @brief Page-residency vector large enough for the widest aperture. */
  static unsigned char s_residency[k_test_sdram_pages];
  (void)memset(s_residency, 0, sizeof(s_residency));
  /* mincore rejects a misaligned address, so its success is also the proof
   * that the aperture is page-aligned -- which uc_mem_map_ptr requires. */
#ifdef __APPLE__
  char* residency = (char*)&s_residency[0];
#else
  unsigned char* residency = &s_residency[0];
#endif
  if (mincore(backing->host, pages * (size_t)k_page_size, residency) != 0) {
    return pages + 1U;
  }
  size_t resident = 0U;
  for (size_t index = 0U; index < pages; index++) {
    if ((s_residency[index] & 1U) != 0U) {
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
  const emu_memmap_backing_t* const aperture = &workspace->backings[backing];
  uint32_t                          value    = 0U;
  for (size_t index = 0U; index < (size_t)k_test_backing_word_sz; index++) {
    const size_t byte_offset = offset + index;
    value |=
      (uint32_t)((uint32_t)aperture->host[byte_offset] << (index * (size_t)k_test_bits_per_byte));
  }
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
 * @brief Open one workspace, one engine, and bind them together.
 * @details Every scenario below needs the same three-step bring-up, so it lives here once instead of being repeated eight times. @param[out] workspace Workspace opened by this call. @param[out] uc Engine opened and attached by this call. @return Zero on success, else the failing source line. @retval 0 The workspace is open and the engine is attached.
 * @pre No workspace is open on @p workspace. @pre @p uc addresses a writable engine slot. @post Success leaves one workspace and one engine the caller must release. @post Failure leaves nothing for the caller to release.
 * @note The operation is synchronous and does not transfer heap ownership. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_bind(emu_memmap_workspace_t* workspace, uc_engine** uc)
{
  TEST_CHECK(emu_memmap_open(workspace).status == k_emu_memmap_ok);
  TEST_CHECK(internal_test_engine_open(uc));
  TEST_CHECK(emu_memmap_attach(workspace, *uc).status == k_emu_memmap_ok);
  return 0;
}

/**
 * @brief Detach the engine, close it, and release the workspace.
 * @details The mirror of ::internal_test_bind, in the order the module requires: a workspace cannot close while an engine still holds its pages. @param[in,out] workspace Workspace to release. @param[in,out] uc Engine to detach and close. @return Zero on success, else the failing source line. @retval 0 Both the engine and the workspace were released.
 * @pre @p uc is attached to @p workspace. @pre No other engine is bound to @p workspace. @post No host mapping or engine from this binding remains. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_unbind(emu_memmap_workspace_t* workspace, uc_engine* uc)
{
  TEST_CHECK(emu_memmap_detach(workspace, uc));
  TEST_CHECK(uc_close(uc) == UC_ERR_OK);
  TEST_CHECK(emu_memmap_close(workspace));
  return 0;
}

/**
 * @brief Prove every aperture has a host mapping and the total is exact.
 * @details Walks the region table so a new aperture is covered the day it is added, rather than being measured against a hand-kept count. @param[in] workspace Open workspace whose apertures are measured. @return Zero on success, else the failing source line. @retval 0 Every aperture is mapped and the sizes sum exactly.
 * @pre @p workspace is open. @pre The region order matches ::test_backing_t. @post No aperture, workspace, or engine state changes. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_aperture_hosts(const emu_memmap_workspace_t* workspace)
{
  uint64_t total = 0U;
  for (size_t index = 0U; index < k_emu_memmap_backing_count; index++) {
    TEST_CHECK(workspace->backings[index].host != nullptr);
    total += workspace->backings[index].size;
  }
  TEST_CHECK(total == k_test_logical_bytes);
  return 0;
}

/**
 * @brief Prove the planned geometry and that re-opening is refused.
 * @details The requirements answer, the null-destination guard, the aperture table and the refusal to re-open a live workspace are one contract. @param[out] workspace Workspace opened by this call. @return Zero on success, else the failing source line. @retval 0 The geometry matched and the second open was refused.
 * @pre No workspace is open on @p workspace. @pre The call executes on the emulator's single owning thread. @post Success leaves one workspace the caller must release. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_open_geometry(emu_memmap_workspace_t* workspace)
{
  const emu_memmap_result_t requirements = emu_memmap_requirements();
  TEST_CHECK(requirements.status == k_emu_memmap_ok);
  TEST_CHECK(requirements.logical_backing_bytes == k_test_logical_bytes);
  TEST_CHECK(emu_memmap_open(nullptr).status == k_emu_memmap_invalid);
  TEST_CHECK(emu_memmap_open(workspace).status == k_emu_memmap_ok);
  TEST_STEP(internal_test_aperture_hosts(workspace));
  /* Re-opening would strand the apertures this workspace already owns. */
  TEST_CHECK(emu_memmap_open(workspace).status == k_emu_memmap_invalid);
  return 0;
}

/**
 * @brief Prove apertures commit lazily and exactly one written page lands.
 * @details Nothing is resident after the open or after an engine attaches; the single page a guest-visible write touches is the only one committed. @param[in,out] workspace Open workspace under measurement. @param[out] uc Engine opened and attached by this call. @return Zero on success, else the failing source line. @retval 0 Residency moved from zero to exactly one page.
 * @pre @p workspace is open and has no engine attached. @pre @p uc addresses a writable engine slot. @post Success leaves one engine attached for the caller to release. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_lazy_commit(emu_memmap_workspace_t* workspace, uc_engine** uc)
{
  TEST_CHECK(workspace->backings[k_test_backing_sdram].host != nullptr);
  TEST_CHECK(internal_test_resident_pages(&workspace->backings[k_test_backing_sdram],
                                          k_test_sdram_pages) == 0U);
  TEST_CHECK(internal_test_engine_open(uc));
  TEST_CHECK(emu_memmap_attach(workspace, *uc).status == k_emu_memmap_ok);
  TEST_CHECK(internal_test_resident_pages(&workspace->backings[k_test_backing_sdram],
                                          k_test_sdram_pages) == 0U);
  const uint32_t value = 0x0BADF00DU;
  TEST_CHECK(emu_mem_write(*uc, k_test_sdram_data, &value, sizeof(value)) == UC_ERR_OK);
  TEST_CHECK(internal_test_resident_pages(&workspace->backings[k_test_backing_sdram],
                                          k_test_sdram_pages) == 1U);
  return 0;
}

/**
 * @brief Prove the exact geometry and that apertures commit lazily.
 * @details The apertures span 130 MiB of logical guest memory. This proves the emulator does not pay that in host memory: nothing is resident after the open or after an engine attaches, and exactly the one page a write lands on becomes resident. @return The test geometry and lazy backing result produced by the fixture. @retval value Zero on success, else the failing source line.
 * @pre No workspace is open in this process. @pre The call executes on the emulator's single owning thread. @post Every workspace and engine opened here is released again. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_geometry_and_lazy_backing(void)
{
  emu_memmap_workspace_t workspace = {};
  TEST_STEP(internal_test_open_geometry(&workspace));
  uc_engine* uc = nullptr;
  TEST_STEP(internal_test_lazy_commit(&workspace, &uc));
  TEST_STEP(internal_test_unbind(&workspace, uc));
  TEST_CHECK(emu_memmap_close(&workspace));
  TEST_CHECK(workspace.backings[k_test_backing_sdram].host == nullptr);
  return 0;
}

/**
 * @brief Open one workspace and bind two engines to it.
 * @details The alias and late-attach scenarios both need two live engines on one set of apertures, which is the maximum this module binds. @param[out] workspace Workspace opened by this call. @param[out] cpu0 First engine, opened and attached. @param[out] cpu1 Second engine, opened and attached. @return Zero on success, else the failing source line. @retval 0 Both engines are attached to one workspace.
 * @pre No workspace is open on @p workspace. @pre @p cpu0 and @p cpu1 are distinct writable engine slots. @post Success leaves two engines the caller must release. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership. @since 0.1.0
 */
RA8_INTERNAL static int
internal_test_bind_pair(emu_memmap_workspace_t* workspace, uc_engine** cpu0, uc_engine** cpu1)
{
  TEST_CHECK(emu_memmap_open(workspace).status == k_emu_memmap_ok);
  TEST_CHECK(internal_test_engine_open(cpu0) && internal_test_engine_open(cpu1));
  TEST_CHECK(emu_memmap_attach(workspace, *cpu0).status == k_emu_memmap_ok);
  TEST_CHECK(emu_memmap_attach(workspace, *cpu1).status == k_emu_memmap_ok);
  return 0;
}

/**
 * @brief Prove a host write to SRAM is one state in every view.
 * @details The Secure address, its bit[28] Non-secure alias, the other engine's view and the aperture bytes themselves are the same object. @param[in] workspace Open workspace backing both engines. @param[in,out] cpu0 Engine performing the write. @param[in,out] cpu1 Engine observing it. @return Zero on success, else the failing source line. @retval 0 Every view reported the written word.
 * @pre Both engines are attached to @p workspace. @pre The call executes on the emulator's single owning thread. @post One SRAM word is written and nothing is released. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_sram_alias_views(const emu_memmap_workspace_t* workspace,
                                                       uc_engine*                    cpu0,
                                                       uc_engine*                    cpu1)
{
  const uint32_t secure = 0x11223344U;
  TEST_CHECK(emu_mem_write(cpu0, k_test_sram2_data, &secure, sizeof(secure)) == UC_ERR_OK);
  TEST_CHECK(internal_test_view_word(cpu0, k_test_ns_sram_data) == secure);
  TEST_CHECK(internal_test_view_word(cpu1, k_test_sram2_data) == secure);
  TEST_CHECK(internal_test_backing_word(workspace, k_test_backing_sram, 0x00100200U) == secure);
  return 0;
}

/**
 * @brief Prove a host write to SDRAM is one state in every view.
 * @details The Non-secure SDRAM alias, the Secure address, the other engine's view and the aperture bytes are the same object. @param[in] workspace Open workspace backing both engines. @param[in,out] cpu0 Engine observing the write. @param[in,out] cpu1 Engine performing it. @return Zero on success, else the failing source line. @retval 0 Every view reported the written word.
 * @pre Both engines are attached to @p workspace. @pre The call executes on the emulator's single owning thread. @post One SDRAM word is written and nothing is released. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_sdram_alias_views(const emu_memmap_workspace_t* workspace,
                                                        uc_engine*                    cpu0,
                                                        uc_engine*                    cpu1)
{
  const uint32_t sdram = 0x55667788U;
  TEST_CHECK(emu_mem_write(cpu1, k_test_ns_sdram_data, &sdram, sizeof(sdram)) == UC_ERR_OK);
  TEST_CHECK(internal_test_view_word(cpu0, k_test_sdram_data) == sdram);
  TEST_CHECK(internal_test_view_word(cpu1, k_test_ns_sdram_data) == sdram);
  TEST_CHECK(internal_test_backing_word(workspace, k_test_backing_sdram, 0x200U) == sdram);
  return 0;
}

/**
 * @brief Prove guest stores and instruction fetch share the same apertures.
 * @details Each engine executes a store from a shared aperture and the other engine sees the result, including the instruction bytes themselves. @param[in,out] cpu0 First engine. @param[in,out] cpu1 Second engine. @return Zero on success, else the failing source line. @retval 0 Both guest stores were visible to the other engine.
 * @pre Both engines are attached to one workspace. @pre The call executes on the emulator's single owning thread. @post Aperture bytes hold the stored words; nothing is released. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_guest_store_views(uc_engine* cpu0, uc_engine* cpu1)
{
  TEST_CHECK(internal_test_guest_store(cpu0, k_test_sram_code, k_test_sdram_data, 0xAABBCCDDU) ==
             UC_ERR_OK);
  TEST_CHECK(internal_test_view_word(cpu1, k_test_ns_sdram_data) == 0xAABBCCDDU);
  TEST_CHECK(internal_test_guest_store(cpu1, k_test_ospi_code, k_test_sram_data, 0xC001CAFEU) ==
             UC_ERR_OK);
  TEST_CHECK(internal_test_view_word(cpu0, k_test_sram_data) == 0xC001CAFEU);
  TEST_CHECK(internal_test_view_word(cpu1, k_test_ns_ospi_code) == k_test_thumb_str_r0r1);
  return 0;
}

/**
 * @brief Prove a workspace refuses to close while any engine is bound.
 * @details A live binding holds uc_mem_map_ptr references into the aperture pages, so closing under one would strand that engine on unmapped host memory. The refusal must hold with two bindings and with one. @param[in,out] workspace Workspace with both engines still bound. @param[in,out] cpu1 Engine detached and closed here. @return Zero on success, else the failing source line. @retval 0 Both closes were refused and one engine was released.
 * @pre Two engines are attached to @p workspace. @pre The call executes on the emulator's single owning thread. @post @p cpu1 is detached and closed; @p workspace is still open. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_close_refused_while_bound(emu_memmap_workspace_t* workspace,
                                                                uc_engine*              cpu1)
{
  TEST_CHECK(!emu_memmap_close(workspace));
  TEST_CHECK(emu_memmap_detach(workspace, cpu1));
  TEST_CHECK(uc_close(cpu1) == UC_ERR_OK);
  TEST_CHECK(!emu_memmap_close(workspace));
  return 0;
}

/**
 * @brief Prove the last detach releases the workspace and is not repeatable.
 * @details Detaching the final engine lets the workspace close; detaching it a second time must be refused rather than silently accepted. @param[in,out] workspace Workspace with one engine still bound. @param[in,out] cpu0 The last bound engine. @return Zero on success, else the failing source line. @retval 0 The workspace closed and the repeat detach was refused.
 * @pre Exactly one engine is attached to @p workspace. @pre The call executes on the emulator's single owning thread. @post No host mapping or engine from this binding remains. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_close_after_last_detach(emu_memmap_workspace_t* workspace,
                                                              uc_engine*              cpu0)
{
  TEST_CHECK(emu_memmap_detach(workspace, cpu0));
  TEST_CHECK(!emu_memmap_detach(workspace, cpu0));
  TEST_CHECK(uc_close(cpu0) == UC_ERR_OK);
  TEST_CHECK(emu_memmap_close(workspace));
  return 0;
}

/**
 * @brief Prove aliases, CPU views, execution, and binding lifetime.
 * @details The Secure aperture, its IDAU bit[28] Non-secure alias, and every attached engine are one state, for host writes and for guest stores alike, including instruction fetch from a shared aperture. @return The test alias execution and lifetime result produced by the fixture. @retval value Zero on success, else the failing source line.
 * @pre No workspace is open in this process. @pre The call executes on the emulator's single owning thread. @post Every workspace and engine opened here is released again. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_alias_execution_and_lifetime(void)
{
  emu_memmap_workspace_t workspace = {};
  uc_engine*             cpu0      = nullptr;
  uc_engine*             cpu1      = nullptr;
  TEST_STEP(internal_test_bind_pair(&workspace, &cpu0, &cpu1));
  TEST_STEP(internal_test_sram_alias_views(&workspace, cpu0, cpu1));
  TEST_STEP(internal_test_sdram_alias_views(&workspace, cpu0, cpu1));
  TEST_STEP(internal_test_guest_store_views(cpu0, cpu1));
  TEST_STEP(internal_test_close_refused_while_bound(&workspace, cpu1));
  TEST_STEP(internal_test_close_after_last_detach(&workspace, cpu0));
  return 0;
}

/**
 * @brief Open two independent workspaces, each with its own engine.
 * @details Two simultaneous instances are the case a shared authoritative store would silently break, so they are brought up separately. @param[out] first First workspace. @param[out] second Second workspace. @param[out] first_uc Engine attached to @p first. @param[out] second_uc Engine attached to @p second. @return Zero on success, else the failing source line. @retval 0 Both instances are open and bound.
 * @pre Neither workspace is currently open. @pre All four output objects are distinct. @post Success leaves two workspaces and two engines to release. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_open_two_instances(emu_memmap_workspace_t* first,
                                                         emu_memmap_workspace_t* second,
                                                         uc_engine**             first_uc,
                                                         uc_engine**             second_uc)
{
  TEST_CHECK(emu_memmap_open(first).status == k_emu_memmap_ok);
  TEST_CHECK(emu_memmap_open(second).status == k_emu_memmap_ok);
  TEST_CHECK(internal_test_engine_open(first_uc) && internal_test_engine_open(second_uc));
  TEST_CHECK(emu_memmap_attach(first, *first_uc).status == k_emu_memmap_ok);
  TEST_CHECK(emu_memmap_attach(second, *second_uc).status == k_emu_memmap_ok);
  return 0;
}

/**
 * @brief Prove two instances share no bytes at the identical guest address.
 * @details Each instance is written a distinct word at the same address; both views keep their own word and the host mappings are different objects. @param[in] first First workspace. @param[in] second Second workspace. @param[in,out] first_uc Engine attached to @p first. @param[in,out] second_uc Engine attached to @p second. @return Zero on success, else the failing source line. @retval 0 The two instances stayed independent.
 * @pre Both instances are open and bound. @pre The call executes on the emulator's single owning thread. @post One word is written in each instance; nothing is released. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_instances_isolated(const emu_memmap_workspace_t* first,
                                                         const emu_memmap_workspace_t* second,
                                                         uc_engine*                    first_uc,
                                                         uc_engine*                    second_uc)
{
  const uint32_t first_value  = 0x11111111U;
  const uint32_t second_value = 0x22222222U;
  TEST_CHECK(emu_mem_write(first_uc, k_test_sdram_data, &first_value, 4U) == UC_ERR_OK);
  TEST_CHECK(emu_mem_write(second_uc, k_test_sdram_data, &second_value, 4U) == UC_ERR_OK);
  TEST_CHECK(internal_test_view_word(first_uc, k_test_sdram_data) == first_value);
  TEST_CHECK(internal_test_view_word(second_uc, k_test_sdram_data) == second_value);
  TEST_CHECK(first->backings[k_test_backing_sdram].host !=
             second->backings[k_test_backing_sdram].host);
  return 0;
}

/**
 * @brief Prove two simultaneous backing instances do not share bytes.
 * @details Two workspaces own distinct apertures, so a write through one is invisible to the other even at the identical guest address. @return The test independent instances result produced by the fixture. @retval value Zero on success, else the failing source line.
 * @pre No workspace is open in this process. @pre The call executes on the emulator's single owning thread. @post Every workspace and engine opened here is released again. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_independent_instances(void)
{
  emu_memmap_workspace_t first     = {};
  emu_memmap_workspace_t second    = {};
  uc_engine*             first_uc  = nullptr;
  uc_engine*             second_uc = nullptr;
  TEST_STEP(internal_test_open_two_instances(&first, &second, &first_uc, &second_uc));
  TEST_STEP(internal_test_instances_isolated(&first, &second, first_uc, second_uc));
  TEST_CHECK(emu_memmap_detach(&first, first_uc) && emu_memmap_detach(&second, second_uc));
  TEST_CHECK((uc_close(first_uc) == UC_ERR_OK) && (uc_close(second_uc) == UC_ERR_OK));
  TEST_CHECK(emu_memmap_close(&first) && emu_memmap_close(&second));
  return 0;
}

/**
 * @brief Prove byte, halfword and word guest stores land at exact offsets.
 * @details Each store width is executed from SRAM against a distinct SDRAM offset, then the aperture bytes are read back directly. @param[in] workspace Open workspace backing the engine. @param[in,out] uc Engine executing the stores. @return Zero on success, else the failing source line. @retval 0 Every width landed exactly where the silicon puts it.
 * @pre @p uc is attached to @p workspace. @pre The call executes on the emulator's single owning thread. @post SDRAM offsets 0x200..0x207 hold the stored bytes. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_store_widths(const emu_memmap_workspace_t* workspace,
                                                   uc_engine*                    uc)
{
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
  const uint8_t* const committed = &workspace->backings[k_test_backing_sdram].host[0x200];
  TEST_CHECK((committed[0] == 0xA5U) && (committed[1] == 0U));
  TEST_CHECK((committed[2] == 0xEFU) && (committed[3] == 0xBEU));
  TEST_CHECK(internal_test_backing_word(workspace, k_test_backing_sdram, 0x204U) == 0xC001CAFEU);
  return 0;
}

/**
 * @brief Prove an unbounded host transfer publishes whole, and reject bad args.
 * @details A transfer spanning two pages is visible in every view -- there is no per-transfer size ceiling -- while a null source and a zero length are both refused. @param[in] workspace Open workspace backing the engine. @param[in,out] uc Engine performing the transfers. @return Zero on success, else the failing source line. @retval 0 The whole transfer published and both bad calls were refused.
 * @pre @p uc is attached to @p workspace. @pre The call executes on the emulator's single owning thread. @post Two SDRAM pages hold the transfer payload. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership. @since 0.1.0
 */
RA8_INTERNAL static int
internal_test_multipage_and_arg_guards(const emu_memmap_workspace_t* workspace, uc_engine* uc)
{
  /** @brief Payload for the multi-page host transfer probe. */
  static uint8_t s_multipage[k_test_multipage_size];
  const size_t   tail = sizeof(s_multipage) - (size_t)k_test_backing_word_sz;
  (void)memset(s_multipage, 0xCC, sizeof(s_multipage));
  TEST_CHECK(emu_mem_write(uc, k_test_sdram_data, s_multipage, sizeof(s_multipage)) == UC_ERR_OK);
  TEST_CHECK(internal_test_view_word(uc, k_test_sdram_data) == 0xCCCCCCCCU);
  TEST_CHECK(internal_test_view_word(uc, k_test_ns_sdram_data + tail) == 0xCCCCCCCCU);
  TEST_CHECK(internal_test_backing_word(workspace, k_test_backing_sdram, 0x200U + tail) ==
             0xCCCCCCCCU);
  TEST_CHECK(emu_mem_write(uc, k_test_sdram_data, nullptr, 4U) == UC_ERR_ARG);
  TEST_CHECK(emu_mem_write(uc, k_test_sdram_data, s_multipage, 0U) == UC_ERR_ARG);
  return 0;
}

/**
 * @brief Prove every store width and an unbounded host transfer stay coherent.
 * @details Byte, halfword and word guest stores land at the exact aperture offsets, and a host transfer spanning two pages is published whole to every view -- there is no per-transfer size ceiling. @return The test guest widths and multipage result produced by the fixture. @retval value Zero on success, else the failing source line.
 * @pre No workspace is open in this process. @pre The call executes on the emulator's single owning thread. @post Every workspace and engine opened here is released again. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_guest_widths_and_multipage(void)
{
  emu_memmap_workspace_t workspace = {};
  uc_engine*             uc        = nullptr;
  TEST_STEP(internal_test_bind(&workspace, &uc));
  TEST_STEP(internal_test_store_widths(&workspace, uc));
  TEST_STEP(internal_test_multipage_and_arg_guards(&workspace, uc));
  TEST_STEP(internal_test_unbind(&workspace, uc));
  return 0;
}

/**
 * @brief Prove one address the silicon does not decode faults every way in.
 * @details The guest load faults, and both host read and host write are refused, exactly as a J-Link access to that window is on an EK-RA8D2. @param[in,out] uc Engine with the load instruction already staged. @param[in] address Guest address that must not be reachable. @return Zero on success, else the failing source line. @retval 0 Every access to @p address was refused.
 * @pre The Thumb load at ::k_test_sram_code is staged. @pre @p uc is attached to an open workspace. @post No aperture byte is modified. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_unmapped_probe(uc_engine* uc, uint64_t address)
{
  const uint32_t target = (uint32_t)address;
  (void)uc_reg_write(uc, UC_ARM_REG_R1, &target);
  TEST_CHECK(uc_emu_start(uc, k_test_sram_code | 1U, k_test_sram_code + 2U, 0U, 1U) != UC_ERR_OK);
  uint32_t word = 0U;
  TEST_CHECK(emu_mem_read(uc, address, &word, sizeof(word)) != UC_ERR_OK);
  TEST_CHECK(emu_mem_write(uc, address, &word, sizeof(word)) != UC_ERR_OK);
  return 0;
}

/**
 * @brief Prove the guest map contains only windows the silicon decodes.
 * @details The phantom 0x27000000 data-flash window #397 removed must fault exactly as a J-Link write to it does on an EK-RA8D2, and no host bookkeeping page may be reachable from firmware. @return The test unmapped windows result produced by the fixture. @retval value Zero on success, else the failing source line.
 * @pre No workspace is open in this process. @pre The call executes on the emulator's single owning thread. @post Every workspace and engine opened here is released again. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_unmapped_windows(void)
{
  emu_memmap_workspace_t workspace = {};
  uc_engine*             uc        = nullptr;
  TEST_STEP(internal_test_bind(&workspace, &uc));
  const uint16_t load = (uint16_t)k_test_thumb_ldr_r0r1;
  TEST_CHECK(emu_mem_write(uc, k_test_sram_code, &load, sizeof(load)) == UC_ERR_OK);
  const uint64_t probes[] = {k_test_top_of_space, k_test_phantom_flash};
  for (size_t index = 0U; index < 2U; index++) {
    TEST_STEP(internal_test_unmapped_probe(uc, probes[index]));
  }
  TEST_STEP(internal_test_unbind(&workspace, uc));
  return 0;
}

/**
 * @brief Attach a second engine to a workspace that already has one bound.
 * @details The late engine binds the same host pages the first one is already using, with no replay step in between. @param[in,out] workspace Open workspace with one engine bound. @param[out] cpu1 Engine opened and attached by this call. @return Zero on success, else the failing source line. @retval 0 The second engine is attached.
 * @pre One engine is already attached to @p workspace. @pre @p cpu1 addresses a writable engine slot. @post Success leaves a second engine the caller must release. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_attach_second(emu_memmap_workspace_t* workspace,
                                                    uc_engine**             cpu1)
{
  TEST_CHECK(internal_test_engine_open(cpu1));
  TEST_CHECK(emu_memmap_attach(workspace, *cpu1).status == k_emu_memmap_ok);
  return 0;
}

/**
 * @brief Prove an engine attached after the writes sees every one of them.
 * @details The late engine binds the same host pages, so it observes SRAM, SDRAM and OSPI bytes written before it existed, through both views. @param[in,out] cpu1 Engine attached after the writes. @param[in] sram_value Word written to SRAM before the attach. @param[in] sdram_value Word written to SDRAM before the attach. @param[in] ospi_value Word written to OSPI before the attach. @return Zero on success, else the failing source line. @retval 0 Every earlier write was visible to the late engine.
 * @pre The three words are already written through the first engine. @pre @p cpu1 is attached to the same workspace. @post No aperture byte is modified. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_late_views(uc_engine* cpu1,
                                                 uint32_t   sram_value,
                                                 uint32_t   sdram_value,
                                                 uint32_t   ospi_value)
{
  TEST_CHECK(internal_test_view_word(cpu1, k_test_sram2_data) == sram_value);
  TEST_CHECK(internal_test_view_word(cpu1, k_test_ns_sram_data) == sram_value);
  TEST_CHECK(internal_test_view_word(cpu1, k_test_sdram_data) == sdram_value);
  TEST_CHECK(internal_test_view_word(cpu1, k_test_ns_sdram_data) == sdram_value);
  TEST_CHECK(internal_test_view_word(cpu1, k_test_ospi_code) == ospi_value);
  TEST_CHECK(internal_test_view_word(cpu1, k_test_ns_ospi_code) == ospi_value);
  return 0;
}

/**
 * @brief Write one word into each of the three apertures through one engine.
 * @details The three seeds are what a later engine has to be able to see. @param[in,out] cpu0 Engine performing the writes. @param[in] sram_value Word written to SRAM. @param[in] sdram_value Word written to SDRAM. @param[in] ospi_value Word written to OSPI. @return Zero on success, else the failing source line. @retval 0 All three apertures were seeded.
 * @pre @p cpu0 is attached to an open workspace. @pre The call executes on the emulator's single owning thread. @post One word is written in each aperture. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_seed_apertures(uc_engine* cpu0,
                                                     uint32_t   sram_value,
                                                     uint32_t   sdram_value,
                                                     uint32_t   ospi_value)
{
  TEST_CHECK(emu_mem_write(cpu0, k_test_sram2_data, &sram_value, 4U) == UC_ERR_OK);
  TEST_CHECK(emu_mem_write(cpu0, k_test_sdram_data, &sdram_value, 4U) == UC_ERR_OK);
  TEST_CHECK(emu_mem_write(cpu0, k_test_ospi_code, &ospi_value, 4U) == UC_ERR_OK);
  return 0;
}

/**
 * @brief Prove a third engine is refused without disturbing the two live ones.
 * @details There is no binding slot left, so the attach must fail -- and the engines already bound must go on serving their apertures. @param[in,out] workspace Workspace with both binding slots taken. @param[in,out] cpu0 First live engine, re-read after the refusal. @param[in] sdram_value Word @p cpu0 must still see in SDRAM. @return Zero on success, else the failing source line. @retval 0 The third attach was refused and the live views held.
 * @pre Two engines are attached to @p workspace. @pre The call executes on the emulator's single owning thread. @post The third engine is closed again. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_third_engine_refused(emu_memmap_workspace_t* workspace,
                                                           uc_engine*              cpu0,
                                                           uint32_t                sdram_value)
{
  uc_engine* extra = nullptr;
  TEST_CHECK(internal_test_engine_open(&extra));
  TEST_CHECK(emu_memmap_attach(workspace, extra).status != k_emu_memmap_ok);
  TEST_CHECK(uc_close(extra) == UC_ERR_OK);
  TEST_CHECK(internal_test_view_word(cpu0, k_test_sdram_data) == sdram_value);
  return 0;
}

/**
 * @brief Detach and close both engines, then release the workspace.
 * @details The workspace can only close once no binding holds its pages. @param[in,out] workspace Workspace to release. @param[in,out] cpu0 First bound engine. @param[in,out] cpu1 Second bound engine. @return Zero on success, else the failing source line. @retval 0 Both engines and the workspace were released.
 * @pre Both engines are attached to @p workspace. @pre The call executes on the emulator's single owning thread. @post No host mapping or engine from this binding remains. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership. @since 0.1.0
 */
RA8_INTERNAL static int
internal_test_unbind_pair(emu_memmap_workspace_t* workspace, uc_engine* cpu0, uc_engine* cpu1)
{
  TEST_CHECK(emu_memmap_detach(workspace, cpu1) && emu_memmap_detach(workspace, cpu0));
  TEST_CHECK((uc_close(cpu1) == UC_ERR_OK) && (uc_close(cpu0) == UC_ERR_OK));
  TEST_CHECK(emu_memmap_close(workspace));
  return 0;
}

/**
 * @brief Seed the three apertures, then bind a second engine and read them.
 * @details The whole point of the late attach is that the second engine needs no replay step to see writes that predate it. @param[in,out] workspace Open workspace with one engine bound. @param[in,out] cpu0 Engine that performs the seeding. @param[out] cpu1 Engine opened and attached after the seeding. @param[in] sram_value Word written to SRAM. @param[in] sdram_value Word written to SDRAM. @param[in] ospi_value Word written to OSPI. @return Zero on success, else the failing source line. @retval 0 The late engine saw every earlier write.
 * @pre @p cpu0 is attached to @p workspace. @pre @p cpu1 addresses a writable engine slot. @post Success leaves a second engine the caller must release. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_seed_and_late_attach(emu_memmap_workspace_t* workspace,
                                                           uc_engine*              cpu0,
                                                           uc_engine**             cpu1,
                                                           uint32_t                sram_value,
                                                           uint32_t                sdram_value,
                                                           uint32_t                ospi_value)
{
  TEST_STEP(internal_test_seed_apertures(cpu0, sram_value, sdram_value, ospi_value));
  TEST_STEP(internal_test_attach_second(workspace, cpu1));
  TEST_STEP(internal_test_late_views(*cpu1, sram_value, sdram_value, ospi_value));
  return 0;
}

/**
 * @brief Prove an engine attached late sees every earlier aperture write.
 * @details A second engine binds the same host pages, so it observes SRAM, SDRAM and OSPI bytes written before it existed, through both the Secure and the Non-secure view, with no replay step to get wrong. @return The test late attach coherence result produced by the fixture. @retval value Zero on success, else the failing source line.
 * @pre No workspace is open in this process. @pre The call executes on the emulator's single owning thread. @post Every workspace and engine opened here is released again. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_late_attach_coherence(void)
{
  emu_memmap_workspace_t workspace = {};
  uc_engine*             cpu0      = nullptr;
  TEST_STEP(internal_test_bind(&workspace, &cpu0));
  const uint32_t sram_value  = 0x31415926U;
  const uint32_t sdram_value = 0x27182818U;
  const uint32_t ospi_value  = 0x16180339U;
  uc_engine*     cpu1        = nullptr;
  TEST_STEP(internal_test_seed_and_late_attach(&workspace,
                                               cpu0,
                                               &cpu1,
                                               sram_value,
                                               sdram_value,
                                               ospi_value));
  TEST_STEP(internal_test_third_engine_refused(&workspace, cpu0, sdram_value));
  TEST_STEP(internal_test_unbind_pair(&workspace, cpu0, cpu1));
  return 0;
}

/**
 * @brief Write the same word into the SDRAM and SRAM apertures.
 * @details The two seeds are what the next workspace must NOT inherit. @param[in,out] uc Engine performing the writes. @param[in] value Word written to both apertures. @return Zero on success, else the failing source line. @retval 0 Both apertures hold @p value.
 * @pre @p uc is attached to an open workspace. @pre The call executes on the emulator's single owning thread. @post Both apertures hold @p value. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_seed_two_apertures(uc_engine* uc, uint32_t value)
{
  TEST_CHECK(emu_mem_write(uc, k_test_sdram_data, &value, 4U) == UC_ERR_OK);
  TEST_CHECK(emu_mem_write(uc, k_test_sram2_data, &value, 4U) == UC_ERR_OK);
  return 0;
}

/**
 * @brief Prove a fresh workspace serves zeroes everywhere it is read.
 * @details The previous run wrote SRAM and SDRAM; after a close and re-open neither address, nor the Non-secure alias, may still hold those bytes. @param[in,out] uc Engine bound to the freshly opened workspace. @return Zero on success, else the failing source line. @retval 0 Every probed address read back as zero.
 * @pre @p uc is attached to a freshly opened workspace. @pre The call executes on the emulator's single owning thread. @post No aperture byte is modified. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_blank_after_reopen(uc_engine* uc)
{
  TEST_CHECK(internal_test_view_word(uc, k_test_sdram_data) == 0U);
  TEST_CHECK(internal_test_view_word(uc, k_test_sram2_data) == 0U);
  TEST_CHECK(internal_test_view_word(uc, k_test_ns_sram_data) == 0U);
  return 0;
}

/**
 * @brief Prove a fresh workspace starts blank after an earlier one is closed.
 * @details Closing releases the apertures, so the next workspace cannot inherit the previous run's bytes -- the property a persistent authoritative store would silently break. @return The test backing lifetime result produced by the fixture. @retval value Zero on success, else the failing source line.
 * @pre No workspace is open in this process. @pre The call executes on the emulator's single owning thread. @post Every workspace and engine opened here is released again. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_backing_lifetime(void)
{
  emu_memmap_workspace_t workspace = {};
  uc_engine*             uc        = nullptr;
  TEST_STEP(internal_test_bind(&workspace, &uc));
  TEST_STEP(internal_test_seed_two_apertures(uc, 0x5A5AA5A5U));
  TEST_STEP(internal_test_unbind(&workspace, uc));
  TEST_STEP(internal_test_bind(&workspace, &uc));
  TEST_STEP(internal_test_blank_after_reopen(uc));
  TEST_STEP(internal_test_unbind(&workspace, uc));
  return 0;
}

/**
 * @brief Prove a faulting store moves nothing and leaves the engine usable.
 * @details The store to unmapped memory faults, the settled aperture word is untouched in both the backing bytes and the guest view, and the very same engine then completes a legitimate store. @param[in] workspace Open workspace backing the engine. @param[in,out] uc Engine executing the stores. @param[in] settled Word the SRAM aperture held before the fault. @return Zero on success, else the failing source line. @retval 0 The fault changed nothing and the engine recovered.
 * @pre @p uc is attached to @p workspace. @pre The SRAM aperture holds @p settled. @post SDRAM holds the recovery store; SRAM still holds @p settled. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_fault_and_recovery(const emu_memmap_workspace_t* workspace,
                                                         uc_engine*                    uc,
                                                         uint32_t                      settled)
{
  TEST_CHECK(internal_test_guest_store(uc, k_test_sram_code, k_test_phantom_flash, 0x2468ACE0U) ==
             UC_ERR_WRITE_UNMAPPED);
  TEST_CHECK(internal_test_backing_word(workspace, k_test_backing_sram, 0x200U) == settled);
  TEST_CHECK(internal_test_view_word(uc, k_test_sram_data) == settled);
  TEST_CHECK(internal_test_run_store(uc, k_test_sram_code, k_test_sdram_data, 0x2468ACE0U) ==
             UC_ERR_OK);
  TEST_CHECK(internal_test_backing_word(workspace, k_test_backing_sdram, 0x200U) == 0x2468ACE0U);
  return 0;
}

/**
 * @brief Prove a faulting guest store changes nothing and leaves a usable engine.
 * @details A store to the unmapped phantom flash faults, no aperture byte moves, and the same engine still completes a legitimate store afterwards. @return The test guest store faults result produced by the fixture. @retval value Zero on success, else the failing source line.
 * @pre No workspace is open in this process. @pre The call executes on the emulator's single owning thread. @post Every workspace and engine opened here is released again. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_guest_store_faults(void)
{
  emu_memmap_workspace_t workspace = {};
  uc_engine*             uc        = nullptr;
  TEST_STEP(internal_test_bind(&workspace, &uc));
  const uint32_t settled = 0x13579BDFU;
  TEST_CHECK(emu_mem_write(uc, k_test_sram_data, &settled, 4U) == UC_ERR_OK);
  TEST_STEP(internal_test_fault_and_recovery(&workspace, uc, settled));
  TEST_STEP(internal_test_unbind(&workspace, uc));
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
