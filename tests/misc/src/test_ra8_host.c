/**
 * @file test_ra8_host.c
 * @brief Unit tests for the POSIX host composition root (RA8FW-313).
 *
 * @details
 * Exercises the request-shaped configuration (each binding independently
 * present or absent), the validation guards, the already-open guard, the
 * unwind that must leave no root descriptor behind when a later step fails,
 * and the idempotent close.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <unistd.h>

#include "ra8_arena.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_host.h"
#include "ra8_io_stream.h"
#include "unity_minimal.h"

/**
 * @enum t_host_const_t
 * @brief Fixture sizes.
 */
typedef enum : uint32_t {
  k_t_scratch_bytes = 1024U, /**< Host scratch block size. */
} t_host_const_t;

[[gnu::aligned(16)]] static uint8_t s_scratch[(size_t)k_t_scratch_bytes];

/**
 * @brief Verify that an all-zero request binds nothing and still succeeds.
 * @details Opens with no filesystem, no streams and no scratch, then asserts
 * every published handle stayed null and the close path accepts it.
 * @pre The host object is zero-initialized.
 * @post No handle is published and the host closes cleanly.
 * @note A tool that only wants the signal disposition asks for exactly this.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- an empty request publishes nothing)
 */
RA8_INTERNAL static void internal_test_empty_request(void)
{
  TEST_BEGIN("host empty request binds nothing");
  ra8_host_t           host = {};
  const ra8_host_cfg_t cfg  = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_host_open(&host, &cfg));
  TEST_ASSERT_NULL(host.fs);
  TEST_ASSERT_NULL(host.output);
  TEST_ASSERT_NULL(host.diagnostic);
  TEST_ASSERT_EQ(0U, host.arena.size);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_host_close(&host));
  TEST_END("host empty request binds nothing");
}

/**
 * @brief Verify that the stream request publishes both standard streams.
 * @details Asks for streams only and checks that the pair is published
 * together and that the stream handles are usable through the facade.
 * @pre Standard output and standard error are open and writable.
 * @post Both stream pointers are non-null and no filesystem is bound.
 * @note The descriptors are borrowed; closing the host never closes them.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- the pair binds or neither does)
 */
RA8_INTERNAL static void internal_test_streams_only(void)
{
  TEST_BEGIN("host binds the standard stream pair");
  ra8_host_t           host = {};
  const ra8_host_cfg_t cfg  = {.bind_streams = true};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_host_open(&host, &cfg));
  TEST_ASSERT_NOT_NULL(host.output);
  TEST_ASSERT_NOT_NULL(host.diagnostic);
  TEST_ASSERT_NULL(host.fs);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_host_close(&host));
  TEST_ASSERT_NULL(host.output);
  TEST_ASSERT_NULL(host.diagnostic);
  /* The borrowed descriptors survive the close. */
  TEST_ASSERT(write(STDERR_FILENO, "", 0U) == 0);
  TEST_END("host binds the standard stream pair");
}

/**
 * @brief Verify that the scratch request yields a usable, empty arena.
 * @details Binds scratch only, then carves from the published arena to prove
 * it was initialized rather than merely copied.
 * @pre The fixture scratch block is writable and aligned.
 * @post The arena reports the full region available before the carve.
 * @note The arena lives inside the host object, so the host must out-live it.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- scratch binds when bytes are named)
 */
RA8_INTERNAL static void internal_test_arena_request(void)
{
  TEST_BEGIN("host binds the caller scratch block");
  ra8_host_t           host = {};
  const ra8_host_cfg_t cfg  = {.arena_base = s_scratch, .arena_bytes = k_t_scratch_bytes};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_host_open(&host, &cfg));
  uint32_t remaining = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_remaining(&host.arena, &remaining));
  TEST_ASSERT_EQ(k_t_scratch_bytes, remaining);
  void* block = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_carve(&host.arena, 64U, 8U, &block));
  TEST_ASSERT_NOT_NULL(block);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_host_close(&host));
  TEST_ASSERT_EQ(0U, host.arena.size);
  TEST_END("host binds the caller scratch block");
}

/**
 * @brief Verify that the filesystem request publishes a bound facade.
 * @details Binds the process root read-only through the POSIX adapter and
 * asserts the facade pointer is published and withdrawn again on close.
 * @pre The process can open the root directory.
 * @post The facade is published while open and null once closed.
 * @note The adapter confines every portable path to the selected root.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- the facade binds or the open fails)
 */
RA8_INTERNAL static void internal_test_filesystem_request(void)
{
  TEST_BEGIN("host binds the root-confined filesystem");
  ra8_host_t           host = {};
  const ra8_host_cfg_t cfg  = {.root_path = "/", .bind_filesystem = true};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_host_open(&host, &cfg));
  TEST_ASSERT_NOT_NULL(host.fs);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_host_close(&host));
  TEST_ASSERT_NULL(host.fs);
  TEST_END("host binds the root-confined filesystem");
}

