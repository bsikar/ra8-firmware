/**
 * @file test_emu_memmap.c
 * @brief Focused sparse authoritative memory and Unicorn-view tests
 * @details Exercises caller-owned backing storage, dirty-page publication,
 * fault injection, and reconciliation between authoritative bytes and mapped
 * Unicorn views without allocating memory in the implementation under test.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <errno.h>
#include <fcntl.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "emu_host_io_internal.h"
#include "emu_memmap.h"

/** @brief Test-only addresses, canaries, and Thumb instruction. */
typedef enum : uint64_t {
  /** @brief Canary width around caller scratch. */
  k_test_guard_bytes = 8U,
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
  /** @brief Protected host binding page. */
  k_test_metadata = 0xFFFFF000U,
  /** @brief Exact three-fd logical total. */
  k_test_logical_bytes = 136134656U,
  /** @brief Thumb `str r0, [r1]`. */
  k_test_thumb_str_r0r1 = 0x6008U,
  /** @brief Thumb `strb r0, [r1]`. */
  k_test_thumb_strb_r0r1 = 0x7008U,
  /** @brief Thumb `strh r0, [r1]`. */
  k_test_thumb_strh_r0r1 = 0x8008U,
} test_constant_t;

/** @brief Aligned scratch with fixed pre/post canaries. */
typedef struct {
  /** @brief Canary-surrounded scratch bytes. */
  alignas(max_align_t) uint8_t bytes[k_emu_memmap_scratch_bytes + (2U * k_test_guard_bytes)];
} test_guarded_scratch_t;

/** @brief Return the failing source line without process output. */
#define TEST_CHECK(condition)                                                                      \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      return __LINE__;                                                                             \
    }                                                                                              \
  } while (false)

static bool s_fail_next_pread;  /**< Inject exactly one positioned-read fault.  */
static bool s_fail_next_pwrite; /**< Inject exactly one positioned-write fault. */

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
 * @brief Production sequential-read adapter.
 * @details Production sequential-read adapter; this step is contained within the emu memmap test fixture and uses bounded caller or module-owned storage.
 * @param[in] fd Open raw descriptor used for the transfer.
 * @param[in,out] bytes Byte storage transferred by the operation.
 * @param[in] count Number of elements or bytes to process.
 * @return The test read result produced by the emu memmap test fixture.
 * @retval value The operation-specific test read value.
 * @pre Arguments satisfy the ranges documented for test read. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static ssize_t internal_test_read(int fd, void* bytes, size_t count)
{
  return read(fd, bytes, count);
}

