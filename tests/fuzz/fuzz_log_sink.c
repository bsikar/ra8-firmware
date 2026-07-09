/**
 * @file fuzz_log_sink.c
 * @brief Route ra_log away from the ITM registers for every fuzz harness.
 *
 * @details
 * Under `RA_SIMULATOR_MODE`, and absent an installed byte sink, `ra_log`
 * emits through the Cortex-M85 ITM stimulus registers at 0xE0000000. The
 * host unit-test build backs that window with anonymous RAM via
 * `tests/mocks/ra_sim_mmap.c`, so those reads are harmless there. The
 * libFuzzer harnesses (issue #193) run under AddressSanitizer, where that
 * address is inside ASan's reserved shadow gap and cannot be mapped: the
 * pure-computation harnesses omit `ra_sim_mmap.c` entirely, and the two
 * MMIO harnesses that keep it skip exactly the shadow-gap region. An
 * `ra_log` call from linked-in production code (for example
 * `ra_canfd_init()` logging a clock-ready timeout) would therefore
 * dereference unmapped memory and crash.
 *
 * This translation unit installs a discard byte sink from a load-time
 * constructor -- it only stores two pointers and touches no MMIO, so it is
 * safe under ASan -- before `main()` and thus before any harness input is
 * processed. With a sink installed, every `ra_log` path short-circuits
 * ahead of the ITM access (see `internal_itm_ready()` /
 * `internal_itm_write_byte()` in libs/ra_core/src/ra_log.c). Log output is
 * intentionally dropped: the harnesses care about crashes and sanitizer
 * findings, not log text, and routing to stderr would flood a fuzz session.
 *
 * The file is compiled into every harness (see the foreach in
 * tests/fuzz/CMakeLists.txt) and is a no-op outside `RA_SIMULATOR_MODE`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#ifdef RA_SIMULATOR_MODE

#include <stdint.h>

#include "ra_log.h"

/**
 * @brief Byte sink that discards every logged byte.
 *
 * @details
 * Matches ::ra_log_byte_sink_fn_t. Installed by ::fuzz_log_sink_install so
 * `ra_log` never reaches the ITM stimulus registers during a fuzz run.
 *
 * @param[in] ctx  Opaque cookie (unused; always nullptr here).
 * @param[in] byte Logged byte to discard.
 *
 * @return void
 *
 * @pre None.
 * @post No observable side effect; the byte is dropped.
 *
 * @note Thread-safe: holds no state.
 * @since 0.1.0
 */
static void fuzz_log_discard(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)byte;
}

/**
 * @brief Install the discard byte sink before `main()`.
 *
 * @details
 * Runs via `__attribute__((constructor))` so the sink is active before the
 * libFuzzer driver calls `LLVMFuzzerTestOneInput`. Only sets pointers via
 * ::ra_log_set_byte_sink; performs no `mmap`, so it cannot trip ASan's
 * shadow-gap guard the way the peripheral backing store would.
 *
 * @return void
 *
 * @pre Runs exactly once per process, at load time.
 * @post ::ra_log_set_byte_sink has been called with ::fuzz_log_discard, so
 *       subsequent `ra_log` calls bypass the ITM registers.
 *
 * @note Not thread-safe, but constructors run single-threaded before main.
 * @since 0.1.0
 */
[[gnu::constructor]] static void fuzz_log_sink_install(void)
{
  ra_log_set_byte_sink(fuzz_log_discard, nullptr);
}

#else
/* Non-simulator build: this TU contributes nothing. */
typedef int fuzz_log_sink_placeholder_t;
#endif /* RA_SIMULATOR_MODE */
