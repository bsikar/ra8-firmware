/**
 * @file test_emu_presentation.c
 * @brief Focused raw-fd presentation workspace and provider tests
 * @details Verifies workspace planning, exact descriptor-backed transfers,
 * tile rotation, guard preservation, and deterministic failures under short
 * and faulting injected I/O operations.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <errno.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "board_overlay.h"
#include "board_view.h"
#include "emu_host_io_internal.h"
#include "emu_presentation.h"
#include "emu_view_tile_internal.h"

/** @brief Fixed guard bytes surrounding caller scratch. */
typedef enum : size_t {
  k_guard_bytes = 8U, /**< Canary width on each side. */
} test_limit_t;

/** @brief Aligned scratch plus pre/post canaries. */
typedef union {
  max_align_t alignment; /**< Ensures the bound interior is RGB565-aligned. */
  /** @brief Guarded scratch bytes. */
  uint8_t bytes[k_emu_presentation_max_scratch_bytes + (2U * k_guard_bytes)];
} test_guarded_scratch_t;

/** @brief Legacy memory composite for a 3x2 panel plus minimum sidebar. */
static uint16_t s_legacy[(3U + 520U) * 600U];

/** @brief Return the failing source line without process output. */
#define TEST_CHECK(condition)                                                                      \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      return __LINE__;                                                                             \
    }                                                                                              \
  } while (false)

/**
 * @brief Production read adapter for injected fault tables.
 * @details Production read adapter for injected fault tables; this step is contained within the emu presentation test fixture and uses bounded caller or module-owned storage.
 * @param[in] fd Open raw descriptor used for the transfer.
 * @param[in,out] bytes Byte storage transferred by the operation.
 * @param[in] count Number of elements or bytes to process.
 * @return The test read result produced by the emu presentation test fixture.
 * @retval value The operation-specific test read value.
 * @pre Arguments satisfy the ranges documented for test read. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu presentation test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static ssize_t internal_test_read(int fd, void* bytes, size_t count)
{
  return read(fd, bytes, count);
}

/**
 * @brief Production write adapter for injected fault tables.
 * @details Production write adapter for injected fault tables; this step is contained within the emu presentation test fixture and uses bounded caller or module-owned storage.
 * @param[in] fd Open raw descriptor used for the transfer.
 * @param[in] bytes Byte storage transferred by the operation.
 * @param[in] count Number of elements or bytes to process.
 * @return The test write result produced by the emu presentation test fixture.
 * @retval value The operation-specific test write value.
 * @pre Arguments satisfy the ranges documented for test write. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu presentation test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static ssize_t internal_test_write(int fd, const void* bytes, size_t count)
{
  return write(fd, bytes, count);
}

/**
 * @brief Production positioned-read adapter.
 * @details Production positioned-read adapter; this step is contained within the emu presentation test fixture and uses bounded caller or module-owned storage.
 * @param[in] fd Open raw descriptor used for the transfer.
 * @param[in,out] bytes Byte storage transferred by the operation.
 * @param[in] count Number of elements or bytes to process.
 * @param[in] offset Byte or register offset at which processing begins.
 * @return The test pread result produced by the emu presentation test fixture.
 * @retval value The operation-specific test pread value.
 * @pre Arguments satisfy the ranges documented for test pread. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu presentation test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static ssize_t internal_test_pread(int fd, void* bytes, size_t count, off_t offset)
{
  return pread(fd, bytes, count, offset);
}

/**
 * @brief Production positioned-write adapter.
 * @details Production positioned-write adapter; this step is contained within the emu presentation test fixture and uses bounded caller or module-owned storage.
 * @param[in] fd Open raw descriptor used for the transfer.
 * @param[in] bytes Byte storage transferred by the operation.
 * @param[in] count Number of elements or bytes to process.
 * @param[in] offset Byte or register offset at which processing begins.
 * @return The test pwrite result produced by the emu presentation test fixture.
 * @retval value The operation-specific test pwrite value.
 * @pre Arguments satisfy the ranges documented for test pwrite. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu presentation test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static ssize_t
internal_test_pwrite(int fd, const void* bytes, size_t count, off_t offset)
{
  return pwrite(fd, bytes, count, offset);
}

/**
 * @brief Deterministic positioned-read fault.
 * @details Deterministic positioned-read fault; this step is contained within the emu presentation test fixture and uses bounded caller or module-owned storage.
 * @param[in] fd Open raw descriptor used for the transfer.
 * @param[in,out] bytes Byte storage transferred by the operation.
 * @param[in] count Number of elements or bytes to process.
 * @param[in] offset Byte or register offset at which processing begins.
 * @return The test pread fault result produced by the emu presentation test fixture.
 * @retval value The operation-specific test pread fault value.
 * @pre Arguments satisfy the ranges documented for test pread fault. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu presentation test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static ssize_t
internal_test_pread_fault(int fd, void* bytes, size_t count, off_t offset)
{
  (void)fd;
  (void)bytes;
  (void)count;
  (void)offset;
  errno = EIO;
  return -1;
}

/**
 * @brief Deterministic positioned-write fault.
 * @details Deterministic positioned-write fault; this step is contained within the emu presentation test fixture and uses bounded caller or module-owned storage.
 * @param[in] fd Open raw descriptor used for the transfer.
 * @param[in] bytes Byte storage transferred by the operation.
 * @param[in] count Number of elements or bytes to process.
 * @param[in] offset Byte or register offset at which processing begins.
 * @return The test pwrite fault result produced by the emu presentation test fixture.
 * @retval value The operation-specific test pwrite fault value.
 * @pre Arguments satisfy the ranges documented for test pwrite fault. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu presentation test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static ssize_t
internal_test_pwrite_fault(int fd, const void* bytes, size_t count, off_t offset)
{
  (void)fd;
  (void)bytes;
  (void)count;
  (void)offset;
  errno = EIO;
  return -1;
}

/** @brief Initialize canaries and return the aligned scratch interior. */
RA8_INTERNAL static uint8_t* internal_test_scratch_reset(test_guarded_scratch_t* guarded)
{
  (void)memset(guarded->bytes, 0xA5, sizeof(guarded->bytes));
  return &guarded->bytes[k_guard_bytes];
}

