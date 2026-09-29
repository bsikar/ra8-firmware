/**
 * @file examples/ek_ra8d2/hw_pending/io_blockdev_cache_demo/src/main.c
 * @brief ra8_io caching block device contract demo (#983).
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Drives the `ra8_io` caching block device -- the write-through LRU sector-cache
 * decorator declared in `ra8_io_blockdev_cache.h` -- directly over a
 * deterministic RAM backend, and asserts the decorator's documented contract
 * rather than merely observing that some cache hit occurred.
 *
 * The parked `hil_needs_revalidation/ra8_io_cache_demo` reaches the cache only
 * indirectly, through FAT12 + VFS traffic, and requires just `hits != 0`; the
 * cache therefore had no maintained consumer that pins its behaviour. This demo
 * is deliberately not a copy of it: there is no filesystem in the path, every
 * access is a single known LBA, so the hit/miss counters are exact and the
 * eviction order is provable.
 *
 * Legs, each halting the run on its own error code:
 *   1. `bind`    -- ::ra8_io_blockdev_cache_init binds over the RAM backend,
 *                   the counters start at zero, and ::ra8_io_blockdev_get_caps
 *                   forwards the wrapped geometry field for field.
 *   2. `hitmiss` -- a cold read is one miss, the immediate re-read is one hit,
 *                   and both deliver the backing pattern byte for byte.
 *   3. `through` -- a write is committed to the *backing array* before the call
 *                   returns (write-through, inspected behind the device) and
 *                   leaves the block cached, so the following read is a hit.
 *   4. `lru`     -- with four slots, the access pattern 10,11,12,13,10,14 must
 *                   evict 11 and keep 10. The leg requires exactly 4 hits and
 *                   6 misses; a slot-order (FIFO) victim choice would yield
 *                   3 and 7, so the policy itself is pinned, not just the data.
 *   5. `erase`   -- an erase through the cache drops only the slots covering the
 *                   erased range: the erased blocks re-read as misses returning
 *                   the erase value, a neighbouring cached block stays a hit.
 *   6. `span`    -- a three-block request is three misses, the repeat is three
 *                   hits, and the whole 1536-byte span matches the pattern.
 *   7. `guards`  -- every documented rejection: five NULL arguments to
 *                   ::ra8_io_blockdev_cache_init, a zero slot count, and a NULL
 *                   state to ::ra8_io_blockdev_cache_stats.
 *
 * Bench state: not yet run on hardware, so this app lives under `hw_pending`
 * and ships no `hil.conf`. Nothing outside the SoC is needed: the medium is a
 * RAM disk in `.bss` and the only peripheral is the SCI8 console, which the
 * ra8_emulator captures, so a pass prints headlessly as
 * `io_blockdev_cache_demo: legs=7 hits=H misses=M PASS`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_board_ek_ra8d2_console_stream.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_io.h"
#include "ra8_io_blockdev_cache.h"
#include "ra8_io_log.h"
#include "ra8_log.h"
#include "ra8_time.h"

/** @enum demo_const_t @brief Console, medium, and cache geometry (no magic numbers). */
typedef enum : uint32_t {
  k_demo_uart_baud    = 115200U, /**< Console baud.                              */
  k_demo_disk_blocks  = 64U,     /**< 32 KiB RAM disk (the "slow" medium).        */
  k_demo_cache_slots  = 4U,      /**< Cached sectors; small enough to evict.      */
  k_demo_span_blocks  = 3U,      /**< Blocks in the multi-block request leg.      */
  k_demo_legs         = 7U,      /**< Legs reported on the PASS line.             */
  k_demo_seed_mul     = 31U,     /**< Backing-pattern LBA multiplier.             */
  k_demo_seed_add     = 7U,      /**< Backing-pattern byte bias.                  */
  k_demo_payload_mul  = 13U,     /**< Write-through payload multiplier.           */
  k_demo_payload_add  = 3U,      /**< Write-through payload bias.                 */
  k_demo_lru_expect_h = 4U,      /**< Hits the LRU access pattern must produce.   */
  k_demo_lru_expect_m = 6U,      /**< Misses the LRU access pattern must produce. */
} demo_const_t;

