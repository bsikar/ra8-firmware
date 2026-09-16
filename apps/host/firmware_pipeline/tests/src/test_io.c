// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

/**
 * @file test_io.c
 * @brief Always-active fault-transition tests for C-owned resources.
 * @details Injects each hosted operation failure and proves exact close/release counts.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "firmware_pipeline_io_internal.h"

/**
 * @enum failure_stage_t
 * @brief Single injected operation failure.
 * @details Each value isolates one acquired-resource transition.
 * @invariant Exactly one stage is selected per fixture.
 * @code
 * failure_stage_t stage = k_failure_open;
 * @endcode
 * @see run_failure
 */
typedef enum : uint8_t {
  k_failure_none,        /**< No operation fails.                      */
  k_failure_open,        /**< Opening fails before acquisition.        */
  k_failure_seek_end,    /**< Initial seek fails after open.           */
  k_failure_tell,        /**< Position query fails after open.         */
  k_failure_oversize,    /**< Position exceeds the bounded limit.      */
  k_failure_seek_start,  /**< Rewind fails after measurement.          */
  k_failure_allocate,    /**< Allocation fails while the file is open. */
  k_failure_read,        /**< Read fails after allocation.             */
  k_failure_close,       /**< Final close reports failure.             */
  k_empty_success,       /**< Empty input succeeds without allocation. */
  k_empty_close_failure, /**< Empty input whose close reports failure. */
} failure_stage_t;

/**
 * @enum fake_value_t
 * @brief Named deterministic values used by the fake provider.
 * @details Keeps failure behavior readable and independent of production constants.
 * @invariant Values fit in a signed long.
 * @code
 * long measured = k_fake_image_size;
 * @endcode
 * @see fake_tell
 */
typedef enum : int32_t {
  k_fake_image_size = 4L,                         /**< Successful image size.     */
  k_fake_oversize   = (16L * 1024L * 1024L) + 1L, /**< One byte beyond the limit. */
  k_fake_fill       = 0x5aL,                      /**< Successful read fill byte. */
} fake_value_t;

/**
 * @struct fake_io_t
 * @brief Deterministic fake provider state and resource counters.
 * @details Owns a fixed allocation stand-in and records every lifecycle callback.
 * @invariant Counters begin at zero and only their matching callbacks increment them.
 * @code
 * fake_io_t fake = {};
 * @endcode
 * @see make_ops
 */
typedef struct {
  failure_stage_t stage;                      /**< Injected failure stage.    */
  unsigned int    open_count;                 /**< Open calls observed.       */
  unsigned int    seek_count;                 /**< Seek calls observed.       */
  unsigned int    allocate_count;             /**< Allocation calls observed. */
  unsigned int    read_count;                 /**< Read calls observed.       */
  unsigned int    close_count;                /**< Close calls observed.      */
  unsigned int    release_count;              /**< Release calls observed.    */
  uint8_t         storage[k_fake_image_size]; /**< Fixed allocation stand-in. */
} fake_io_t;

/**
 * @brief Abort when an always-active test requirement is false.
 * @param[in] condition Requirement result.
 * @pre None.
 * @post Returns only when the condition is true.
 * @note Calls abort on failure in every build mode.
 * @since 0.1.0
 */
static void require(bool condition)
{
  if (!condition) {
    abort();
  }
}

/**
 * @brief Open the deterministic fake handle.
 * @details Counts the call and injects only the selected open failure.
 * @param[in,out] context Writable fake provider state.
 * @param[in] path Candidate path.
 * @return Fake handle or null on the selected failure.
 * @pre `context` names initialized fake state.
 * @post Open count increments exactly once.
 * @note Retains no path pointer.
 * @since 0.1.0
 */
static void* fake_open(void* context, const char* path)
{
  fake_io_t* fake = context;
  ++fake->open_count;
  return (fake->stage == k_failure_open) || (path == nullptr) ? nullptr : fake;
}

/**
 * @brief Reposition the deterministic fake handle.
 * @details Injects independent end-seek and rewind failures.
 * @param[in,out] context Writable fake provider state.
 * @param[in] file Fake open handle.
 * @param[in] offset Requested offset.
 * @param[in] origin Hosted seek origin.
 * @return Zero or the selected failure value.
 * @retval 0 Reposition succeeded.
 * @retval -1 Selected reposition failed.
 * @pre `context` and `file` name the same fake.
 * @post Seek count increments exactly once.
 * @note Performs no hosted I/O.
 * @since 0.1.0
 */
