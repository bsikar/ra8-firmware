/**
 * @file ra_crashlog.c
 * @brief Cross-reset crash-log implementation (`.noinit` record + loop guard).
 *
 * @details
 * Owns the one `.noinit` ::ra_crashlog_record_t instance, the self-contained
 * software CRC-32 that protects it, and the write/validate/claim state
 * machine described in ra_crashlog.h. The record is placed in the linker's
 * `.noinit` section (pinned at the top of SRAM, above the stack) on the
 * firmware build so `Reset_Handler`'s `.bss` zero-fill never touches it; on
 * the host unit-test build it is an ordinary zero-init static (there is no
 * cross-reset survival to model in a single test process).
 *
 * A hardware CRC peripheral is deliberately NOT used: the write path runs
 * from a fault context where no peripheral may be assumed powered or
 * initialised, so the checksum is a tiny bitwise CRC-32 that touches
 * nothing but the record's own bytes.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_crashlog.h"

#include <stddef.h>
#include <stdint.h>

#include "ra_crashlog_internal.h"
#include "ra_exception.h"

/**
 * @enum ra_crashlog_crc_const_t
 * @brief Constants for the self-contained bitwise CRC-32.
 *
 * @details
 * Standard reflected CRC-32 (IEEE 802.3 / zlib): the polynomial is applied
 * LSB-first and the seed doubles as the final XOR mask.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra_crashlog_crc_poly = 0xEDB88320UL, /**< Reflected CRC-32 polynomial.        */
  k_ra_crashlog_crc_seed = 0xFFFFFFFFUL, /**< Initial value and final XOR mask.    */
} ra_crashlog_crc_const_t;

/**
 * @enum ra_crashlog_crc_iter_t
 * @brief Per-byte iteration count for the bitwise CRC-32.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra_crashlog_crc_bits = 8U, /**< Bits consumed per input byte. */
} ra_crashlog_crc_iter_t;

/**
 * @enum ra_crashlog_loops_const_t
 * @brief Saturation ceiling for the reset-loop counter.
 *
 * @details
 * `boot_loops` saturates here instead of wrapping, so a very long crash
 * loop can never roll the counter back below
 * ::k_ra_crashlog_loop_threshold and silently clear the safe-mode request.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra_crashlog_loops_max = 255U, /**< boot_loops never grows past this. */
} ra_crashlog_loops_const_t;

/**
 * @var s_ra_crashlog_record
 * @brief The one cross-reset crash-log record instance.
 *
 * @details
 * On the firmware build it lives in `.noinit` (NOLOAD, pinned at the top of
 * SRAM by `libs/ra_board_ek_ra8d2/ld/linker_script.ld`) so a warm/watchdog
 * reset does not clear it. On the host test build (`RA_SIMULATOR_MODE`) the
 * section attribute is dropped -- Mach-O rejects a bare section name and a
 * single test process has no reset to survive -- leaving a plain zero-init
 * static.
 *
 * @note `volatile` so the compiler cannot elide the write sequence or
 *       reorder the `magic`-written-last ordering the integrity model
 *       depends on.
 * @warning Written only through this module's API; the linker address is
 *          exposed as `g_ra_ls_noinit_start` for debugger inspection.
 * @since 0.1.0
 */
#ifdef RA_SIMULATOR_MODE
static volatile ra_crashlog_record_t s_ra_crashlog_record;
#else
[[gnu::section(".noinit")]] static volatile ra_crashlog_record_t s_ra_crashlog_record;
#endif

static_assert(sizeof(ra_crashlog_record_t) <= (size_t)k_ra_crashlog_reserve_bytes,
              "ra_crashlog_record_t must fit the linker NOINIT region (LENGTH(NOINIT))");

/**
 * @brief Compute a reflected CRC-32 over a byte span.
 *
 * @details
 * Bitwise (table-free) reflected CRC-32 so the checksum adds no `.rodata`
 * table and is safe to run from a fault context. The mask trick
 * `0 - (crc & 1)` yields `0x00000000` or `0xFFFFFFFF` branchlessly.
 *
 * @param[in] data Start of the span. Reads are `volatile` so the record's
 *                 own storage can be checksummed in place. Must not be
 *                 `nullptr`.
 * @param[in] len  Number of bytes to fold in (0 yields the empty CRC 0).
 *
 * @return The CRC-32 of the span.
 * @retval 0 When @p data is `nullptr` or @p len is 0.
 * @retval other The reflected CRC-32 of the @p len bytes at @p data.
 *
 * @pre @p data points at @p len readable bytes, or is `nullptr`.
 * @pre @p len does not exceed the size of the span at @p data.
 * @post No memory outside the span is read; nothing is written.
 * @post The result depends only on the @p len bytes and their order.
 *
 * @note Not stateful; trivially thread-safe.
 * @since 0.1.0
 */
static uint32_t ra_crashlog_crc32(const volatile uint8_t* data, uint32_t len)
{
  if (data == nullptr) {
    return 0U;
  }
  uint32_t crc = (uint32_t)k_ra_crashlog_crc_seed;
  for (uint32_t i = 0U; i < len; i++) {
    crc ^= (uint32_t)data[i];
    for (uint8_t bit = 0U; bit < (uint8_t)k_ra_crashlog_crc_bits; bit++) {
      const uint32_t mask = 0U - (crc & 1U);
      crc                 = (crc >> 1U) ^ ((uint32_t)k_ra_crashlog_crc_poly & mask);
    }
  }
  return crc ^ (uint32_t)k_ra_crashlog_crc_seed;
}