/**
 * @brief Production sequential-write adapter.
 * @details Production sequential-write adapter; this step is contained within the emu memmap test fixture and uses bounded caller or module-owned storage.
 * @param[in] fd Open raw descriptor used for the transfer.
 * @param[in] bytes Byte storage transferred by the operation.
 * @param[in] count Number of elements or bytes to process.
 * @return The test write result produced by the emu memmap test fixture.
 * @retval value The operation-specific test write value.
 * @pre Arguments satisfy the ranges documented for test write. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static ssize_t internal_test_write(int fd, const void* bytes, size_t count)
{
  return write(fd, bytes, count);
}

/**
 * @brief Positioned-read adapter with one-shot failure injection.
 * @details Positioned-read adapter with one-shot failure injection; this step is contained within the emu memmap test fixture and uses bounded caller or module-owned storage.
 * @param[in] fd Open raw descriptor used for the transfer.
 * @param[in,out] bytes Byte storage transferred by the operation.
 * @param[in] count Number of elements or bytes to process.
 * @param[in] offset Byte or register offset at which processing begins.
 * @return The test pread result produced by the emu memmap test fixture.
 * @retval value The operation-specific test pread value.
 * @pre Arguments satisfy the ranges documented for test pread. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static ssize_t internal_test_pread(int fd, void* bytes, size_t count, off_t offset)
{
  if (s_fail_next_pread) {
    s_fail_next_pread = false;
    errno             = EIO;
    return -1;
  }
  return pread(fd, bytes, count, offset);
}

/**
 * @brief Positioned-write adapter with one-shot failure injection.
 * @details Positioned-write adapter with one-shot failure injection; this step is contained within the emu memmap test fixture and uses bounded caller or module-owned storage.
 * @param[in] fd Open raw descriptor used for the transfer.
 * @param[in] bytes Byte storage transferred by the operation.
 * @param[in] count Number of elements or bytes to process.
 * @param[in] offset Byte or register offset at which processing begins.
 * @return The test pwrite result produced by the emu memmap test fixture.
 * @retval value The operation-specific test pwrite value.
 * @pre Arguments satisfy the ranges documented for test pwrite. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static ssize_t
internal_test_pwrite(int fd, const void* bytes, size_t count, off_t offset)
{
  if (s_fail_next_pwrite) {
    s_fail_next_pwrite = false;
    errno              = EIO;
    return -1;
  }
  return pwrite(fd, bytes, count, offset);
}

static const emu_io_ops_t s_test_ops = {.read_fn   = internal_test_read,
                                        .write_fn  = internal_test_write,
                                        .pread_fn  = internal_test_pread,
                                        .pwrite_fn = internal_test_pwrite};

/**
 * @brief Reset scratch/canaries and return the writable interior.
 * @details Reset scratch/canaries and return the writable interior; this step is contained within the emu memmap test fixture and uses bounded caller or module-owned storage.
 * @param[in,out] guarded Guarded state or storage updated in place by the operation.
 * @return The test scratch reset result produced by the emu memmap test fixture.
 * @retval nullptr No test scratch reset result is available; otherwise the returned pointer identifies it.
 * @pre Arguments satisfy the ranges documented for test scratch reset. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint8_t* internal_test_scratch_reset(test_guarded_scratch_t* guarded)
{
  (void)memset(guarded->bytes, 0xA5, sizeof(guarded->bytes));
  return &guarded->bytes[k_test_guard_bytes];
}

/**
 * @brief Verify the publisher never crossed either scratch boundary.
 * @details Verify the publisher never crossed either scratch boundary; this step is contained within the emu memmap test fixture and uses bounded caller or module-owned storage.
 * @param[in] guarded Guarded input used by the operation.
 * @return The test guards valid result produced by the emu memmap test fixture.
 * @retval true The test guards valid condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for test guards valid. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_test_guards_valid(const test_guarded_scratch_t* guarded)
{
  for (size_t index = 0U; index < k_test_guard_bytes; index++) {
    if ((guarded->bytes[index] != 0xA5U) ||
        (guarded->bytes[sizeof(guarded->bytes) - 1U - index] != 0xA5U)) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Open and configure one Cortex-M engine.
 * @details Open and configure one cortex-m engine; this step is contained within the emu memmap test fixture and uses bounded caller or module-owned storage.
 * @param[in,out] engine Engine state or storage updated in place by the operation.
 * @return The test engine open result produced by the emu memmap test fixture.
 * @retval true The test engine open condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for test engine open. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
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
 * @details Read one word directly from a unicorn-owned view; this step is contained within the emu memmap test fixture and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @param[in] address Guest address involved in the operation.
 * @return The test view word result produced by the emu memmap test fixture.
 * @retval value The operation-specific test view word value.
 * @pre Arguments satisfy the ranges documented for test view word. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
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
 * @brief Read one word directly from the authoritative sparse descriptor.
 * @details Read one word directly from the authoritative sparse descriptor; this step is contained within the emu memmap test fixture and uses bounded caller or module-owned storage.
 * @param[in] workspace Caller-owned workspace used by the operation.
 * @param[in] backing Descriptor-backed authoritative storage object.
 * @param[in] offset Byte or register offset at which processing begins.
 * @return The test descriptor word result produced by the emu memmap test fixture.
 * @retval value The operation-specific test descriptor word value.
 * @pre Arguments satisfy the ranges documented for test descriptor word. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t
internal_test_fd_word(const emu_memmap_workspace_t* workspace, size_t backing, off_t offset)
{
  uint32_t value = 0U;
  (void)pread(workspace->backings[backing].fd, &value, sizeof(value), offset);
  return value;
}

/**
 * @brief Read one word directly from any test-owned raw descriptor.
 * @details Read one word directly from any test-owned raw descriptor; this step is contained within the emu memmap test fixture and uses bounded caller or module-owned storage.
 * @param[in] fd Open raw descriptor used for the transfer.
 * @param[in] offset Byte or register offset at which processing begins.
 * @return The test raw word result produced by the emu memmap test fixture.
 * @retval value The operation-specific test raw word value.
 * @pre Arguments satisfy the ranges documented for test raw word. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_test_raw_word(int fd, off_t offset)
{
  uint32_t value = 0U;
  (void)pread(fd, &value, sizeof(value), offset);
  return value;
}

/**
 * @brief Run one previously installed Thumb store instruction.
 * @details Run one previously installed thumb store instruction; this step is contained within the emu memmap test fixture and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @param[in] code Instruction, status, or command code decoded by the operation.
 * @param[in] target Target input used by the operation.
 * @param[in] value Register or payload value involved in the operation.
 * @return The test run store result produced by the emu memmap test fixture.
 * @retval value The operation-specific test run store value.
 * @pre Arguments satisfy the ranges documented for test run store. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
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
 * @details Install and execute one thumb store from the mapped code address; this step is contained within the emu memmap test fixture and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @param[in] code Instruction, status, or command code decoded by the operation.
 * @param[in] target Target input used by the operation.
 * @param[in] value Register or payload value involved in the operation.
 * @return The test guest store result produced by the emu memmap test fixture.
 * @retval value The operation-specific test guest store value.
 * @pre Arguments satisfy the ranges documented for test guest store. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
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
 * @brief Prove exact capacity, preservation, sparse size, and canaries.
 * @details Prove exact capacity, preservation, sparse size, and canaries; this step is contained within the emu memmap test fixture and uses bounded caller or module-owned storage.
 * @return The test requirements and sparse open result produced by the emu memmap test fixture.
 * @retval value The operation-specific test requirements and sparse open value.
 * @pre Arguments satisfy the ranges documented for test requirements and sparse open. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_test_requirements_and_sparse_open(void)
{
  const emu_memmap_result_t requirements = emu_memmap_requirements();
  TEST_CHECK(requirements.status == k_emu_memmap_ok);
  TEST_CHECK(requirements.required_scratch_bytes == k_emu_memmap_scratch_bytes);
  TEST_CHECK(requirements.logical_backing_bytes == k_test_logical_bytes);

  test_guarded_scratch_t guarded = {};
  uint8_t* const         scratch = internal_test_scratch_reset(&guarded);
  emu_memmap_workspace_t workspace;
  (void)memset(&workspace, 0x5A, sizeof(workspace));
  const emu_memmap_workspace_t saved = workspace;
  emu_memmap_result_t          result =
    emu_memmap_open(scratch, k_emu_memmap_scratch_bytes - 1U, &workspace);
  TEST_CHECK(result.status == k_emu_memmap_capacity);
  TEST_CHECK(result.required_scratch_bytes == k_emu_memmap_scratch_bytes);
  TEST_CHECK(result.supplied_scratch_bytes == k_emu_memmap_scratch_bytes - 1U);
  TEST_CHECK(memcmp(&workspace, &saved, sizeof(saved)) == 0);
  TEST_CHECK(internal_test_guards_valid(&guarded));

  result = emu_memmap_open(scratch, k_emu_memmap_scratch_bytes, &workspace);
  TEST_CHECK(result.status == k_emu_memmap_ok);
  uint64_t total = 0U;
  for (size_t index = 0U; index < k_emu_memmap_backing_count; index++) {
    struct stat metadata = {};
    TEST_CHECK(fstat(workspace.backings[index].fd, &metadata) == 0);
    TEST_CHECK((uint64_t)metadata.st_size == workspace.backings[index].size);
    TEST_CHECK(metadata.st_blocks == 0);
    total += (uint64_t)metadata.st_size;
  }
  TEST_CHECK(total == k_test_logical_bytes);
  TEST_CHECK(emu_memmap_close(&workspace));
  TEST_CHECK(emu_memmap_close(&workspace));
  TEST_CHECK(internal_test_guards_valid(&guarded));
  return 0;
}

/** @brief Prove byte, halfword, and word guest stores reach authoritative fd
 * state.  * @details Prove byte, halfword, and word guest stores reach authoritative fd; this step is contained within the emu memmap test fixture and uses bounded caller or module-owned storage.
 * @return The test guest widths and oversize result produced by the emu memmap test fixture.
 * @retval value The operation-specific test guest widths and oversize value.
 * @pre Arguments satisfy the ranges documented for test guest widths and oversize. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_test_guest_widths_and_oversize(void)
{
  test_guarded_scratch_t guarded   = {};
  emu_memmap_workspace_t workspace = {};
  TEST_CHECK(emu_memmap_open(internal_test_scratch_reset(&guarded), 4096U, &workspace).status ==
             k_emu_memmap_ok);
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
  uint8_t committed[8] = {};
  TEST_CHECK(pread(workspace.backings[1].fd, committed, sizeof(committed), 0x200) ==
             (ssize_t)sizeof(committed));
  TEST_CHECK((committed[0] == 0xA5U) && (committed[1] == 0U));
  TEST_CHECK((committed[2] == 0xEFU) && (committed[3] == 0xBEU));
  TEST_CHECK(internal_test_fd_word(&workspace, 1U, 0x204) == 0xC001CAFEU);

  uint8_t oversized[k_emu_memmap_scratch_bytes + 1U];
  (void)memset(oversized, 0xCC, sizeof(oversized));
  TEST_CHECK(emu_mem_write(uc, k_test_sdram_data, oversized, sizeof(oversized)) == UC_ERR_RESOURCE);
  TEST_CHECK(!emu_memmap_is_poisoned(&workspace));
  TEST_CHECK(internal_test_fd_word(&workspace, 1U, 0x200) == 0xBEEF00A5U);
  TEST_CHECK(emu_memmap_detach(&workspace, uc));
  TEST_CHECK(uc_close(uc) == UC_ERR_OK);
  TEST_CHECK(emu_memmap_close(&workspace));
  TEST_CHECK(internal_test_guards_valid(&guarded));
  return 0;
}

/** @brief Prove firmware can neither read nor execute the host binding metadata
 * page.  * @details Prove firmware can neither read nor execute the host binding metadata; this step is contained within the emu memmap test fixture and uses bounded caller or module-owned storage.
 * @return The test metadata guest protection result produced by the emu memmap test fixture.
 * @retval value The operation-specific test metadata guest protection value.
 * @pre Arguments satisfy the ranges documented for test metadata guest protection. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_test_metadata_guest_protection(void)
{
  test_guarded_scratch_t guarded   = {};
  emu_memmap_workspace_t workspace = {};
  TEST_CHECK(emu_memmap_open(internal_test_scratch_reset(&guarded), 4096U, &workspace).status ==
             k_emu_memmap_ok);
  uc_engine* uc = nullptr;
  TEST_CHECK(internal_test_engine_open(&uc));
  TEST_CHECK(emu_memmap_attach(&workspace, uc).status == k_emu_memmap_ok);

  const uint16_t load = 0x6808U; /* Thumb `ldr r0, [r1]`. */
  TEST_CHECK(emu_mem_write(uc, k_test_sram_code, &load, sizeof(load)) == UC_ERR_OK);
  const uint32_t metadata = (uint32_t)k_test_metadata;
  (void)uc_reg_write(uc, UC_ARM_REG_R1, &metadata);
  TEST_CHECK(uc_emu_start(uc, k_test_sram_code | 1U, k_test_sram_code + 2U, 0U, 1U) ==
             UC_ERR_READ_PROT);
  TEST_CHECK(emu_memmap_detach(&workspace, uc));
  TEST_CHECK(uc_emu_start(uc, k_test_metadata | 1U, k_test_metadata + 2U, 0U, 1U) != UC_ERR_OK);

  TEST_CHECK(uc_close(uc) == UC_ERR_OK);
  TEST_CHECK(emu_memmap_close(&workspace));
  TEST_CHECK(internal_test_guards_valid(&guarded));
  return 0;
}