/** @enum demo_lba_t @brief Fixed logical block addresses, one group per leg. */
typedef enum : uint32_t {
  k_demo_lba_probe  = 0U,  /**< Cold-read / re-read leg.                    */
  k_demo_lba_write  = 5U,  /**< Write-through leg.                          */
  k_demo_lba_lru_a  = 10U, /**< LRU leg: touched again, must survive.       */
  k_demo_lba_lru_b  = 11U, /**< LRU leg: least recently used, must go.      */
  k_demo_lba_lru_c  = 12U, /**< LRU leg: must survive.                      */
  k_demo_lba_lru_d  = 13U, /**< LRU leg: must survive.                      */
  k_demo_lba_lru_e  = 14U, /**< LRU leg: the read that forces an eviction.  */
  k_demo_lba_erase  = 20U, /**< Erase leg: first block of the erased range. */
  k_demo_lba_keep   = 22U, /**< Erase leg: cached neighbour outside range.  */
  k_demo_lba_span   = 30U, /**< Multi-block leg: first block of the span.   */
} demo_lba_t;

/** @brief RAM-disk backing store for the wrapped backend (in SRAM .bss). */
static uint8_t s_disk[(size_t)k_demo_disk_blocks * (size_t)k_ra8_io_block_size_bytes];
/** @brief Wrapped backend handle + its RAM backend state. */
static ra8_io_blockdev_t           s_under;
static ra8_io_blockdev_ram_state_t s_ustate;
/** @brief Caching decorator handle + its caller-owned cache storage. */
static ra8_io_blockdev_t             s_cached;
static ra8_io_blockdev_cache_state_t s_cstate;
static uint8_t s_cache_data[(size_t)k_demo_cache_slots * (size_t)k_ra8_io_block_size_bytes];
static ra8_io_blockdev_cache_slot_t s_cache_slots[(size_t)k_demo_cache_slots];
/** @brief Scratch block buffers and the multi-block span buffer. */
static uint8_t s_blk[(size_t)k_ra8_io_block_size_bytes];
static uint8_t s_pay[(size_t)k_ra8_io_block_size_bytes];
static uint8_t s_span[(size_t)k_demo_span_blocks * (size_t)k_ra8_io_block_size_bytes];
/** @brief Console output stream; the board owns the sink behind it. */
static ra8_io_stream_t s_uart;

/** @brief Module log tag. */
static const char* const s_tag = "io_blockdev_cache_demo";

/**
 * @brief Print a NUL-terminated string on the UART stream.
 *
 * @details Delegates bounded string emission to the initialized stream and
 * intentionally ignores diagnostic-output errors in this terminal demo.
 *
 * @param[in] msg NUL-terminated message to emit.
 * @pre @p msg is non-NULL and readable through its terminator.
 * @pre ::s_uart has been bound to the board console sink.
 * @post The stream has accepted the message or reported an ignored sink error.
 * @post Cache and block-device state remain unchanged.
 * @note This single-threaded diagnostic helper performs no retry.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_demo_print(const char* msg)
{
  (void)ra8_io_stream_puts(&s_uart, msg);
}

/**
 * @brief Bring up CGC, SysTick, and the board console; halt on failure.
 *
 * @details Resolves CPUCLK0, initializes the time base, then hands the console
 * over to the BSP -- which owns the channel, the PD02 / PD03 routing and the
 * live-PCLKA bit-rate solve -- and binds it as an ra8_io stream.
 *
 * @pre Reset startup has initialized data and BSS storage.
 * @pre Peripheral register mappings for clocks, pins, and the console are
 *      accessible.
 * @post On return, the board console is configured for the requested
 *       diagnostic baud and bound into ::s_uart.
 * @post Any required setup failure parks the application before returning.
 * @note This helper is intended for the single-threaded startup path only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) ||
      (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) ||
      (ra8_time_init(cpuclk0_hz) != k_ra8_ok) ||
      (ra8_board_uart_console_init((uint32_t)k_demo_uart_baud) != k_ra8_ok) ||
      (ra8_board_console_stream(&s_uart) != k_ra8_ok)) {
    while (true) {
    }
  }
}

/**
 * @brief Backing-store pattern byte for one offset of one block.
 *
 * @details Position dependent in both the block address and the byte offset, so
 * a read served from the wrong slot or the wrong offset cannot compare equal.
 *
 * @param[in] lba Logical block address the byte belongs to.
 * @param[in] off Byte offset within that block.
 * @return uint8_t The expected byte value.
 * @pre @p off is below ::k_ra8_io_block_size_bytes.
 * @post No state is mutated.
 * @note Pure function; safe to call from any context.
 * @since 0.1.0
 */