static int fake_seek(void* context, void* file, long offset, int origin)
{
  fake_io_t* fake = context;
  (void)file;
  (void)offset;
  ++fake->seek_count;
  return ((fake->stage == k_failure_seek_end) && (origin == SEEK_END)) ||
             ((fake->stage == k_failure_seek_start) && (origin == SEEK_SET))
           ? -1
           : 0;
}

/**
 * @brief Return the selected deterministic image size.
 * @details Produces normal, empty, oversize, or failure measurements.
 * @param[in] context Readable fake provider state.
 * @param[in] file Fake open handle.
 * @return Selected signed measurement.
 * @retval -1 Selected measurement failure.
 * @pre `context` and `file` name the same fake.
 * @post Fake state and ownership are unchanged.
 * @note Performs no hosted I/O.
 * @since 0.1.0
 */
static long fake_tell(void* context, void* file)
{
  const fake_io_t* fake = context;
  (void)file;
  if (fake->stage == k_failure_tell) {
    return -1L;
  }
  if ((fake->stage == k_empty_success) || (fake->stage == k_empty_close_failure)) {
    return 0L;
  }
  return fake->stage == k_failure_oversize ? k_fake_oversize : k_fake_image_size;
}

/**
 * @brief Return the fixed fake allocation.
 * @details Counts the call and injects the selected allocation failure.
 * @param[in,out] context Writable fake provider state.
 * @param[in] size Exact requested size.
 * @return Fixed storage or null on the selected failure.
 * @pre `size` equals the fixed storage size.
 * @post Allocation count increments exactly once.
 * @note Performs no hosted allocation.
 * @since 0.1.0
 */
static void* fake_allocate(void* context, size_t size)
{
  fake_io_t* fake = context;
  ++fake->allocate_count;
  require(size == sizeof(fake->storage));
  return fake->stage == k_failure_allocate ? nullptr : fake->storage;
}

/**
 * @brief Fill the fixed fake allocation.
 * @details Counts the call and injects a short read when selected.
 * @param[in,out] context Writable fake provider state.
 * @param[out] bytes Writable fixed storage.
 * @param[in] size Exact requested size.
 * @param[in] file Fake open handle.
 * @return Complete or deliberately short byte count.
 * @pre `bytes` names `size` writable fake bytes.
 * @post Read count increments and storage is initialized.
 * @note Performs no hosted I/O.
 * @since 0.1.0
 */
static size_t fake_read(void* context, void* bytes, size_t size, void* file)
{
  fake_io_t* fake = context;
  ++fake->read_count;
  (void)file;
  (void)memset(bytes, k_fake_fill, size);
  return fake->stage == k_failure_read ? size - 1U : size;
}

/**
 * @brief End ownership of the fake handle.
 * @details Counts the call and injects the selected close report.
 * @param[in,out] context Writable fake provider state.
 * @param[in] file Fake open handle.
 * @return Zero or the selected failure value.
 * @retval 0 Close succeeded.
 * @retval -1 Selected close failed.
 * @pre `context` and `file` name the same fake.
 * @post Close count increments exactly once.
 * @note Handle ownership ends for either result.
 * @since 0.1.0
 */
static int fake_close(void* context, void* file)
{
  fake_io_t* fake = context;
  (void)file;
  ++fake->close_count;
  return ((fake->stage == k_failure_close) || (fake->stage == k_empty_close_failure)) ? -1 : 0;
}

/**
 * @brief End ownership of the fixed fake allocation.
 * @details Verifies the pointer and counts exactly one release.
 * @param[in,out] context Writable fake provider state.
 * @param[in] bytes Fixed fake storage.
 * @pre `bytes` is the provider's storage address.
 * @post Release count increments exactly once.
 * @note Performs no hosted deallocation.
 * @since 0.1.0
 */
static void fake_release(void* context, void* bytes)
{
  fake_io_t* fake = context;
  require(bytes == fake->storage);
  ++fake->release_count;
}

/**
 * @brief Construct a complete operation table for one fake.
 * @details Binds every callback to the same caller-owned state.
 * @param[in,out] fake Writable fake state with process-lifetime test scope.
 * @return Complete operation table by value.
 * @pre `fake` remains alive for every operation call.
 * @post No operation has executed and no resource is owned.
 * @note The returned table retains only the fake pointer.
 * @since 0.1.0
 */