/**
 * @brief Verify neither scratch guard changed.
 * @details Verify neither scratch guard changed; this step is contained within the emu presentation test fixture and uses bounded caller or module-owned storage.
 * @param[in] guarded Guarded input used by the operation.
 * @return The test guards valid result produced by the emu presentation test fixture.
 * @retval true The test guards valid condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for test guards valid. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu presentation test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_test_guards_valid(const test_guarded_scratch_t* guarded)
{
  for (size_t index = 0U; index < k_guard_bytes; index++) {
    if ((guarded->bytes[index] != 0xA5U) ||
        (guarded->bytes[sizeof(guarded->bytes) - 1U - index] != 0xA5U)) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Exercise exact geometry, overflow, and inactive planning.
 * @details Exercise exact geometry, overflow, and inactive planning; this step is contained within the emu presentation test fixture and uses bounded caller or module-owned storage.
 * @return The test requirements result produced by the emu presentation test fixture.
 * @retval value The operation-specific test requirements value.
 * @pre Arguments satisfy the ranges documented for test requirements. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu presentation test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_test_requirements(void)
{
  emu_presentation_spec_t   spec   = {.panel_width  = 4096U,
                                      .panel_height = 4096U,
                                      .rotate_deg   = 90U,
                                      .active       = true};
  emu_presentation_result_t result = emu_presentation_requirements(&spec);
  TEST_CHECK(result.status == k_emu_presentation_ok);
  TEST_CHECK(result.required_surface_bytes == 37814272U);
  TEST_CHECK(result.required_scratch_bytes == 16384U);
  spec.rotate_deg = 0U;
  result          = emu_presentation_requirements(&spec);
  TEST_CHECK(result.required_surface_bytes == 37814272U);
  TEST_CHECK(result.required_scratch_bytes == 9232U);
  spec.active = false;
  result      = emu_presentation_requirements(&spec);
  TEST_CHECK((result.required_surface_bytes == 0U) && (result.required_scratch_bytes == 0U));
  spec.panel_width = SIZE_MAX;
  spec.active      = true;
  result           = emu_presentation_requirements(&spec);
  TEST_CHECK(result.status == k_emu_presentation_overflow);
  spec.panel_width = 4097U;
  result           = emu_presentation_requirements(&spec);
  TEST_CHECK(result.status == k_emu_presentation_invalid);
  return 0;
}

/**
 * @brief Exercise failure-atomic capacity/alignment and max sparse creation.
 * @details Exercise failure-atomic capacity/alignment and max sparse creation; this step is contained within the emu presentation test fixture and uses bounded caller or module-owned storage.
 * @return The test open contracts result produced by the emu presentation test fixture.
 * @retval value The operation-specific test open contracts value.
 * @pre Arguments satisfy the ranges documented for test open contracts. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu presentation test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_test_open_contracts(void)
{
  test_guarded_scratch_t          guarded     = {};
  uint8_t* const                  scratch     = internal_test_scratch_reset(&guarded);
  emu_presentation_spec_t         spec        = {.panel_width  = 3U,
                                                 .panel_height = 2U,
                                                 .rotate_deg   = 0U,
                                                 .active       = true};
  const emu_presentation_result_t requirement = emu_presentation_requirements(&spec);
  emu_presentation_workspace_t    workspace   = {};
  (void)memset(&workspace, 0x5A, sizeof(workspace));
  const emu_presentation_workspace_t saved = workspace;
  emu_presentation_result_t          result =
    emu_presentation_open(&spec, scratch, requirement.required_scratch_bytes - 1U, &workspace);
  TEST_CHECK(result.status == k_emu_presentation_capacity);
  TEST_CHECK(memcmp(&workspace, &saved, sizeof(saved)) == 0);
  TEST_CHECK(internal_test_guards_valid(&guarded));
  result =
    emu_presentation_open(&spec, &scratch[1], requirement.required_scratch_bytes, &workspace);
  TEST_CHECK(result.status == k_emu_presentation_invalid);
  TEST_CHECK(memcmp(&workspace, &saved, sizeof(saved)) == 0);

  spec.panel_width  = 4096U;
  spec.panel_height = 4096U;
  spec.rotate_deg   = 90U;
  result = emu_presentation_open(&spec, scratch, k_emu_presentation_max_scratch_bytes, &workspace);
  TEST_CHECK(result.status == k_emu_presentation_ok);
  struct stat metadata = {};
  TEST_CHECK((fstat(workspace.fd, &metadata) == 0) && (metadata.st_size == 37814272));
  TEST_CHECK(emu_presentation_close(&workspace));
  TEST_CHECK(emu_presentation_close(&workspace));

  spec.active = false;
  result      = emu_presentation_open(&spec, nullptr, 0U, &workspace);
  TEST_CHECK((result.status == k_emu_presentation_ok) && (workspace.fd == -1));
  TEST_CHECK(emu_presentation_close(&workspace));
  return 0;
}

/**
 * @brief Prove two descriptors, close lifetimes, fills, and canaries.
 * @details Prove two descriptors, close lifetimes, fills, and canaries; this step is contained within the emu presentation test fixture and uses bounded caller or module-owned storage.
 * @return The test independent lifetimes result produced by the emu presentation test fixture.
 * @retval value The operation-specific test independent lifetimes value.
 * @pre Arguments satisfy the ranges documented for test independent lifetimes. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu presentation test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_test_independent_lifetimes(void)
{
  test_guarded_scratch_t        first_guarded  = {};
  test_guarded_scratch_t        second_guarded = {};
  uint8_t* const                first_scratch  = internal_test_scratch_reset(&first_guarded);
  uint8_t* const                second_scratch = internal_test_scratch_reset(&second_guarded);
  const emu_presentation_spec_t spec           = {.panel_width  = 3U,
                                                  .panel_height = 2U,
                                                  .rotate_deg   = 0U,
                                                  .active       = true};
  emu_presentation_workspace_t  first          = {.fd = -1};
  emu_presentation_workspace_t  second         = {.fd = -1};
  TEST_CHECK(
    emu_presentation_open(&spec, first_scratch, k_emu_presentation_max_scratch_bytes, &first)
      .status == k_emu_presentation_ok);
  TEST_CHECK(
    emu_presentation_open(&spec, second_scratch, k_emu_presentation_max_scratch_bytes, &second)
      .status == k_emu_presentation_ok);
  TEST_CHECK(first.fd != second.fd);
  TEST_CHECK(emu_presentation_fill(&first, 0U, 0U, 1U, 1U, 0xF800U));
  TEST_CHECK(emu_presentation_fill(&second, 0U, 0U, 1U, 1U, 0x001FU));
  uint16_t first_pixel  = 0U;
  uint16_t second_pixel = 0U;
  TEST_CHECK(emu_presentation_read(&first, 0U, &first_pixel, sizeof(first_pixel)));
  TEST_CHECK(emu_presentation_read(&second, 0U, &second_pixel, sizeof(second_pixel)));
  TEST_CHECK((first_pixel == 0xF800U) && (second_pixel == 0x001FU));
  TEST_CHECK(emu_presentation_close(&first));
  TEST_CHECK(emu_presentation_read(&second, 0U, &second_pixel, sizeof(second_pixel)));
  TEST_CHECK(emu_presentation_close(&second));
  TEST_CHECK(internal_test_guards_valid(&first_guarded) &&
             internal_test_guards_valid(&second_guarded));
  return 0;
}

/**
 * @brief Prove the fd sink is pixel-identical to the legacy memory composer.
 * @details Prove the fd sink is pixel-identical to the legacy memory composer; this step is contained within the emu presentation test fixture and uses bounded caller or module-owned storage.
 * @return The test overlay equivalence result produced by the emu presentation test fixture.
 * @retval value The operation-specific test overlay equivalence value.
 * @pre Arguments satisfy the ranges documented for test overlay equivalence. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu presentation test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_test_overlay_equivalence(void)
{
  test_guarded_scratch_t        guarded   = {};
  uint8_t* const                scratch   = internal_test_scratch_reset(&guarded);
  const emu_presentation_spec_t spec      = {.panel_width  = 3U,
                                             .panel_height = 2U,
                                             .rotate_deg   = 0U,
                                             .active       = true};
  emu_presentation_workspace_t  workspace = {.fd = -1};
  TEST_CHECK(emu_presentation_open(&spec, scratch, k_emu_presentation_max_scratch_bytes, &workspace)
               .status == k_emu_presentation_ok);
  const uint16_t panel[6] = {0x0000U, 0xFFFFU, 0xF800U, 0x07E0U, 0x001FU, 0x39E7U};
  board_status_t status   = {};
  board_overlay_compose(s_legacy, panel, 3U, 2U, &status);
  TEST_CHECK(emu_presentation_fill(&workspace,
                                   0U,
                                   0U,
                                   workspace.composite_width,
                                   workspace.composite_height,
                                   0U));
  for (uint16_t row = 0U; row < 2U; row++) {
    const size_t offset = (size_t)row * workspace.composite_width * sizeof(uint16_t);
    TEST_CHECK(
      emu_presentation_write(&workspace, offset, &panel[(size_t)row * 3U], 3U * sizeof(uint16_t)));
  }
  board_overlay_surface_t surface = {.context = &workspace,
                                     .fill    = emu_presentation_fill,
                                     .width   = workspace.composite_width,
                                     .height  = workspace.composite_height,
                                     .ok      = true};
  TEST_CHECK(board_overlay_draw_sidebar(&surface, 3U, &status));
  uint8_t              actual[4096];
  const uint8_t* const expected = (const uint8_t*)s_legacy;
  for (size_t offset = 0U; offset < workspace.surface_bytes;) {
    const size_t count = ((workspace.surface_bytes - offset) < sizeof(actual))
                           ? (workspace.surface_bytes - offset)
                           : sizeof(actual);
    TEST_CHECK(emu_presentation_read(&workspace, offset, actual, count));
    TEST_CHECK(memcmp(actual, &expected[offset], count) == 0);
    offset += count;
  }
  TEST_CHECK(internal_test_guards_valid(&guarded));
  TEST_CHECK(emu_presentation_close(&workspace));
  return 0;
}

/**
 * @brief Prove every bounded tile rotation and absolute display placement.
 * @details Prove every bounded tile rotation and absolute display placement; this step is contained within the emu presentation test fixture and uses bounded caller or module-owned storage.
 * @return The test tile rotations result produced by the emu presentation test fixture.
 * @retval value The operation-specific test tile rotations value.
 * @pre Arguments satisfy the ranges documented for test tile rotations. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu presentation test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_test_tile_rotations(void)
{
  static const uint16_t expected[4][6] = {
    {1U, 2U, 3U, 4U, 5U, 6U},
    {4U, 1U, 5U, 2U, 6U, 3U},
    {6U, 5U, 4U, 3U, 2U, 1U},
    {3U, 6U, 2U, 5U, 1U, 4U},
  };
  static const uint32_t rotations[4] = {0U, 90U, 180U, 270U};
  for (size_t vector = 0U; vector < 4U; vector++) {
    test_guarded_scratch_t        guarded   = {};
    uint8_t* const                scratch   = internal_test_scratch_reset(&guarded);
    const emu_presentation_spec_t spec      = {.panel_width  = 3U,
                                               .panel_height = 2U,
                                               .rotate_deg   = rotations[vector],
                                               .active       = true};
    emu_presentation_workspace_t  workspace = {.fd = -1};
    TEST_CHECK(
      emu_presentation_open(&spec, scratch, k_emu_presentation_max_scratch_bytes, &workspace)
        .status == k_emu_presentation_ok);
    TEST_CHECK(emu_presentation_fill(&workspace,
                                     0U,
                                     0U,
                                     workspace.composite_width,
                                     workspace.composite_height,
                                     0U));
    const uint16_t source[6] = {1U, 2U, 3U, 4U, 5U, 6U};
    (void)memcpy(workspace.scratch, source, sizeof(source));
    TEST_CHECK(priv_emu_view_tile_write(&workspace,
                                        (const uint16_t*)(const void*)workspace.scratch,
                                        0U,
                                        0U,
                                        3U,
                                        2U));
    uint16_t actual[6] = {};
    for (uint16_t row = 0U; row < workspace.display_height; row++) {
      const size_t count  = (size_t)workspace.display_width * sizeof(uint16_t);
      const size_t offset = (size_t)row * workspace.composite_width * sizeof(uint16_t);
      TEST_CHECK(emu_presentation_read(&workspace,
                                       offset,
                                       &actual[(size_t)row * workspace.display_width],
                                       count));
    }
    TEST_CHECK(memcmp(actual, expected[vector], sizeof(actual)) == 0);
    TEST_CHECK(internal_test_guards_valid(&guarded));
    TEST_CHECK(emu_presentation_close(&workspace));
  }
  return 0;
}

/**
 * @brief Prove provider byte ranges read an immutable descriptor snapshot.
 * @details Prove provider byte ranges read an immutable descriptor snapshot; this step is contained within the emu presentation test fixture and uses bounded caller or module-owned storage.
 * @return The test snapshot provider result produced by the emu presentation test fixture.
 * @retval value The operation-specific test snapshot provider value.
 * @pre Arguments satisfy the ranges documented for test snapshot provider. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu presentation test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_test_snapshot_provider(void)
{
  test_guarded_scratch_t        guarded   = {};
  uint8_t* const                scratch   = internal_test_scratch_reset(&guarded);
  const emu_presentation_spec_t spec      = {.panel_width  = 3U,
                                             .panel_height = 2U,
                                             .rotate_deg   = 0U,
                                             .active       = true};
  emu_presentation_workspace_t  workspace = {.fd = -1};
  TEST_CHECK(emu_presentation_open(&spec, scratch, k_emu_presentation_max_scratch_bytes, &workspace)
               .status == k_emu_presentation_ok);
  const uint16_t original[5] = {0x0000U, 0xFFFFU, 0xF800U, 0x07E0U, 0x001FU};
  TEST_CHECK(emu_presentation_write(&workspace, 0U, original, sizeof(original)));
  int snapshot = -1;
  TEST_CHECK(emu_presentation_snapshot(&workspace, &snapshot));
  const uint16_t replacement[5] = {};
  TEST_CHECK(emu_presentation_write(&workspace, 0U, replacement, sizeof(replacement)));
  const uint32_t expected[5] = {0x00000000U, 0x00FFFFFFU, 0x00FF0000U, 0x0000FF00U, 0x000000FFU};
  uint32_t       full[5]     = {};
  TEST_CHECK(board_view_read_rgb888_fd(snapshot, 0, full, sizeof(full)) == sizeof(full));
  TEST_CHECK(memcmp(full, expected, sizeof(expected)) == 0);
  uint8_t partial[11] = {};
  TEST_CHECK(board_view_read_rgb888_fd(snapshot, 1, partial, sizeof(partial)) == sizeof(partial));
  TEST_CHECK(memcmp(partial, &((const uint8_t*)expected)[1], sizeof(partial)) == 0);
  TEST_CHECK(close(snapshot) == 0);
  TEST_CHECK(emu_presentation_close(&workspace));
  return 0;
}

/**
 * @brief Prove injected raw faults remain bounded and preserve destinations.
 * @details Prove injected raw faults remain bounded and preserve destinations; this step is contained within the emu presentation test fixture and uses bounded caller or module-owned storage.
 * @return The test faults result produced by the emu presentation test fixture.
 * @retval value The operation-specific test faults value.
 * @pre Arguments satisfy the ranges documented for test faults. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu presentation test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_test_faults(void)
{
  test_guarded_scratch_t        guarded   = {};
  uint8_t* const                scratch   = internal_test_scratch_reset(&guarded);
  const emu_presentation_spec_t spec      = {.panel_width  = 3U,
                                             .panel_height = 2U,
                                             .rotate_deg   = 0U,
                                             .active       = true};
  emu_presentation_workspace_t  workspace = {.fd = -1};
  TEST_CHECK(emu_presentation_open(&spec, scratch, k_emu_presentation_max_scratch_bytes, &workspace)
               .status == k_emu_presentation_ok);
  const emu_io_ops_t fault_ops = {.read_fn   = internal_test_read,
                                  .write_fn  = internal_test_write,
                                  .pread_fn  = internal_test_pread_fault,
                                  .pwrite_fn = internal_test_pwrite_fault};
  priv_emu_io_configure(STDOUT_FILENO, STDERR_FILENO, &fault_ops);
  TEST_CHECK(!emu_presentation_fill(&workspace, 0U, 0U, 3U, 1U, 0xFFFFU));
  uint8_t destination[16];
  (void)memset(destination, 0x5AU, sizeof(destination));
  TEST_CHECK(board_view_read_rgb888_fd(workspace.fd, 0, destination, sizeof(destination)) == 0U);
  for (size_t index = 0U; index < sizeof(destination); index++) {
    TEST_CHECK(destination[index] == 0x5AU);
  }
  const emu_io_ops_t pass_ops = {.read_fn   = internal_test_read,
                                 .write_fn  = internal_test_write,
                                 .pread_fn  = internal_test_pread,
                                 .pwrite_fn = internal_test_pwrite};
  priv_emu_io_configure(STDOUT_FILENO, STDERR_FILENO, &pass_ops);
  TEST_CHECK(internal_test_guards_valid(&guarded));
  TEST_CHECK(emu_presentation_close(&workspace));
  priv_emu_io_configure(STDOUT_FILENO, STDERR_FILENO, nullptr);
  return 0;
}

int main(void)
{
  const int requirements = internal_test_requirements();
  if (requirements != 0) {
    return requirements;
  }
  const int contracts = internal_test_open_contracts();
  if (contracts != 0) {
    return contracts;
  }
  const int lifetimes = internal_test_independent_lifetimes();
  if (lifetimes != 0) {
    return lifetimes;
  }
  const int overlay = internal_test_overlay_equivalence();
  if (overlay != 0) {
    return overlay;
  }
  const int rotations = internal_test_tile_rotations();
  if (rotations != 0) {
    return rotations;
  }
  const int provider = internal_test_snapshot_provider();
  if (provider != 0) {
    return provider;
  }
  return internal_test_faults();
}