RA8_INTERNAL static uint8_t internal_demo_pattern(uint32_t lba, uint32_t off)
{
  return (uint8_t)((lba * (uint32_t)k_demo_seed_mul) + off + (uint32_t)k_demo_seed_add);
}

/**
 * @brief Seed the whole RAM disk with the position-dependent pattern.
 *
 * @details Writes the backing array directly, behind the block device, so the
 * medium starts in a known state without any cached access.
 *
 * @pre ::s_disk covers ::k_demo_disk_blocks blocks.
 * @post Every byte of ::s_disk equals ::internal_demo_pattern for its position.
 * @post No cache or block-device state is touched.
 * @note Called once, before the backend is bound.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_demo_seed_disk(void)
{
  for (uint32_t lba = 0U; lba < (uint32_t)k_demo_disk_blocks; ++lba) {
    for (uint32_t off = 0U; off < (uint32_t)k_ra8_io_block_size_bytes; ++off) {
      s_disk[((size_t)lba * (size_t)k_ra8_io_block_size_bytes) + (size_t)off] =
        internal_demo_pattern(lba, off);
    }
  }
}

/**
 * @brief Compare a buffer against the backing pattern of a block range.
 *
 * @param[in] buf   Buffer holding @p count consecutive blocks.
 * @param[in] lba   Logical block address the buffer starts at.
 * @param[in] count Number of blocks in @p buf.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                     Every byte matched.
 * @retval k_ra8_err_checksum_mismatch  At least one byte differed.
 * @pre @p buf is readable for `count * 512` bytes.
 * @post No state is mutated.
 * @note Pure comparison; reports the first mismatch as a single code.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_demo_expect_pattern(const uint8_t* buf, uint32_t lba, uint32_t count)
{
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  for (uint32_t i = 0U; i < count; ++i) {
    for (uint32_t off = 0U; off < (uint32_t)k_ra8_io_block_size_bytes; ++off) {
      const size_t at = ((size_t)i * (size_t)k_ra8_io_block_size_bytes) + (size_t)off;
      if (buf[at] != internal_demo_pattern(lba + i, off)) {
        return k_ra8_err_checksum_mismatch;
      }
    }
  }
  return k_ra8_ok;
}

/**
 * @brief (Re)bind the caching decorator over the backend with zeroed counters.
 *
 * @details Each leg that asserts exact hit/miss totals starts from a fresh
 * cache, so the totals belong to that leg alone.
 *
 * @return ra8_err_t Error code propagated from ::ra8_io_blockdev_cache_init.
 * @retval k_ra8_ok The decorator is bound and its counters are zero.
 * @pre ::s_under is a bound backend and the cache storage out-lives the cache.
 * @post ::s_cached routes through a cache holding no valid slot.
 * @post ::s_cstate reports zero hits and zero misses.
 * @note Rebinding drops cached contents; the medium is untouched.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_demo_rebind_cache(void)
{
  (void)memset(&s_cached, 0, sizeof(s_cached));
  (void)memset(&s_cstate, 0, sizeof(s_cstate));
  (void)memset(s_cache_slots, 0, sizeof(s_cache_slots));
  return ra8_io_blockdev_cache_init(&s_cached,
                                    &s_cstate,
                                    &s_under,
                                    s_cache_data,
                                    s_cache_slots,
                                    (uint32_t)k_demo_cache_slots);
}

/**
 * @brief Require the cache counters to equal an expected pair exactly.
 *
 * @param[in] hits   Expected read-hit total.
 * @param[in] misses Expected read-miss total.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    Both counters matched.
 * @retval k_ra8_err_checksum_mismatch A counter differed from its expectation.
 * @retval k_ra8_err_*                 Propagated from the stats call.
 * @pre The cache is bound.
 * @post No state is mutated.
 * @note Exact equality is the point: a range check would not pin the policy.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_demo_expect_stats(uint32_t hits, uint32_t misses)
{
  uint32_t h = 0U;
  uint32_t m = 0U;
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_cache_stats(&s_cstate, &h, &m), s_tag, "stats");
  if (h != hits) {
    return k_ra8_err_checksum_mismatch;
  }
  if (m != misses) {
    return k_ra8_err_checksum_mismatch;
  }
  return k_ra8_ok;
}

/**
 * @brief Leg 1: bind the cache and check the forwarded capabilities.
 *
 * @details The decorator must not alter the medium's geometry, so every
 * capability field is compared against the wrapped backend's own report, and
 * the fresh counters must read zero.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    Bound, counters zero, caps identical.
 * @retval k_ra8_err_checksum_mismatch A capability field or counter differed.
 * @retval k_ra8_err_*                 Propagated from init, caps, or stats.
 * @pre ::s_under is bound over the seeded RAM disk.
 * @post ::s_cached is bound and usable by the following legs.
 * @note Compares field by field; a struct memcmp would hide padding noise.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_demo_leg_bind(void)
{
  RA8_RETURN_ON_ERROR(internal_demo_rebind_cache(), s_tag, "cache init");
  RA8_RETURN_ON_ERROR(internal_demo_expect_stats(0U, 0U), s_tag, "fresh counters");

  ra8_io_blockdev_caps_t want = {};
  ra8_io_blockdev_caps_t got  = {};
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_get_caps(&s_under, &want), s_tag, "backend caps");
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_get_caps(&s_cached, &got), s_tag, "cached caps");
  if (got.block_count != want.block_count) {
    return k_ra8_err_checksum_mismatch;
  }
  if (got.erase_unit_blocks != want.erase_unit_blocks) {
    return k_ra8_err_checksum_mismatch;
  }
  if (got.program_size_bytes != want.program_size_bytes) {
    return k_ra8_err_checksum_mismatch;
  }
  if (got.logical_block_bytes != want.logical_block_bytes) {
    return k_ra8_err_checksum_mismatch;
  }
  if (got.erase_value != want.erase_value) {
    return k_ra8_err_checksum_mismatch;
  }
  if (got.must_erase_before_write != want.must_erase_before_write) {
    return k_ra8_err_checksum_mismatch;
  }
  if (got.read_only != want.read_only) {
    return k_ra8_err_checksum_mismatch;
  }
  return k_ra8_ok;
}

/**
 * @brief Leg 2: a cold read is a miss, the re-read is a hit, both correct.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    Counters and both payloads were exact.
 * @retval k_ra8_err_checksum_mismatch A counter or a byte differed.
 * @retval k_ra8_err_*                 Propagated from the reads.
 * @pre The cache is freshly bound over the seeded RAM disk.
 * @post ::k_demo_lba_probe is cached and the counters read 1 hit / 1 miss.
 * @note Serving a hit must still deliver the medium's bytes, so both passes
 *       are compared, not just the first.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_demo_leg_hitmiss(void)
{
  RA8_RETURN_ON_ERROR(internal_demo_rebind_cache(), s_tag, "cache init");
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_read(&s_cached, (uint32_t)k_demo_lba_probe, 1U, s_blk),
                      s_tag,
                      "cold read");
  RA8_RETURN_ON_ERROR(internal_demo_expect_pattern(s_blk, (uint32_t)k_demo_lba_probe, 1U),
                      s_tag,
                      "cold payload");
  RA8_RETURN_ON_ERROR(internal_demo_expect_stats(0U, 1U), s_tag, "cold counters");

  (void)memset(s_blk, 0, sizeof(s_blk));
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_read(&s_cached, (uint32_t)k_demo_lba_probe, 1U, s_blk),
                      s_tag,
                      "warm read");
  RA8_RETURN_ON_ERROR(internal_demo_expect_pattern(s_blk, (uint32_t)k_demo_lba_probe, 1U),
                      s_tag,
                      "warm payload");
  return internal_demo_expect_stats(1U, 1U);
}

/**
 * @brief Leg 3: a write reaches the medium immediately and leaves it cached.
 *
 * @details Write-through is asserted behind the device, against ::s_disk, so a
 * cache that merely buffered the block would fail here even though a read back
 * through the same cache would have looked correct. The following read must be
 * a hit, which pins the insert-on-write behaviour.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    Medium updated, read-back hit, bytes equal.
 * @retval k_ra8_err_checksum_mismatch The medium or a counter disagreed.
 * @retval k_ra8_err_*                 Propagated from the write or read.
 * @pre The cache is bound and the medium is writable.
 * @post ::k_demo_lba_write holds the payload in both the cache and ::s_disk.
 * @note The payload is deliberately unlike the seeded pattern.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_demo_leg_writethrough(void)
{
  RA8_RETURN_ON_ERROR(internal_demo_rebind_cache(), s_tag, "cache init");
  for (uint32_t off = 0U; off < (uint32_t)k_ra8_io_block_size_bytes; ++off) {
    s_pay[off] = (uint8_t)((off * (uint32_t)k_demo_payload_mul) + (uint32_t)k_demo_payload_add);
  }
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_write(&s_cached, (uint32_t)k_demo_lba_write, 1U, s_pay),
                      s_tag,
                      "write");

  const size_t at = (size_t)k_demo_lba_write * (size_t)k_ra8_io_block_size_bytes;
  if (memcmp(&s_disk[at], s_pay, (size_t)k_ra8_io_block_size_bytes) != 0) {
    return k_ra8_err_checksum_mismatch;
  }

  RA8_RETURN_ON_ERROR(ra8_io_blockdev_read(&s_cached, (uint32_t)k_demo_lba_write, 1U, s_blk),
                      s_tag,
                      "read back");
  if (memcmp(s_blk, s_pay, (size_t)k_ra8_io_block_size_bytes) != 0) {
    return k_ra8_err_checksum_mismatch;
  }
  return internal_demo_expect_stats(1U, 0U);
}

/**
 * @brief Leg 4: the victim is the least recently used slot, not the first one.
 *
 * @details Fills all four slots with 10,11,12,13, touches 10 so 11 becomes the
 * least recently used, then reads 14 to force one eviction. Reading 12, 13, 10
 * must all hit and 11 must miss, which is 4 hits and 6 misses in total. A
 * slot-order victim choice would have evicted 10 instead and produced 3 hits
 * and 7 misses, so the exact pair distinguishes the two policies.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    The eviction followed the LRU stamps.
 * @retval k_ra8_err_checksum_mismatch A counter or payload differed.
 * @retval k_ra8_err_*                 Propagated from the reads.
 * @pre The cache holds exactly ::k_demo_cache_slots slots.
 * @post The counters read ::k_demo_lru_expect_h / ::k_demo_lru_expect_m.
 * @note Every read is payload checked, so an evicted slot cannot pass by
 *       returning stale bytes.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_demo_leg_lru(void)
{
  static const uint32_t k_fill[] = {(uint32_t)k_demo_lba_lru_a,
                                    (uint32_t)k_demo_lba_lru_b,
                                    (uint32_t)k_demo_lba_lru_c,
                                    (uint32_t)k_demo_lba_lru_d};
  static const uint32_t k_after[] = {(uint32_t)k_demo_lba_lru_a,
                                     (uint32_t)k_demo_lba_lru_e,
                                     (uint32_t)k_demo_lba_lru_c,
                                     (uint32_t)k_demo_lba_lru_d,
                                     (uint32_t)k_demo_lba_lru_a,
                                     (uint32_t)k_demo_lba_lru_b};

  RA8_RETURN_ON_ERROR(internal_demo_rebind_cache(), s_tag, "cache init");
  for (uint32_t i = 0U; i < (uint32_t)(sizeof(k_fill) / sizeof(k_fill[0])); ++i) {
    RA8_RETURN_ON_ERROR(ra8_io_blockdev_read(&s_cached, k_fill[i], 1U, s_blk), s_tag, "fill");
    RA8_RETURN_ON_ERROR(internal_demo_expect_pattern(s_blk, k_fill[i], 1U), s_tag, "fill bytes");
  }
  RA8_RETURN_ON_ERROR(internal_demo_expect_stats(0U, (uint32_t)k_demo_cache_slots),
                      s_tag,
                      "fill counters");

  for (uint32_t i = 0U; i < (uint32_t)(sizeof(k_after) / sizeof(k_after[0])); ++i) {
    RA8_RETURN_ON_ERROR(ra8_io_blockdev_read(&s_cached, k_after[i], 1U, s_blk), s_tag, "probe");
    RA8_RETURN_ON_ERROR(internal_demo_expect_pattern(s_blk, k_after[i], 1U), s_tag, "probe bytes");
  }
  return internal_demo_expect_stats((uint32_t)k_demo_lru_expect_h, (uint32_t)k_demo_lru_expect_m);
}

/**
 * @brief Leg 5: an erase drops exactly the slots covering the erased range.
 *
 * @details Caches the two blocks that will be erased plus one neighbour just
 * outside the range. After the erase the neighbour must still hit, while both
 * erased blocks must miss and read back as the medium's erase value.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    Only the covered slots were invalidated.
 * @retval k_ra8_err_checksum_mismatch A counter or an erased byte disagreed.
 * @retval k_ra8_err_*                 Propagated from the write, erase, or read.
 * @pre The medium reports ::k_ra8_io_erase_value_zero.
 * @post The erased blocks hold the erase value in the cache and on the medium.
 * @note The neighbour proves the invalidation is range scoped, not a flush.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_demo_leg_erase(void)
{
  const uint32_t erase_count = 2U;
  RA8_RETURN_ON_ERROR(internal_demo_rebind_cache(), s_tag, "cache init");
  RA8_RETURN_ON_ERROR(
    ra8_io_blockdev_read(&s_cached, (uint32_t)k_demo_lba_erase, erase_count, s_span),
    s_tag,
    "warm range");
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_read(&s_cached, (uint32_t)k_demo_lba_keep, 1U, s_blk),
                      s_tag,
                      "warm neighbour");
  RA8_RETURN_ON_ERROR(internal_demo_expect_stats(0U, 3U), s_tag, "warm counters");

  RA8_RETURN_ON_ERROR(ra8_io_blockdev_erase(&s_cached, (uint32_t)k_demo_lba_erase, erase_count),
                      s_tag,
                      "erase");

  RA8_RETURN_ON_ERROR(ra8_io_blockdev_read(&s_cached, (uint32_t)k_demo_lba_keep, 1U, s_blk),
                      s_tag,
                      "neighbour read");
  RA8_RETURN_ON_ERROR(internal_demo_expect_pattern(s_blk, (uint32_t)k_demo_lba_keep, 1U),
                      s_tag,
                      "neighbour bytes");
  RA8_RETURN_ON_ERROR(internal_demo_expect_stats(1U, 3U), s_tag, "neighbour counters");

  for (uint32_t i = 0U; i < erase_count; ++i) {
    RA8_RETURN_ON_ERROR(
      ra8_io_blockdev_read(&s_cached, (uint32_t)k_demo_lba_erase + i, 1U, s_blk),
      s_tag,
      "erased read");
    for (uint32_t off = 0U; off < (uint32_t)k_ra8_io_block_size_bytes; ++off) {
      if (s_blk[off] != (uint8_t)k_ra8_io_erase_value_zero) {
        return k_ra8_err_checksum_mismatch;
      }
    }
  }
  return internal_demo_expect_stats(1U, 3U + erase_count);
}

/**
 * @brief Leg 6: a multi-block request caches every block it touches.
 *
 * @details One three-block request must produce three misses, and the identical
 * repeat three hits, with the whole span byte compared both times. This is the
 * range path through the vtable rather than the single-block path the other
 * legs take.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    Counters exact and the span matched twice.
 * @retval k_ra8_err_checksum_mismatch A counter or a byte differed.
 * @retval k_ra8_err_*                 Propagated from the reads.
 * @pre ::k_demo_span_blocks does not exceed ::k_demo_cache_slots.
 * @post The span is cached and the counters read 3 hits / 3 misses.
 * @note The span buffer is static; it is larger than the app's stack budget.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_demo_leg_span(void)
{
  const uint32_t n = (uint32_t)k_demo_span_blocks;
  RA8_RETURN_ON_ERROR(internal_demo_rebind_cache(), s_tag, "cache init");
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_read(&s_cached, (uint32_t)k_demo_lba_span, n, s_span),
                      s_tag,
                      "cold span");
  RA8_RETURN_ON_ERROR(internal_demo_expect_pattern(s_span, (uint32_t)k_demo_lba_span, n),
                      s_tag,
                      "cold span bytes");
  RA8_RETURN_ON_ERROR(internal_demo_expect_stats(0U, n), s_tag, "cold span counters");

  (void)memset(s_span, 0, sizeof(s_span));
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_read(&s_cached, (uint32_t)k_demo_lba_span, n, s_span),
                      s_tag,
                      "warm span");
  RA8_RETURN_ON_ERROR(internal_demo_expect_pattern(s_span, (uint32_t)k_demo_lba_span, n),
                      s_tag,
                      "warm span bytes");
  return internal_demo_expect_stats(n, n);
}

/**
 * @brief Leg 7: every documented argument rejection returns its own code.
 *
 * @details Walks the five NULL arguments of ::ra8_io_blockdev_cache_init, the
 * zero slot count, and the NULL state of ::ra8_io_blockdev_cache_stats. The
 * stats call with both outputs NULL must still succeed, as documented.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                 Every guard returned its documented code.
 * @retval k_ra8_err_invalid_state  A guard accepted an invalid argument.
 * @pre The cache storage arrays are valid, so only the tested argument is bad.
 * @post ::s_cached is rebound, so later callers see a usable cache.
 * @note A rejected init must leave its output handle unbound.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_demo_leg_guards(void)
{
  ra8_io_blockdev_t             bd    = {};
  ra8_io_blockdev_cache_state_t st    = {};
  const uint32_t                slots = (uint32_t)k_demo_cache_slots;

  if (ra8_io_blockdev_cache_init(nullptr, &st, &s_under, s_cache_data, s_cache_slots, slots) !=
      k_ra8_err_null_ptr) {
    return k_ra8_err_invalid_state;
  }
  if (ra8_io_blockdev_cache_init(&bd, nullptr, &s_under, s_cache_data, s_cache_slots, slots) !=
      k_ra8_err_null_ptr) {
    return k_ra8_err_invalid_state;
  }
  if (ra8_io_blockdev_cache_init(&bd, &st, nullptr, s_cache_data, s_cache_slots, slots) !=
      k_ra8_err_null_ptr) {
    return k_ra8_err_invalid_state;
  }
  if (ra8_io_blockdev_cache_init(&bd, &st, &s_under, nullptr, s_cache_slots, slots) !=
      k_ra8_err_null_ptr) {
    return k_ra8_err_invalid_state;
  }
  if (ra8_io_blockdev_cache_init(&bd, &st, &s_under, s_cache_data, nullptr, slots) !=
      k_ra8_err_null_ptr) {
    return k_ra8_err_invalid_state;
  }
  if (ra8_io_blockdev_cache_init(&bd, &st, &s_under, s_cache_data, s_cache_slots, 0U) !=
      k_ra8_err_invalid_size) {
    return k_ra8_err_invalid_state;
  }
  if (bd.iface != nullptr) {
    return k_ra8_err_invalid_state;
  }

  uint32_t h = 0U;
  if (ra8_io_blockdev_cache_stats(nullptr, &h, nullptr) != k_ra8_err_null_ptr) {
    return k_ra8_err_invalid_state;
  }
  RA8_RETURN_ON_ERROR(internal_demo_rebind_cache(), s_tag, "cache init");
  return ra8_io_blockdev_cache_stats(&s_cstate, nullptr, nullptr);
}

/**
 * @brief Run every leg in order and report the last leg's counters.
 *
 * @param[out] out_hits   Receives the hit counter of the final leg.
 * @param[out] out_misses Receives the miss counter of the final leg.
 * @return ra8_err_t Error from the first leg that failed.
 * @retval k_ra8_ok           Every leg passed.
 * @retval k_ra8_err_null_ptr An output pointer was NULL.
 * @retval k_ra8_err_*        Propagated from the failing leg.
 * @pre Both output pointers address writable 32-bit storage.
 * @post On success both counters hold the guard leg's totals.
 * @post On failure the run stops at the first failing leg.
 * @note Legs are ordered cheapest first so a break is easy to localise.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_demo_run(uint32_t* out_hits, uint32_t* out_misses)
{
  RA8_CHECK_NULL_PTR(out_hits, s_tag, "out_hits must not be nullptr");
  RA8_CHECK_NULL_PTR(out_misses, s_tag, "out_misses must not be nullptr");

  internal_demo_seed_disk();
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_ram_init(&s_under,
                                               &s_ustate,
                                               s_disk,
                                               (uint32_t)k_demo_disk_blocks,
                                               false),
                      s_tag,
                      "backend init");

  RA8_RETURN_ON_ERROR(internal_demo_leg_bind(), s_tag, "leg bind");
  RA8_RETURN_ON_ERROR(internal_demo_leg_hitmiss(), s_tag, "leg hitmiss");
  RA8_RETURN_ON_ERROR(internal_demo_leg_writethrough(), s_tag, "leg through");
  RA8_RETURN_ON_ERROR(internal_demo_leg_lru(), s_tag, "leg lru");
  RA8_RETURN_ON_ERROR(internal_demo_leg_erase(), s_tag, "leg erase");
  RA8_RETURN_ON_ERROR(internal_demo_leg_span(), s_tag, "leg span");
  RA8_RETURN_ON_ERROR(internal_demo_leg_guards(), s_tag, "leg guards");

  return ra8_io_blockdev_cache_stats(&s_cstate, out_hits, out_misses);
}

/**
 * @brief Firmware entry point.
 *
 * @pre SystemInit set VTOR / FPU / priority grouping.
 */
void main(void)
{
  ra8_log_init();
  internal_demo_setup_or_halt();
  (void)ra8_io_log_attach(&s_uart); /* route ra8_log into the console stream too */
  internal_demo_print("io_blockdev_cache_demo: boot\r\n");

  uint32_t        hits   = 0U;
  uint32_t        misses = 0U;
  const ra8_err_t e      = internal_demo_run(&hits, &misses);
  if (e == k_ra8_ok) {
    internal_demo_print("io_blockdev_cache_demo: legs=");
    (void)ra8_io_stream_put_u32(&s_uart, (uint32_t)k_demo_legs);
    internal_demo_print(" hits=");
    (void)ra8_io_stream_put_u32(&s_uart, hits);
    internal_demo_print(" misses=");
    (void)ra8_io_stream_put_u32(&s_uart, misses);
    internal_demo_print(" PASS\r\n");
  } else {
    internal_demo_print("io_blockdev_cache_demo: err=");
    (void)ra8_io_stream_put_u32(&s_uart, (uint32_t)e);
    internal_demo_print(" FAIL\r\n");
  }
  (void)ra8_board_uart_console_flush();
  while (true) {
  }
}