static firmware_pipeline_io_ops_t make_ops(fake_io_t* fake)
{
  return (firmware_pipeline_io_ops_t){
    .context  = fake,
    .open     = fake_open,
    .seek     = fake_seek,
    .tell     = fake_tell,
    .allocate = fake_allocate,
    .read     = fake_read,
    .close    = fake_close,
    .release  = fake_release,
  };
}

/**
 * @brief Prove an invalid provider table is rejected before acquisition.
 * @details Checks unchanged output and zero open calls for one incomplete table.
 * @param[in] ops Incomplete provider table.
 * @param[in] fake Writable provider counters.
 * @pre `ops` names a table with at least one null callback.
 * @pre `fake` begins with zero open calls.
 * @post Output remains unchanged.
 * @post No resource operation executes.
 * @note Calls abort on a contract violation.
 * @since 0.1.0
 */
static void require_invalid_ops(const firmware_pipeline_io_ops_t* ops, fake_io_t* fake)
{
  firmware_pipeline_image_t       image = {};
  const firmware_pipeline_image_t saved = image;
  require(priv_firmware_pipeline_read_image(ops, "image.bin", &image) ==
          k_firmware_pipeline_io_failed);
  require(memcmp(&image, &saved, sizeof(image)) == 0);
  require(fake->open_count == 0U);
}

/**
 * @brief Exercise every independently required provider callback.
 * @details Removes one callback at a time and proves pre-acquisition rejection.
 * @pre Hosted abort semantics are available.
 * @pre Fake provider construction is deterministic.
 * @post Every missing callback was rejected.
 * @post No resource operation executed.
 * @note Requirements remain active under `NDEBUG`.
 * @since 0.1.0
 */
static void test_invalid_providers(void)
{
  fake_io_t                  fake = {};
  firmware_pipeline_io_ops_t ops  = make_ops(&fake);
  ops.open                        = nullptr;
  require_invalid_ops(&ops, &fake);
  ops      = make_ops(&fake);
  ops.seek = nullptr;
  require_invalid_ops(&ops, &fake);
  ops      = make_ops(&fake);
  ops.tell = nullptr;
  require_invalid_ops(&ops, &fake);
  ops          = make_ops(&fake);
  ops.allocate = nullptr;
  require_invalid_ops(&ops, &fake);
  ops      = make_ops(&fake);
  ops.read = nullptr;
  require_invalid_ops(&ops, &fake);
  ops       = make_ops(&fake);
  ops.close = nullptr;
  require_invalid_ops(&ops, &fake);
  ops         = make_ops(&fake);
  ops.release = nullptr;
  require_invalid_ops(&ops, &fake);
}

/**
 * @brief Exercise every independent pointer and path preflight guard.
 * @details Proves rejection happens before the provider open callback.
 * @pre Hosted abort semantics are available.
 * @pre Empty output storage is writable.
 * @post Null operations, null/empty paths, and null output are rejected.
 * @post No resource operation executed.
 * @note Requirements remain active under `NDEBUG`.
 * @since 0.1.0
 */
static void test_invalid_inputs(void)
{
  fake_io_t                  fake  = {};
  firmware_pipeline_io_ops_t ops   = make_ops(&fake);
  firmware_pipeline_image_t  image = {};
  require(priv_firmware_pipeline_read_image(nullptr, "image.bin", &image) ==
          k_firmware_pipeline_io_failed);
  require(priv_firmware_pipeline_read_image(&ops, nullptr, &image) ==
          k_firmware_pipeline_io_failed);
  require(priv_firmware_pipeline_read_image(&ops, "", &image) == k_firmware_pipeline_io_failed);
  require(priv_firmware_pipeline_read_image(&ops, "image.bin", nullptr) ==
          k_firmware_pipeline_io_failed);
  require(fake.open_count == 0U);
  require(image.bytes == nullptr);
  require(image.size == 0U);
}

/**
 * @brief Prove the zero-size path closes without allocation or read.
 * @details Exercises false controls for both size-dependent decisions.
 * @pre Hosted abort semantics are available.
 * @pre Fake provider construction is deterministic.
 * @post Empty output is published after exactly one close.
 * @post Allocate, read, and release callbacks remain unused.
 * @note Requirements remain active under `NDEBUG`.
 * @since 0.1.0
 */