/**
 * @brief Prove aliases, CPU views, execution, and warm-reset persistence.
 * @details Prove aliases, cpu views, execution, and warm-reset persistence; this step is contained within the emu memmap test fixture and uses bounded caller or module-owned storage.
 * @return The test alias execution and lifetime result produced by the emu memmap test fixture.
 * @retval value The operation-specific test alias execution and lifetime value.
 * @pre Arguments satisfy the ranges documented for test alias execution and lifetime. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_test_alias_execution_and_lifetime(void)
{
  test_guarded_scratch_t guarded   = {};
  emu_memmap_workspace_t workspace = {};
  TEST_CHECK(
    emu_memmap_open(internal_test_scratch_reset(&guarded), k_emu_memmap_scratch_bytes, &workspace)
      .status == k_emu_memmap_ok);
  uc_engine* cpu0 = nullptr;
  uc_engine* cpu1 = nullptr;
  TEST_CHECK(internal_test_engine_open(&cpu0) && internal_test_engine_open(&cpu1));
  TEST_CHECK(emu_memmap_attach(&workspace, cpu0).status == k_emu_memmap_ok);
  TEST_CHECK(emu_memmap_attach(&workspace, cpu1).status == k_emu_memmap_ok);

  const uint32_t secure = 0x11223344U;
  TEST_CHECK(emu_mem_write(cpu0, k_test_sram2_data, &secure, sizeof(secure)) == UC_ERR_OK);
  TEST_CHECK(internal_test_view_word(cpu0, k_test_ns_sram_data) == secure);
  TEST_CHECK(internal_test_view_word(cpu1, k_test_sram2_data) == secure);
  TEST_CHECK(internal_test_fd_word(&workspace, 0U, 0x00100200) == secure);

  const uint32_t sdram = 0x55667788U;
  TEST_CHECK(emu_mem_write(cpu1, k_test_ns_sdram_data, &sdram, sizeof(sdram)) == UC_ERR_OK);
  TEST_CHECK(internal_test_view_word(cpu0, k_test_sdram_data) == sdram);
  TEST_CHECK(internal_test_view_word(cpu1, k_test_ns_sdram_data) == sdram);
  TEST_CHECK(internal_test_fd_word(&workspace, 1U, 0x200) == sdram);

  TEST_CHECK(internal_test_guest_store(cpu0, k_test_sram_code, k_test_sdram_data, 0xAABBCCDDU) ==
             UC_ERR_OK);
  TEST_CHECK(internal_test_view_word(cpu1, k_test_ns_sdram_data) == 0xAABBCCDDU);
  TEST_CHECK(internal_test_guest_store(cpu1, k_test_ospi_code, k_test_sram_data, 0xC001CAFEU) ==
             UC_ERR_OK);
  TEST_CHECK(internal_test_view_word(cpu0, k_test_sram_data) == 0xC001CAFEU);
  TEST_CHECK(internal_test_view_word(cpu1, k_test_ns_ospi_code) == k_test_thumb_str_r0r1);

  uint32_t reset_value = 0U;
  (void)uc_reg_write(cpu0, UC_ARM_REG_R0, &reset_value);
  (void)uc_reg_write(cpu0, UC_ARM_REG_R1, &reset_value);
  TEST_CHECK(internal_test_view_word(cpu0, k_test_sram_data) == 0xC001CAFEU);
  TEST_CHECK(workspace.guest_writes == 2U);
  TEST_CHECK(workspace.host_writes == 4U);

  TEST_CHECK(emu_memmap_detach(&workspace, cpu1));
  TEST_CHECK(uc_close(cpu1) == UC_ERR_OK);
  TEST_CHECK(!emu_memmap_close(&workspace));
  TEST_CHECK(emu_memmap_detach(&workspace, cpu0));
  TEST_CHECK(uc_close(cpu0) == UC_ERR_OK);
  TEST_CHECK(emu_memmap_close(&workspace));
  TEST_CHECK(internal_test_guards_valid(&guarded));
  return 0;
}

/**
 * @brief Prove two simultaneous backing instances do not share bytes.
 * @details Prove two simultaneous backing instances do not share bytes; this step is contained within the emu memmap test fixture and uses bounded caller or module-owned storage.
 * @return The test independent instances result produced by the emu memmap test fixture.
 * @retval value The operation-specific test independent instances value.
 * @pre Arguments satisfy the ranges documented for test independent instances. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_test_independent_instances(void)
{
  test_guarded_scratch_t first_guarded  = {};
  test_guarded_scratch_t second_guarded = {};
  emu_memmap_workspace_t first          = {};
  emu_memmap_workspace_t second         = {};
  TEST_CHECK(emu_memmap_open(internal_test_scratch_reset(&first_guarded), 4096U, &first).status ==
             k_emu_memmap_ok);
  TEST_CHECK(emu_memmap_open(internal_test_scratch_reset(&second_guarded), 4096U, &second).status ==
             k_emu_memmap_ok);
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
  TEST_CHECK(first.backings[1].fd != second.backings[1].fd);
  TEST_CHECK(emu_memmap_detach(&first, first_uc) && emu_memmap_detach(&second, second_uc));
  TEST_CHECK((uc_close(first_uc) == UC_ERR_OK) && (uc_close(second_uc) == UC_ERR_OK));
  TEST_CHECK(emu_memmap_close(&first) && emu_memmap_close(&second));
  TEST_CHECK(internal_test_guards_valid(&first_guarded) &&
             internal_test_guards_valid(&second_guarded));
  return 0;
}

/**
 * @brief Prove host publication faults roll back fd/views and stop outputs.
 * @details Prove host publication faults roll back fd/views and stop outputs; this step is contained within the emu memmap test fixture and uses bounded caller or module-owned storage.
 * @return The test host fault atomicity result produced by the emu memmap test fixture.
 * @retval value The operation-specific test host fault atomicity value.
 * @pre Arguments satisfy the ranges documented for test host fault atomicity. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_test_host_fault_atomicity(void)
{
  test_guarded_scratch_t guarded   = {};
  emu_memmap_workspace_t workspace = {};
  TEST_CHECK(emu_memmap_open(internal_test_scratch_reset(&guarded), 4096U, &workspace).status ==
             k_emu_memmap_ok);
  uc_engine* uc = nullptr;
  TEST_CHECK(internal_test_engine_open(&uc));
  TEST_CHECK(emu_memmap_attach(&workspace, uc).status == k_emu_memmap_ok);
  const uint32_t old_value = 0x01020304U;
  const uint32_t new_value = 0xA1A2A3A4U;
  TEST_CHECK(emu_mem_write(uc, k_test_sdram_data, &old_value, 4U) == UC_ERR_OK);

  char      output_path[] = "/tmp/ra8-emu-output-XXXXXX";
  const int output_fd     = mkstemp(output_path);
  TEST_CHECK(output_fd >= 0);
  TEST_CHECK(unlink(output_path) == 0);
  const uint32_t sentinel = 0xDEADBEEFU;
  TEST_CHECK(write(output_fd, &sentinel, 4U) == 4);
  priv_emu_io_configure(output_fd, STDERR_FILENO, &s_test_ops);
  s_fail_next_pwrite = true;
  TEST_CHECK(emu_mem_write(uc, k_test_sdram_data, &new_value, 4U) != UC_ERR_OK);
  TEST_CHECK(emu_memmap_is_poisoned(&workspace));
  TEST_CHECK(internal_test_fd_word(&workspace, 1U, 0x200) == old_value);
  TEST_CHECK(internal_test_view_word(uc, k_test_sdram_data) == old_value);
  TEST_CHECK(internal_test_view_word(uc, k_test_ns_sdram_data) == old_value);
  TEST_CHECK(internal_test_raw_word(output_fd, 0) == sentinel);
  priv_emu_io_configure(STDOUT_FILENO, STDERR_FILENO, nullptr);
  TEST_CHECK(close(output_fd) == 0);
  TEST_CHECK(emu_memmap_detach(&workspace, uc));
  TEST_CHECK(uc_close(uc) == UC_ERR_OK);
  TEST_CHECK(emu_memmap_close(&workspace));
  TEST_CHECK(internal_test_guards_valid(&guarded));
  return 0;
}

/** @brief Prove pre-read and mirror failures preserve every still-mapped old
 * view.  * @details Prove pre-read and mirror failures preserve every still-mapped old; this step is contained within the emu memmap test fixture and uses bounded caller or module-owned storage.
 * @return The test read and mirror faults result produced by the emu memmap test fixture.
 * @retval value The operation-specific test read and mirror faults value.
 * @pre Arguments satisfy the ranges documented for test read and mirror faults. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_test_read_and_mirror_faults(void)
{
  test_guarded_scratch_t guarded   = {};
  emu_memmap_workspace_t workspace = {};
  TEST_CHECK(emu_memmap_open(internal_test_scratch_reset(&guarded), 4096U, &workspace).status ==
             k_emu_memmap_ok);
  uc_engine* cpu0 = nullptr;
  uc_engine* cpu1 = nullptr;
  TEST_CHECK(internal_test_engine_open(&cpu0) && internal_test_engine_open(&cpu1));
  TEST_CHECK(emu_memmap_attach(&workspace, cpu0).status == k_emu_memmap_ok);
  TEST_CHECK(emu_memmap_attach(&workspace, cpu1).status == k_emu_memmap_ok);
  const uint32_t old_value = 0x31415926U;
  const uint32_t new_value = 0x27182818U;
  TEST_CHECK(emu_mem_write(cpu0, k_test_sdram_data, &old_value, 4U) == UC_ERR_OK);

  TEST_CHECK(uc_mem_unmap(cpu1, k_test_ns_sdram_data & ~0xFFFU, 4096U) == UC_ERR_OK);
  TEST_CHECK(emu_mem_write(cpu0, k_test_sdram_data, &new_value, 4U) == UC_ERR_RESOURCE);
  TEST_CHECK(emu_memmap_is_poisoned(&workspace));
  TEST_CHECK(internal_test_fd_word(&workspace, 1U, 0x200) == old_value);
  TEST_CHECK(internal_test_view_word(cpu0, k_test_sdram_data) == old_value);
  TEST_CHECK(internal_test_view_word(cpu0, k_test_ns_sdram_data) == old_value);
  TEST_CHECK(internal_test_view_word(cpu1, k_test_sdram_data) == old_value);
  TEST_CHECK(emu_memmap_detach(&workspace, cpu1));
  TEST_CHECK(uc_close(cpu1) == UC_ERR_OK);
  TEST_CHECK(emu_memmap_detach(&workspace, cpu0));
  TEST_CHECK(uc_close(cpu0) == UC_ERR_OK);
  TEST_CHECK(emu_memmap_close(&workspace));

  TEST_CHECK(emu_memmap_open(internal_test_scratch_reset(&guarded), 4096U, &workspace).status ==
             k_emu_memmap_ok);
  TEST_CHECK(internal_test_engine_open(&cpu0));
  TEST_CHECK(emu_memmap_attach(&workspace, cpu0).status == k_emu_memmap_ok);
  TEST_CHECK(emu_mem_write(cpu0, k_test_sdram_data, &old_value, 4U) == UC_ERR_OK);
  priv_emu_io_configure(STDOUT_FILENO, STDERR_FILENO, &s_test_ops);
  s_fail_next_pread = true;
  TEST_CHECK(emu_mem_write(cpu0, k_test_sdram_data, &new_value, 4U) != UC_ERR_OK);
  TEST_CHECK(emu_memmap_is_poisoned(&workspace));
  TEST_CHECK(internal_test_fd_word(&workspace, 1U, 0x200) == old_value);
  TEST_CHECK(internal_test_view_word(cpu0, k_test_sdram_data) == old_value);
  priv_emu_io_configure(STDOUT_FILENO, STDERR_FILENO, nullptr);
  TEST_CHECK(emu_memmap_detach(&workspace, cpu0));
  TEST_CHECK(uc_close(cpu0) == UC_ERR_OK);
  TEST_CHECK(emu_memmap_close(&workspace));
  TEST_CHECK(internal_test_guards_valid(&guarded));
  return 0;
}

/**
 * @brief Prove failed guest stores never tear authoritative or guest state.
 * @details Prove failed guest stores never tear authoritative or guest state; this step is contained within the emu memmap test fixture and uses bounded caller or module-owned storage.
 * @return The test guest fault atomicity result produced by the emu memmap test fixture.
 * @retval value The operation-specific test guest fault atomicity value.
 * @pre Arguments satisfy the ranges documented for test guest fault atomicity. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu memmap test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_test_guest_fault_atomicity(void)
{
  test_guarded_scratch_t guarded   = {};
  emu_memmap_workspace_t workspace = {};
  TEST_CHECK(emu_memmap_open(internal_test_scratch_reset(&guarded), 4096U, &workspace).status ==
             k_emu_memmap_ok);
  uc_engine* uc = nullptr;
  TEST_CHECK(internal_test_engine_open(&uc));
  TEST_CHECK(emu_memmap_attach(&workspace, uc).status == k_emu_memmap_ok);
  const uint32_t old_value   = 0x13579BDFU;
  const uint16_t instruction = (uint16_t)k_test_thumb_str_r0r1;
  TEST_CHECK(emu_mem_write(uc, k_test_sram_data, &old_value, 4U) == UC_ERR_OK);
  TEST_CHECK(emu_mem_write(uc, k_test_sram_code, &instruction, sizeof(instruction)) == UC_ERR_OK);
  priv_emu_io_configure(STDOUT_FILENO, STDERR_FILENO, &s_test_ops);
  s_fail_next_pwrite = true;
  (void)internal_test_run_store(uc, k_test_sram_code, k_test_sram_data, 0x2468ACE0U);
  TEST_CHECK(emu_memmap_is_poisoned(&workspace));
  TEST_CHECK(emu_memmap_reconcile(&workspace));
  TEST_CHECK(internal_test_fd_word(&workspace, 0U, 0x200) == old_value);
  TEST_CHECK(internal_test_view_word(uc, k_test_sram_data) == old_value);
  priv_emu_io_configure(STDOUT_FILENO, STDERR_FILENO, nullptr);
  TEST_CHECK(emu_memmap_detach(&workspace, uc));
  TEST_CHECK(uc_close(uc) == UC_ERR_OK);
  TEST_CHECK(emu_memmap_close(&workspace));
  TEST_CHECK(internal_test_guards_valid(&guarded));
  return 0;
}

/** @brief Run the complete authoritative sparse-memory contract matrix. */
int main(void)
{
  int result = internal_test_requirements_and_sparse_open();
  if (result == 0) {
    result = internal_test_alias_execution_and_lifetime();
  }
  if (result == 0) {
    result = internal_test_independent_instances();
  }
  if (result == 0) {
    result = internal_test_guest_widths_and_oversize();
  }
  if (result == 0) {
    result = internal_test_metadata_guest_protection();
  }
  if (result == 0) {
    result = internal_test_host_fault_atomicity();
  }
  if (result == 0) {
    result = internal_test_read_and_mirror_faults();
  }
  if (result == 0) {
    result = internal_test_guest_fault_atomicity();
  }
  return result;
}
