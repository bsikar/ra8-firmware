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
typedef struct {
  /** @brief Guarded scratch bytes. */
  alignas(max_align_t) uint8_t bytes[k_emu_presentation_max_scratch_bytes + (2U * k_guard_bytes)];
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

/** @brief Propagate a nested step's failing source line unchanged. */
#define TEST_STEP(call)                                                                            \
  do {                                                                                             \
    const int step_line = (call);                                                                  \
    if (step_line != 0) {                                                                          \
      return step_line;                                                                            \
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
 * @brief Report whether a workspace descriptor still holds its saved fields.
 * @details Member-wise rather than byte-wise: ::emu_presentation_workspace_t
 * pads between its `uint32_t` head and its pointer, and the struct assignment
 * that snapshots it is not required to carry that padding across -- so a
 * byte-wise comparison would assert something about padding rather than about
 * the descriptor. @param[in] workspace Descriptor observed after the rejected
 * call. @param[in] saved Snapshot taken before it. @return Whether every field
 * is unchanged. @retval true The rejected call wrote nothing. @retval false At
 * least one field moved.
 * @pre Both pointers address initialized descriptors. @pre Neither is null.
 * @post Neither descriptor is modified. @post The result depends only on the
 * eleven descriptor fields.
 * @note Test-only helper with no production ABI. @since 0.1.0
 */
RA8_INTERNAL static bool
internal_test_workspace_unchanged(const emu_presentation_workspace_t* workspace,
                                  const emu_presentation_workspace_t* saved)
{
  if ((workspace->fd != saved->fd) || (workspace->rotate_deg != saved->rotate_deg)) {
    return false;
  }
  if ((workspace->panel_width != saved->panel_width) ||
      (workspace->panel_height != saved->panel_height)) {
    return false;
  }
  if ((workspace->display_width != saved->display_width) ||
      (workspace->display_height != saved->display_height)) {
    return false;
  }
  if ((workspace->composite_width != saved->composite_width) ||
      (workspace->composite_height != saved->composite_height)) {
    return false;
  }
  if ((workspace->scratch != saved->scratch) ||
      (workspace->scratch_bytes != saved->scratch_bytes)) {
    return false;
  }
  return workspace->surface_bytes == saved->surface_bytes;
}

/**
 * @brief Open one active 3x2 surface over freshly guarded caller scratch.
 * @details The whole file exercises the same small panel, so this owns the
 * spec, the canary reset and the open every scenario would otherwise repeat.
 * @param[out] guarded Scratch block reset to its canary pattern here.
 * @param[out] workspace Descriptor initialised by the open.
 * @param[in] rotate_deg Bound display rotation in degrees.
 * @return Zero on success, else the failing source line.
 * @retval 0 The surface is open and bound to @p guarded.
 * @pre @p guarded and @p workspace are distinct writable objects.
 * @pre No surface is currently open on @p workspace.
 * @post Success leaves one descriptor the caller must close.
 * @post The canaries around the scratch prefix are freshly written.
 * @note Test-only helper with no production ABI. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_open_small(test_guarded_scratch_t*       guarded,
                                                 emu_presentation_workspace_t* workspace,
                                                 uint32_t                      rotate_deg)
{
  uint8_t* const                scratch = internal_test_scratch_reset(guarded);
  const emu_presentation_spec_t spec    = {.panel_width  = 3U,
                                           .panel_height = 2U,
                                           .rotate_deg   = rotate_deg,
                                           .active       = true};
  *workspace                            = (emu_presentation_workspace_t){.fd = -1};
  TEST_CHECK(
    emu_presentation_open(&spec, scratch, k_emu_presentation_max_scratch_bytes, workspace).status ==
    k_emu_presentation_ok);
  return 0;
}

/**
 * @brief Prove exact surface and scratch geometry for both rotation classes.
 * @details A 4096x4096 panel needs the same surface either way, but a rotated
 * one needs the larger row-transpose scratch.
 * @return Zero on success, else the failing source line.
 * @retval 0 Every geometry figure matched exactly.
 * @pre The call executes on the emulator's single owning thread.
 * @pre No surface is open in this process.
 * @post No descriptor or host resource is created.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note Test-only helper with no production ABI. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_requirements_geometry(void)
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
  return 0;
}

/**
 * @brief Prove the inactive, overflowing and out-of-range planning answers.
 * @details An inactive panel needs nothing, a SIZE_MAX width overflows, and
 * one pixel past the supported panel is rejected as invalid rather than
 * silently planned.
 * @return Zero on success, else the failing source line.
 * @retval 0 Every edge answer matched exactly.
 * @pre The call executes on the emulator's single owning thread.
 * @pre No surface is open in this process.
 * @post No descriptor or host resource is created.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note Test-only helper with no production ABI. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_requirements_edges(void)
{
  emu_presentation_spec_t   spec   = {.panel_width  = 4096U,
                                      .panel_height = 4096U,
                                      .rotate_deg   = 0U,
                                      .active       = false};
  emu_presentation_result_t result = emu_presentation_requirements(&spec);
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
 * @brief Exercise exact geometry, overflow, and inactive planning.
 * @details Runs the two planning halves in order; both are pure and create no
 * host resources.
 * @return The test requirements result produced by the emu presentation test fixture.
 * @retval 0 Every planning answer matched exactly.
 * @pre Arguments satisfy the ranges documented for test requirements. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu presentation test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_test_requirements(void)
{
  TEST_STEP(internal_test_requirements_geometry());
  TEST_STEP(internal_test_requirements_edges());
  return 0;
}

/**
 * @brief Prove a rejected open leaves the caller destination untouched.
 * @details One byte short of the required scratch, and a misaligned scratch,
 * are both refused -- and neither may write a field of the destination or
 * step outside the bound scratch prefix.
 * @param[in,out] guarded Scratch block whose canaries bound the write.
 * @param[in] scratch Aligned interior of @p guarded.
 * @param[in,out] workspace Poisoned destination that must not move.
 * @param[in] saved Snapshot of the poisoned destination.
 * @return Zero on success, else the failing source line.
 * @retval 0 Both refusals left the destination and the canaries intact.
 * @pre @p workspace holds the poison pattern @p saved records.
 * @pre @p guarded carries freshly written canaries.
 * @post No descriptor is created.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note Test-only helper with no production ABI. @since 0.1.0
 */
RA8_INTERNAL static int
internal_test_open_rejects_bad_scratch(test_guarded_scratch_t*             guarded,
                                       uint8_t*                            scratch,
                                       emu_presentation_workspace_t*       workspace,
                                       const emu_presentation_workspace_t* saved)
{
  const emu_presentation_spec_t   spec        = {.panel_width  = 3U,
                                                 .panel_height = 2U,
                                                 .rotate_deg   = 0U,
                                                 .active       = true};
  const emu_presentation_result_t requirement = emu_presentation_requirements(&spec);
  emu_presentation_result_t       result =
    emu_presentation_open(&spec, scratch, requirement.required_scratch_bytes - 1U, workspace);
  TEST_CHECK(result.status == k_emu_presentation_capacity);
  TEST_CHECK(internal_test_workspace_unchanged(workspace, saved));
  TEST_CHECK(internal_test_guards_valid(guarded));
  result = emu_presentation_open(&spec, &scratch[1], requirement.required_scratch_bytes, workspace);
  TEST_CHECK(result.status == k_emu_presentation_invalid);
  TEST_CHECK(internal_test_workspace_unchanged(workspace, saved));
  return 0;
}

/**
 * @brief Prove the largest supported surface opens and closes idempotently.
 * @details The 4096x4096 rotated surface is the maximum this module plans for;
 * its unlinked descriptor must be exactly the planned length, and closing it
 * twice must be safe.
 * @param[in] scratch Aligned caller scratch of at least the maximum extent.
 * @param[in,out] workspace Destination descriptor.
 * @return Zero on success, else the failing source line.
 * @retval 0 The surface opened, measured and closed twice.
 * @pre @p scratch spans ::k_emu_presentation_max_scratch_bytes.
 * @pre No surface is currently open on @p workspace.
 * @post Every descriptor opened here is closed again.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note Test-only helper with no production ABI. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_open_max_surface(uint8_t*                      scratch,
                                                       emu_presentation_workspace_t* workspace)
{
  const emu_presentation_spec_t   spec = {.panel_width  = 4096U,
                                          .panel_height = 4096U,
                                          .rotate_deg   = 90U,
                                          .active       = true};
  const emu_presentation_result_t result =
    emu_presentation_open(&spec, scratch, k_emu_presentation_max_scratch_bytes, workspace);
  TEST_CHECK(result.status == k_emu_presentation_ok);
  struct stat metadata = {};
  TEST_CHECK((fstat(workspace->fd, &metadata) == 0) && (metadata.st_size == 37814272));
  TEST_CHECK(emu_presentation_close(workspace));
  TEST_CHECK(emu_presentation_close(workspace));
  return 0;
}

/**
 * @brief Prove an inactive panel opens with no descriptor at all.
 * @details An inactive spec needs no scratch and no host object, so the open
 * succeeds while leaving the descriptor field at -1.
 * @param[in,out] workspace Destination descriptor.
 * @return Zero on success, else the failing source line.
 * @retval 0 The inactive open succeeded and closed cleanly.
 * @pre No surface is currently open on @p workspace.
 * @pre The call executes on the emulator's single owning thread.
 * @post No host descriptor is left open.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note Test-only helper with no production ABI. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_open_inactive(emu_presentation_workspace_t* workspace)
{
  const emu_presentation_spec_t   spec   = {.panel_width  = 4096U,
                                            .panel_height = 4096U,
                                            .rotate_deg   = 90U,
                                            .active       = false};
  const emu_presentation_result_t result = emu_presentation_open(&spec, nullptr, 0U, workspace);
  TEST_CHECK((result.status == k_emu_presentation_ok) && (workspace->fd == -1));
  TEST_CHECK(emu_presentation_close(workspace));
  return 0;
}

/**
 * @brief Exercise failure-atomic capacity/alignment and max sparse creation.
 * @details Poisons the destination, proves both refusals are failure-atomic,
 * then opens the maximum surface and the inactive one.
 * @return The test open contracts result produced by the emu presentation test fixture.
 * @retval 0 Every open contract held.
 * @pre Arguments satisfy the ranges documented for test open contracts. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu presentation test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_test_open_contracts(void)
{
  test_guarded_scratch_t       guarded   = {};
  uint8_t* const               scratch   = internal_test_scratch_reset(&guarded);
  emu_presentation_workspace_t workspace = {};
  (void)memset(&workspace, 0x5A, sizeof(workspace));
  const emu_presentation_workspace_t saved = workspace;
  TEST_STEP(internal_test_open_rejects_bad_scratch(&guarded, scratch, &workspace, &saved));
  TEST_STEP(internal_test_open_max_surface(scratch, &workspace));
  TEST_STEP(internal_test_open_inactive(&workspace));
  return 0;
}

/**
 * @brief Prove two open surfaces share no bytes and no lifetime.
 * @details Each surface owns its own descriptor, a fill through one is
 * invisible to the other, and closing one leaves the other readable.
 * @param[in,out] first First open surface.
 * @param[in,out] second Second open surface.
 * @return Zero on success, else the failing source line.
 * @retval 0 The two surfaces stayed independent throughout.
 * @pre Both descriptors are open over distinct scratch blocks.
 * @pre The call executes on the emulator's single owning thread.
 * @post Both surfaces are closed.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note Test-only helper with no production ABI. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_two_surfaces_isolated(emu_presentation_workspace_t* first,
                                                            emu_presentation_workspace_t* second)
{
  TEST_CHECK(first->fd != second->fd);
  TEST_CHECK(emu_presentation_fill(first, 0U, 0U, 1U, 1U, 0xF800U));
  TEST_CHECK(emu_presentation_fill(second, 0U, 0U, 1U, 1U, 0x001FU));
  uint16_t first_pixel  = 0U;
  uint16_t second_pixel = 0U;
  TEST_CHECK(emu_presentation_read(first, 0U, &first_pixel, sizeof(first_pixel)));
  TEST_CHECK(emu_presentation_read(second, 0U, &second_pixel, sizeof(second_pixel)));
  TEST_CHECK((first_pixel == 0xF800U) && (second_pixel == 0x001FU));
  return 0;
}

/**
 * @brief Prove closing one surface leaves the other one readable.
 * @details The two descriptors have independent lifetimes, so the survivor
 * must still serve its pixels after its neighbour is gone.
 * @param[in,out] first Surface closed first.
 * @param[in,out] second Surface that must outlive it.
 * @return Zero on success, else the failing source line.
 * @retval 0 The survivor stayed readable and then closed cleanly.
 * @pre Both descriptors are open.
 * @pre The call executes on the emulator's single owning thread.
 * @post Both surfaces are closed.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note Test-only helper with no production ABI. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_close_one_keeps_other(emu_presentation_workspace_t* first,
                                                            emu_presentation_workspace_t* second)
{
  uint16_t survivor = 0U;
  TEST_CHECK(emu_presentation_close(first));
  TEST_CHECK(emu_presentation_read(second, 0U, &survivor, sizeof(survivor)));
  TEST_CHECK(emu_presentation_close(second));
  return 0;
}

/**
 * @brief Prove two descriptors, close lifetimes, fills, and canaries.
 * @details Opens two independent surfaces over separate guarded scratch and
 * proves neither one can reach the other.
 * @return The test independent lifetimes result produced by the emu presentation test fixture.
 * @retval 0 Both surfaces stayed independent and in bounds.
 * @pre Arguments satisfy the ranges documented for test independent lifetimes. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu presentation test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_test_independent_lifetimes(void)
{
  test_guarded_scratch_t       first_guarded  = {};
  test_guarded_scratch_t       second_guarded = {};
  emu_presentation_workspace_t first          = {.fd = -1};
  emu_presentation_workspace_t second         = {.fd = -1};
  TEST_STEP(internal_test_open_small(&first_guarded, &first, 0U));
  TEST_STEP(internal_test_open_small(&second_guarded, &second, 0U));
  TEST_STEP(internal_test_two_surfaces_isolated(&first, &second));
  TEST_STEP(internal_test_close_one_keeps_other(&first, &second));
  TEST_CHECK(internal_test_guards_valid(&first_guarded) &&
             internal_test_guards_valid(&second_guarded));
  return 0;
}

/**
 * @brief Publish the panel rows the overlay equivalence vector compares.
 * @details Clears the whole composite, then writes each panel row at its exact
 * composite offset.
 * @param[in,out] workspace Open surface receiving the rows.
 * @param[in] panel Six-pixel 3x2 panel source.
 * @return Zero on success, else the failing source line.
 * @retval 0 The composite was cleared and both rows published.
 * @pre @p workspace is open over the 3x2 spec.
 * @pre @p panel addresses six readable pixels.
 * @post The surface holds the panel rows and nothing else.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note Test-only helper with no production ABI. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_overlay_seed(emu_presentation_workspace_t* workspace,
                                                   const uint16_t*               panel)
{
  TEST_CHECK(emu_presentation_fill(workspace,
                                   0U,
                                   0U,
                                   workspace->composite_width,
                                   workspace->composite_height,
                                   0U));
  for (uint16_t row = 0U; row < 2U; row++) {
    const size_t offset = (size_t)row * workspace->composite_width * sizeof(uint16_t);
    TEST_CHECK(
      emu_presentation_write(workspace, offset, &panel[(size_t)row * 3U], 3U * sizeof(uint16_t)));
  }
  return 0;
}

/**
 * @brief Compare the whole raw-fd surface against the legacy composer output.
 * @details Reads the surface back in bounded chunks so the comparison never
 * needs a second full-size buffer.
 * @param[in,out] workspace Open surface holding the composed image.
 * @param[in] expected Legacy composer bytes of the same extent.
 * @return Zero on success, else the failing source line.
 * @retval 0 Every surface byte matched the legacy composer.
 * @pre @p workspace is open and fully composed.
 * @pre @p expected spans at least `workspace->surface_bytes` bytes.
 * @post No surface byte is modified.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note Test-only helper with no production ABI. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_overlay_compare(emu_presentation_workspace_t* workspace,
                                                      const uint8_t*                expected)
{
  uint8_t actual[4096];
  for (size_t offset = 0U; offset < workspace->surface_bytes;) {
    const size_t count = ((workspace->surface_bytes - offset) < sizeof(actual))
                           ? (workspace->surface_bytes - offset)
                           : sizeof(actual);
    TEST_CHECK(emu_presentation_read(workspace, offset, actual, count));
    TEST_CHECK(memcmp(actual, &expected[offset], count) == 0);
    offset += count;
  }
  return 0;
}

/**
 * @brief Prove the fd sink is pixel-identical to the legacy memory composer.
 * @details Composes the same panel and sidebar through both paths and compares
 * every byte of the result.
 * @return The test overlay equivalence result produced by the emu presentation test fixture.
 * @retval 0 The two composers produced identical bytes.
 * @pre Arguments satisfy the ranges documented for test overlay equivalence. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu presentation test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_test_overlay_equivalence(void)
{
  test_guarded_scratch_t       guarded   = {};
  emu_presentation_workspace_t workspace = {.fd = -1};
  TEST_STEP(internal_test_open_small(&guarded, &workspace, 0U));
  const uint16_t panel[6] = {0x0000U, 0xFFFFU, 0xF800U, 0x07E0U, 0x001FU, 0x39E7U};
  board_status_t status   = {};
  board_overlay_compose(s_legacy, panel, 3U, 2U, &status);
  TEST_STEP(internal_test_overlay_seed(&workspace, panel));
  board_overlay_surface_t surface = {.context = &workspace,
                                     .fill    = emu_presentation_fill,
                                     .width   = workspace.composite_width,
                                     .height  = workspace.composite_height,
                                     .ok      = true};
  TEST_CHECK(board_overlay_draw_sidebar(&surface, 3U, &status));
  TEST_STEP(internal_test_overlay_compare(&workspace, (const uint8_t*)s_legacy));
  TEST_CHECK(internal_test_guards_valid(&guarded));
  TEST_CHECK(emu_presentation_close(&workspace));
  return 0;
}

/**
 * @brief Read every display row of one surface into a flat pixel buffer.
 * @details Each row starts at a composite-width stride, so the read walks the
 * rows rather than assuming the display and composite widths agree.
 * @param[in,out] workspace Open surface to read from.
 * @param[out] actual Flat destination of `display_width * display_height`.
 * @return Zero on success, else the failing source line.
 * @retval 0 Every display row was read.
 * @pre @p workspace is open.
 * @pre @p actual spans the whole display area.
 * @post No surface byte is modified.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note Test-only helper with no production ABI. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_read_display_rows(emu_presentation_workspace_t* workspace,
                                                        uint16_t*                     actual)
{
  for (uint16_t row = 0U; row < workspace->display_height; row++) {
    const size_t count  = (size_t)workspace->display_width * sizeof(uint16_t);
    const size_t offset = (size_t)row * workspace->composite_width * sizeof(uint16_t);
    TEST_CHECK(emu_presentation_read(workspace,
                                     offset,
                                     &actual[(size_t)row * workspace->display_width],
                                     count));
  }
  return 0;
}

/**
 * @brief Clear the composite and publish the six-pixel tile through scratch.
 * @details The tile is staged in the bound caller scratch first, because that
 * is the path the production tile writer really reads from.
 * @param[in,out] workspace Open surface receiving the tile.
 * @return Zero on success, else the failing source line.
 * @retval 0 The composite was cleared and the tile written.
 * @pre @p workspace is open over the 3x2 spec.
 * @pre The bound scratch spans at least six pixels.
 * @post The surface holds the rotated tile and nothing else.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note Test-only helper with no production ABI. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_write_tile(emu_presentation_workspace_t* workspace)
{
  TEST_CHECK(emu_presentation_fill(workspace,
                                   0U,
                                   0U,
                                   workspace->composite_width,
                                   workspace->composite_height,
                                   0U));
  const uint16_t source[6] = {1U, 2U, 3U, 4U, 5U, 6U};
  (void)memcpy(workspace->scratch, source, sizeof(source));
  TEST_CHECK(priv_emu_view_tile_write(workspace, workspace->scratch, 0U, 0U, 3U, 2U));
  return 0;
}

/**
 * @brief Prove one bounded tile rotation places every pixel exactly.
 * @details Writes the same six-pixel tile through the rotation under test and
 * reads the display rows back into one flat buffer for comparison.
 * @param[in] rotate_deg Rotation the surface is opened with.
 * @param[in] expected Six pixels in display order for this rotation.
 * @return Zero on success, else the failing source line.
 * @retval 0 The rotated placement matched exactly.
 * @pre @p expected addresses six readable pixels.
 * @pre The call executes on the emulator's single owning thread.
 * @post Every descriptor opened here is closed again.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note Test-only helper with no production ABI. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_tile_rotation_vector(uint32_t        rotate_deg,
                                                           const uint16_t* expected)
{
  test_guarded_scratch_t       guarded   = {};
  emu_presentation_workspace_t workspace = {.fd = -1};
  TEST_STEP(internal_test_open_small(&guarded, &workspace, rotate_deg));
  TEST_STEP(internal_test_write_tile(&workspace));
  uint16_t actual[6] = {};
  TEST_STEP(internal_test_read_display_rows(&workspace, actual));
  TEST_CHECK(memcmp(actual, expected, sizeof(actual)) == 0);
  TEST_CHECK(internal_test_guards_valid(&guarded));
  TEST_CHECK(emu_presentation_close(&workspace));
  return 0;
}

/**
 * @brief Prove every bounded tile rotation and absolute display placement.
 * @details Drives the four supported rotations through the same tile.
 * @return The test tile rotations result produced by the emu presentation test fixture.
 * @retval 0 Every rotation placed every pixel exactly.
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
    TEST_STEP(internal_test_tile_rotation_vector(rotations[vector], expected[vector]));
  }
  return 0;
}

/**
 * @brief Write the original pixels, snapshot them, then overwrite the surface.
 * @details The snapshot must be taken from the live surface and must not be
 * disturbed by the write that follows it.
 * @param[in,out] workspace Open surface to snapshot.
 * @param[out] snapshot Receives the snapshot descriptor.
 * @return Zero on success, else the failing source line.
 * @retval 0 The snapshot was taken and the surface overwritten.
 * @pre @p workspace is open.
 * @pre @p snapshot is a writable descriptor slot.
 * @post Success leaves one descriptor the caller must close.
 * @post The live surface holds the replacement pixels.
 * @note Test-only helper with no production ABI. @since 0.1.0
 */
RA8_INTERNAL static int
internal_test_snapshot_then_overwrite(emu_presentation_workspace_t* workspace, int* snapshot)
{
  const uint16_t original[5] = {0x0000U, 0xFFFFU, 0xF800U, 0x07E0U, 0x001FU};
  TEST_CHECK(emu_presentation_write(workspace, 0U, original, sizeof(original)));
  TEST_CHECK(emu_presentation_snapshot(workspace, snapshot));
  const uint16_t replacement[5] = {};
  TEST_CHECK(emu_presentation_write(workspace, 0U, replacement, sizeof(replacement)));
  return 0;
}

/**
 * @brief Prove the snapshot descriptor still serves the bytes it froze.
 * @details Reads the whole RGB888 conversion and an unaligned partial range
 * out of the snapshot after the live surface has been overwritten.
 * @param[in] snapshot Open snapshot descriptor.
 * @param[in] expected Five RGB888 words the snapshot must still hold.
 * @return Zero on success, else the failing source line.
 * @retval 0 Both reads returned the frozen bytes.
 * @pre @p snapshot is an open readable descriptor.
 * @pre @p expected addresses five readable words.
 * @post No descriptor is closed here.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note Test-only helper with no production ABI. @since 0.1.0
 */
RA8_INTERNAL static int internal_test_snapshot_reads(int snapshot, const uint32_t* expected)
{
  uint32_t full[5] = {};
  TEST_CHECK(board_view_read_rgb888_fd(snapshot, 0, full, sizeof(full)) == sizeof(full));
  TEST_CHECK(memcmp(full, expected, sizeof(full)) == 0);
  uint8_t partial[11] = {};
  TEST_CHECK(board_view_read_rgb888_fd(snapshot, 1, partial, sizeof(partial)) == sizeof(partial));
  TEST_CHECK(memcmp(partial, &((const uint8_t*)expected)[1], sizeof(partial)) == 0);
  return 0;
}

/**
 * @brief Prove provider byte ranges read an immutable descriptor snapshot.
 * @details Takes a snapshot, overwrites the live surface, and proves the
 * snapshot still serves the original pixels.
 * @return The test snapshot provider result produced by the emu presentation test fixture.
 * @retval 0 The snapshot stayed immutable.
 * @pre Arguments satisfy the ranges documented for test snapshot provider. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu presentation test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_test_snapshot_provider(void)
{
  test_guarded_scratch_t       guarded   = {};
  emu_presentation_workspace_t workspace = {.fd = -1};
  TEST_STEP(internal_test_open_small(&guarded, &workspace, 0U));
  int snapshot = -1;
  TEST_STEP(internal_test_snapshot_then_overwrite(&workspace, &snapshot));
  const uint32_t expected[5] = {0x00000000U, 0x00FFFFFFU, 0x00FF0000U, 0x0000FF00U, 0x000000FFU};
  TEST_STEP(internal_test_snapshot_reads(snapshot, expected));
  TEST_CHECK(close(snapshot) == 0);
  TEST_CHECK(emu_presentation_close(&workspace));
  return 0;
}

/**
 * @brief Prove an injected raw fault preserves the caller destination.
 * @details A faulting fill fails, the provider read returns nothing, and every
 * byte of the caller destination keeps its poison pattern.
 * @param[in,out] workspace Open surface bound to the faulting I/O ops.
 * @return Zero on success, else the failing source line.
 * @retval 0 The fault was bounded and wrote nothing.
 * @pre The faulting I/O ops are installed.
 * @pre @p workspace is open.
 * @post No destination byte is modified.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note Test-only helper with no production ABI. @since 0.1.0
 */
RA8_INTERNAL static int
internal_test_fault_preserves_destination(emu_presentation_workspace_t* workspace)
{
  TEST_CHECK(!emu_presentation_fill(workspace, 0U, 0U, 3U, 1U, 0xFFFFU));
  uint8_t destination[16];
  (void)memset(destination, 0x5AU, sizeof(destination));
  TEST_CHECK(board_view_read_rgb888_fd(workspace->fd, 0, destination, sizeof(destination)) == 0U);
  for (size_t index = 0U; index < sizeof(destination); index++) {
    TEST_CHECK(destination[index] == 0x5AU);
  }
  return 0;
}

/**
 * @brief Prove injected raw faults remain bounded and preserve destinations.
 * @details Installs faulting raw I/O around one surface, proves the failure is
 * contained, and restores the passthrough ops before closing.
 * @return The test faults result produced by the emu presentation test fixture.
 * @retval 0 Every injected fault stayed bounded.
 * @pre Arguments satisfy the ranges documented for test faults. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu presentation test fixture and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_test_faults(void)
{
  test_guarded_scratch_t       guarded   = {};
  emu_presentation_workspace_t workspace = {.fd = -1};
  TEST_STEP(internal_test_open_small(&guarded, &workspace, 0U));
  const emu_io_ops_t fault_ops = {.read_fn   = internal_test_read,
                                  .write_fn  = internal_test_write,
                                  .pread_fn  = internal_test_pread_fault,
                                  .pwrite_fn = internal_test_pwrite_fault};
  priv_emu_io_configure(STDOUT_FILENO, STDERR_FILENO, &fault_ops);
  const int          fault_result = internal_test_fault_preserves_destination(&workspace);
  const emu_io_ops_t pass_ops     = {.read_fn   = internal_test_read,
                                     .write_fn  = internal_test_write,
                                     .pread_fn  = internal_test_pread,
                                     .pwrite_fn = internal_test_pwrite};
  priv_emu_io_configure(STDOUT_FILENO, STDERR_FILENO, &pass_ops);
  TEST_CHECK(fault_result == 0);
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