/**
 * @brief CRC-32 over the integrity-protected payload of the record.
 *
 * @details
 * Covers every byte from `boot_loops` to the end of the record -- i.e. the
 * record minus its leading `magic` sentinel and the `crc` field itself --
 * so the sentinel and the checksum are two independent integrity signals.
 *
 * @return The CRC-32 of the record payload.
 * @retval 0..UINT32_MAX Whatever the current payload bytes hash to.
 *
 * @pre The record storage is readable.
 * @pre `boot_loops` is the first CRC-covered field of the record.
 * @post No record byte is modified.
 * @post The result matches `crc` exactly when the payload is intact.
 *
 * @note Not thread-safe w.r.t. a concurrent record write.
 * @since 0.1.0
 */
static uint32_t ra_crashlog_payload_crc(void)
{
  const volatile uint8_t* base = (const volatile uint8_t*)&s_ra_crashlog_record;
  const uint32_t          off  = (uint32_t)offsetof(ra_crashlog_record_t, boot_loops);
  return ra_crashlog_crc32(&base[off], (uint32_t)sizeof(s_ra_crashlog_record) - off);
}

/**
 * @brief Whether the stored record is a trustworthy post-mortem.
 *
 * @details
 * The record is valid only when both integrity signals agree: the `magic`
 * sentinel equals ::k_ra_crashlog_magic_valid AND the recomputed payload
 * CRC equals the stored `crc`. Either failing -- as with the random SRAM
 * after a cold power-on -- rejects the record.
 *
 * @return Whether the record validates.
 * @retval true  `magic` matches AND the payload CRC matches.
 * @retval false Either signal disagrees (empty, stale, or corrupted).
 *
 * @pre The record storage is readable.
 * @pre A prior writer used the `magic`-written-last ordering.
 * @post No record byte is modified.
 * @post `true` is returned only for a fully-written, intact record.
 *
 * @note Not thread-safe. This two-condition record-validity check (the
 *       magic sentinel plus the payload CRC) is the module's MC/DC-covered
 *       compound; the vectors live in tests/test_ra_crashlog.c.
 * @since 0.1.0
 */
static bool ra_crashlog_is_valid(void)
{
  return (s_ra_crashlog_record.magic == (uint32_t)k_ra_crashlog_magic_valid) &&
         (ra_crashlog_payload_crc() == s_ra_crashlog_record.crc);
}

/** @brief Implementation of `ra_crashlog_install()` -- register the persist hook. */
void ra_crashlog_install(void)
{
  ra_exception_set_persist_hook(&ra_crashlog_record_fault);
}

/** @brief Implementation of `ra_crashlog_record_fault()` -- write payload, magic last. */
void ra_crashlog_record_fault(const volatile ra_exception_last_t* decoded)
{
  if (decoded == nullptr) {
    return;
  }
  /* Carry the prior crash count forward only if the existing record still
   * validates; a cold-boot / corrupted record restarts the count at 1. */
  uint32_t next = 1U;
  if (ra_crashlog_is_valid()) {
    const uint32_t prior = s_ra_crashlog_record.boot_loops;
    next = (prior < (uint32_t)k_ra_crashlog_loops_max) ? (prior + 1U)
                                                       : (uint32_t)k_ra_crashlog_loops_max;
  }
  s_ra_crashlog_record.magic      = 0U; /* invalidate for the write window */
  s_ra_crashlog_record.boot_loops = next;
  s_ra_crashlog_record.fault      = *decoded;
  s_ra_crashlog_record.crc        = ra_crashlog_payload_crc();
  s_ra_crashlog_record.magic      = (uint32_t)k_ra_crashlog_magic_valid; /* validate LAST */
}

/** @brief Implementation of `ra_crashlog_peek()` -- validate then copy out. */
bool ra_crashlog_peek(ra_crashlog_record_t* out)
{
  if (out == nullptr) {
    return false;
  }
  if (!ra_crashlog_is_valid()) {
    return false;
  }
  *out = s_ra_crashlog_record;
  return true;
}

/** @brief Implementation of `ra_crashlog_claim()` -- clear magic + reset the guard. */
void ra_crashlog_claim(void)
{
  s_ra_crashlog_record.magic      = 0U;
  s_ra_crashlog_record.crc        = 0U;
  s_ra_crashlog_record.boot_loops = 0U;
}

/** @brief Implementation of `ra_crashlog_safe_mode_requested()` -- count vs threshold. */
bool ra_crashlog_safe_mode_requested(void)
{
  ra_crashlog_record_t rec;
  if (!ra_crashlog_peek(&rec)) {
    return false;
  }
  return rec.boot_loops > (uint32_t)k_ra_crashlog_loop_threshold;
}

#ifdef RA_SIMULATOR_MODE
/** @brief Implementation of `ra_crashlog_test_record()` -- raw record pointer. */
volatile ra_crashlog_record_t* ra_crashlog_test_record(void)
{
  return &s_ra_crashlog_record;
}

/** @brief Implementation of `ra_crashlog_test_wipe()` -- zero every record byte. */
void ra_crashlog_test_wipe(void)
{
  volatile uint8_t* p = (volatile uint8_t*)&s_ra_crashlog_record;
  for (uint32_t i = 0U; i < (uint32_t)sizeof(s_ra_crashlog_record); i++) {
    p[i] = 0U;
  }
}
#endif /* RA_SIMULATOR_MODE */