static void test_empty_success(void)
{
  fake_io_t                  fake  = {.stage = k_empty_success};
  firmware_pipeline_io_ops_t ops   = make_ops(&fake);
  firmware_pipeline_image_t  image = {};
  require(priv_firmware_pipeline_read_image(&ops, "empty.bin", &image) ==
          k_firmware_pipeline_io_ok);
  require(image.bytes == nullptr);
  require(image.size == 0U);
  require(fake.close_count == 1U);
  require(fake.allocate_count == 0U);
  require(fake.read_count == 0U);
  require(fake.release_count == 0U);
}

/**
 * @brief Prove empty-input close failure releases no nonexistent allocation.
 * @details Exercises the close-error path with both size-dependent operations skipped.
 * @pre Hosted abort semantics are available.
 * @pre Empty output storage is writable.
 * @post Output remains empty and close executes exactly once.
 * @post Allocate, read, and release callbacks remain unused.
 * @note Requirements remain active under `NDEBUG`.
 * @since 0.1.0
 */
static void test_empty_close_failure(void)
{
  fake_io_t                  fake  = {.stage = k_empty_close_failure};
  firmware_pipeline_io_ops_t ops   = make_ops(&fake);
  firmware_pipeline_image_t  image = {};
  require(priv_firmware_pipeline_read_image(&ops, "empty.bin", &image) ==
          k_firmware_pipeline_io_failed);
  require(image.bytes == nullptr);
  require(image.size == 0U);
  require(fake.close_count == 1U);
  require(fake.allocate_count == 0U);
  require(fake.read_count == 0U);
  require(fake.release_count == 0U);
}

/**
 * @brief Prove one injected failure preserves output and releases exact resources.
 * @details Expected counts derive from whether the failure occurs before or after allocation.
 * @param[in] stage Single transition to fail.
 * @param[in] expected_close Expected close callback count.
 * @param[in] expected_release Expected release callback count.
 * @pre Stage is a declared failure value.
 * @post All always-active requirements passed or the process aborted.
 * @note Uses no hosted file or heap resource.
 * @since 0.1.0
 */
static void
run_failure(failure_stage_t stage, unsigned int expected_close, unsigned int expected_release)
{
  fake_io_t                       fake  = {.stage = stage};
  firmware_pipeline_io_ops_t      ops   = make_ops(&fake);
  firmware_pipeline_image_t       image = {};
  const firmware_pipeline_image_t saved = image;
  require(priv_firmware_pipeline_read_image(&ops, "image.bin", &image) ==
          k_firmware_pipeline_io_failed);
  require(memcmp(&image, &saved, sizeof(image)) == 0);
  require(fake.close_count == expected_close);
  require(fake.release_count == expected_release);
}

/**
 * @brief Exercise every acquisition failure and the complete success lifecycle.
 * @return Zero after every requirement passes.
 * @retval 0 All transitions preserved ownership invariants.
 * @pre Hosted abort semantics are available.
 * @post No fake allocation remains owned.
 * @note Requirements remain active under `NDEBUG`.
 * @since 0.1.0
 */
int main(void)
{
  test_invalid_providers();
  test_invalid_inputs();
  test_empty_success();
  test_empty_close_failure();

  run_failure(k_failure_open, 0U, 0U);
  run_failure(k_failure_seek_end, 1U, 0U);
  run_failure(k_failure_tell, 1U, 0U);
  run_failure(k_failure_oversize, 1U, 0U);
  run_failure(k_failure_seek_start, 1U, 0U);
  run_failure(k_failure_allocate, 1U, 0U);
  run_failure(k_failure_read, 1U, 1U);
  run_failure(k_failure_close, 1U, 1U);

  fake_io_t                  fake  = {};
  firmware_pipeline_io_ops_t ops   = make_ops(&fake);
  firmware_pipeline_image_t  image = {};
  require(priv_firmware_pipeline_read_image(&ops, "image.bin", &image) ==
          k_firmware_pipeline_io_ok);
  require(image.bytes == fake.storage);
  require(image.size == sizeof(fake.storage));
  require(fake.close_count == 1U);
  require(fake.release_count == 0U);
  priv_firmware_pipeline_release_image(&ops, &image);
  require(image.bytes == nullptr);
  require(image.size == 0U);
  require(fake.release_count == 1U);
  return 0;
}