/**
 * @brief Verify the whole ritual composes in one call.
 * @details Asks for all three bindings at once and checks each published
 * handle, which is the case every host binary hand-wrote before.
 * @pre The process can open the root directory and write both std streams.
 * @post All three handles are published, then all three are withdrawn.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- every requested binding is present)
 */
RA8_INTERNAL static void internal_test_full_composition(void)
{
  TEST_BEGIN("host composes filesystem, streams and scratch");
  ra8_host_t           host = {};
  const ra8_host_cfg_t cfg  = {
        .root_path       = "/",
        .arena_base      = s_scratch,
        .arena_bytes     = k_t_scratch_bytes,
        .bind_filesystem = true,
        .bind_streams    = true,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_host_open(&host, &cfg));
  TEST_ASSERT_NOT_NULL(host.fs);
  TEST_ASSERT_NOT_NULL(host.output);
  TEST_ASSERT_NOT_NULL(host.diagnostic);
  TEST_ASSERT_EQ(k_t_scratch_bytes, host.arena.size);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_host_close(&host));
  TEST_END("host composes filesystem, streams and scratch");
}

/**
 * @brief Verify every validation guard on the open path.
 * @details Covers the two null pointers, a filesystem request with no root
 * path, and scratch bytes named with no base pointer.
 * @pre No host object under test has been opened.
 * @post Each refusal leaves the host with nothing published.
 * @note A refused open must be indistinguishable from one never attempted.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (`host == nullptr` and `cfg == nullptr` each independently force null_ptr;
 * the missing root path and the missing scratch base each force invalid_arg)
 */
RA8_INTERNAL static void internal_test_guards(void)
{
  TEST_BEGIN("host refuses malformed requests");
  ra8_host_t           host = {};
  const ra8_host_cfg_t cfg  = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_host_open(nullptr, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_host_open(&host, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_host_close(nullptr));

  const ra8_host_cfg_t no_root = {.bind_filesystem = true};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_host_open(&host, &no_root));
  TEST_ASSERT_NULL(host.fs);

  const ra8_host_cfg_t no_base = {.arena_bytes = k_t_scratch_bytes};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_host_open(&host, &no_base));
  TEST_ASSERT_EQ(0U, host.arena.size);
  TEST_END("host refuses malformed requests");
}

/**
 * @brief Verify that a later failure unwinds the bindings already made.
 * @details Requests a filesystem that binds and scratch that cannot, then
 * asserts the filesystem was released rather than left open behind a failure.
 * @pre The process can open the root directory.
 * @post The failed open publishes no facade and holds no root descriptor.
 * @note This is the leak the hand-written unwinds kept getting wrong.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (the arena step's refusal independently drives the unwind after the
 * filesystem step succeeded)
 */
RA8_INTERNAL static void internal_test_unwind_releases_filesystem(void)
{
  TEST_BEGIN("host unwinds an earlier binding on a later failure");
  ra8_host_t           host = {};
  const ra8_host_cfg_t cfg  = {
        .root_path       = "/",
        .arena_bytes     = k_t_scratch_bytes, /* no base: refused */
        .bind_filesystem = true,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_host_open(&host, &cfg));
  TEST_ASSERT_NULL(host.fs);
  /* A reopen proves the first attempt did not keep the root descriptor. */
  const ra8_host_cfg_t good = {.root_path = "/", .bind_filesystem = true};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_host_open(&host, &good));
  TEST_ASSERT_NOT_NULL(host.fs);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_host_close(&host));
  TEST_END("host unwinds an earlier binding on a later failure");
}

/**
 * @brief Verify the lifecycle guards on a second open and a second close.
 * @details A duplicate open is refused without disturbing the live binding,
 * and a close on an unopened host reports success so error paths may call it
 * unconditionally.
 * @pre The host opens successfully once.
 * @post The duplicate open changes nothing and the extra close is a no-op.
 * @note Reopening after a close is allowed and is exercised here.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (the `opened` guard independently forces `exists` on the second open and
 * `ok` on the second close)
 */
RA8_INTERNAL static void internal_test_lifecycle_guards(void)
{
  TEST_BEGIN("host guards duplicate open and repeated close");
  ra8_host_t           host = {};
  const ra8_host_cfg_t cfg  = {.bind_streams = true};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_host_open(&host, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_exists, ra8_host_open(&host, &cfg));
  TEST_ASSERT_NOT_NULL(host.output);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_host_close(&host));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_host_close(&host));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_host_open(&host, &cfg));
  TEST_ASSERT_NOT_NULL(host.output);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_host_close(&host));
  TEST_END("host guards duplicate open and repeated close");
}

/**
 * @brief Run every host composition-root case.
 * @return Zero when every case passed.
 * @since 0.1.0
 */
int main(void)
{
  internal_test_empty_request();
  internal_test_streams_only();
  internal_test_arena_request();
  internal_test_filesystem_request();
  internal_test_full_composition();
  internal_test_guards();
  internal_test_unwind_releases_filesystem();
  internal_test_lifecycle_guards();
  return 0;
}
