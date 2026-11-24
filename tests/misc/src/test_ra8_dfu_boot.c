/**
 * @file test_ra8_dfu_boot.c
 * @brief Host unit tests for the pure boot logic of the ra8_dfu core.
 *
 * @details
 * Covers `ra8_dfu_crc32` (against published CRC32 check values),
 * `ra8_dfu_hdr_valid`, `ra8_dfu_run_target_valid`, `ra8_dfu_select_slot`, and
 * `ra8_dfu_boot_decide`. Every compound boolean decision in the production
 * source carries its MC/DC test vector pattern in the relevant `@par MC/DC:`
 * block.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_dfu.h"
#include "ra8_err.h"
#include "unity_minimal.h"

extern ra8_err_t priv_dfu_boot_state_corrupt_copy(uint8_t copy_idx, const uint8_t* raw32);
extern void      priv_dfu_boot_state_reset_store(void);

/** @brief Published CRC32 (IEEE) check values used as oracles. */
typedef enum : uint32_t {
  k_test_crc_empty     = 0x00000000U, /**< crc32("").            */
  k_test_crc_check     = 0xCBF43926U, /**< crc32("123456789").   */
  k_test_crc_a         = 0xE8B7BE43U, /**< crc32("a").           */
  k_test_crc_zero_byte = 0xD202EF8DU, /**< crc32(one 0x00 byte). */
} test_crc_oracle_t;

/** @brief A representative valid image length (a 32-byte multiple). */
typedef enum : uint32_t {
  k_test_len_ok      = 0x00004000U, /**< 16 KiB, page-aligned.    */
  k_test_len_unalign = 0x00000021U, /**< 33 bytes: not a 32-mult. */
  k_test_len_toobig  = 0x00078000U, /**< img_max + one page.      */
  k_test_crc_good    = 0x12345678U, /**< Arbitrary stored CRC.    */
  k_test_crc_bad     = 0x87654321U, /**< Differs from good.       */
} test_hdr_const_t;

/**
 * @brief Build a header with the given fields (entry fixed to the run base).
 */
static ra8_dfu_img_hdr_t mk_hdr(uint32_t magic, uint32_t seq, uint32_t len, uint32_t crc)
{
  ra8_dfu_img_hdr_t h = {};
  h.magic             = magic;
  h.seq               = seq;
  h.img_len           = len;
  h.img_crc32         = crc;
  h.entry             = (uint32_t)k_ra8_dfu_run_base;
  return h;
}

/**
 * @brief CRC32 matches published check values and folds empty to zero.
 *
 * @par MC/DC: not applicable -- straight-line fold; covered by oracle
 *      vectors (empty, "a", "123456789", single zero byte).
 *
 * @pre None.
 * @pre None.
 * @post No persistent side effects.
 * @post No global state changes.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_crc32_known_vectors(void)
{
  TEST_BEGIN("ra8_dfu: crc32 known vectors");

  TEST_ASSERT_EQ(k_test_crc_empty, ra8_dfu_crc32((const uint8_t*)"", 0U));
  TEST_ASSERT_EQ(k_test_crc_empty, ra8_dfu_crc32(nullptr, 0U));

  const uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  TEST_ASSERT_EQ(k_test_crc_check, ra8_dfu_crc32(check, (uint32_t)sizeof(check)));

  const uint8_t one_a = 'a';
  TEST_ASSERT_EQ(k_test_crc_a, ra8_dfu_crc32(&one_a, 1U));

  const uint8_t zero = 0x00U;
  TEST_ASSERT_EQ(k_test_crc_zero_byte, ra8_dfu_crc32(&zero, 1U));

  TEST_END("ra8_dfu: crc32 known vectors");
}

/**
 * @brief `ra8_dfu_hdr_valid` magic/length/CRC gate, with MC/DC vectors.
 *
 * @par MC/DC:
 * Outer decision: `magic_ok && len_ok && crc_ok` (3 conditions, all AND).
 * - V1: T T T -> true  (control: all conditions true)
 * - V2: F T T -> false (varies magic_ok)
 * - V3: T F T -> false (varies len_ok)
 * - V4: T T F -> false (varies crc_ok)
 * Inner decision `len_ok = (len!=0) && (len<=max) && (len%page==0)`
 * (3 conditions, all AND), driven with magic_ok=T, crc_ok=T:
 * - L1: 0x4000 -> true  (control: all true)
 * - L2: 0      -> false (varies len!=0)
 * - L3: img_max+page -> false (varies len<=max)
 * - L4: 0x21   -> false (varies len%page==0)
 * N+1 = 4 vectors per 3-condition AND: minimal MC/DC. NULL header -> false.
 *
 * @pre None.
 * @pre None.
 * @post No persistent side effects.
 * @post No global state changes.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_hdr_valid_mcdc(void)
{
  TEST_BEGIN("ra8_dfu: hdr_valid magic/len/crc (MC/DC)");

  /* NULL guard. */
  TEST_ASSERT(!ra8_dfu_hdr_valid(nullptr, (uint32_t)k_test_crc_good));

  /* Outer AND: magic_ok && len_ok && crc_ok. */
  const ra8_dfu_img_hdr_t v1 =
    mk_hdr((uint32_t)k_ra8_dfu_hdr_magic, 1U, (uint32_t)k_test_len_ok, (uint32_t)k_test_crc_good);
  TEST_ASSERT(ra8_dfu_hdr_valid(&v1, (uint32_t)k_test_crc_good)); /* T T T */

  const ra8_dfu_img_hdr_t v2 = mk_hdr((uint32_t)k_ra8_dfu_hdr_magic + 1U,
                                      1U,
                                      (uint32_t)k_test_len_ok,
                                      (uint32_t)k_test_crc_good);
  TEST_ASSERT(!ra8_dfu_hdr_valid(&v2, (uint32_t)k_test_crc_good)); /* F T T */

  /* T F T -> false: len unaligned (also covers inner L4). */
  const ra8_dfu_img_hdr_t v3 = mk_hdr((uint32_t)k_ra8_dfu_hdr_magic,
                                      1U,
                                      (uint32_t)k_test_len_unalign,
                                      (uint32_t)k_test_crc_good);
  TEST_ASSERT(!ra8_dfu_hdr_valid(&v3, (uint32_t)k_test_crc_good)); /* T F T */

  /* T T F -> false: computed CRC differs from stored. */
  TEST_ASSERT(!ra8_dfu_hdr_valid(&v1, (uint32_t)k_test_crc_bad)); /* T T F */

  /* Inner len_ok vectors (magic_ok=T, crc_ok=T held). */
  const ra8_dfu_img_hdr_t l2 =
    mk_hdr((uint32_t)k_ra8_dfu_hdr_magic, 1U, 0U, (uint32_t)k_test_crc_good);
  TEST_ASSERT(!ra8_dfu_hdr_valid(&l2, (uint32_t)k_test_crc_good)); /* len == 0 */

  const ra8_dfu_img_hdr_t l3 = mk_hdr((uint32_t)k_ra8_dfu_hdr_magic,
                                      1U,
                                      (uint32_t)k_test_len_toobig,
                                      (uint32_t)k_test_crc_good);
  TEST_ASSERT(!ra8_dfu_hdr_valid(&l3, (uint32_t)k_test_crc_good)); /* len > max */

  /* Boundary: exactly img_max is valid (page-aligned). */
  const ra8_dfu_img_hdr_t lmax = mk_hdr((uint32_t)k_ra8_dfu_hdr_magic,
                                        1U,
                                        (uint32_t)k_ra8_dfu_img_max,
                                        (uint32_t)k_test_crc_good);
  TEST_ASSERT(ra8_dfu_hdr_valid(&lmax, (uint32_t)k_test_crc_good));

  TEST_END("ra8_dfu: hdr_valid magic/len/crc (MC/DC)");
}

/**
 * @brief `ra8_dfu_run_target_valid` entry + length gate, with MC/DC vectors.
 *
 * @par MC/DC:
 * Outer decision: `entry_ok && len_ok` (2 conditions, AND).
 * - V1: T T -> true  (control: both true)
 * - V2: F T -> false (varies entry_ok: entry != run base)
 * - V3: T F -> false (varies len_ok: len == 0)
 * Inner decision `len_ok = (len!=0) && (len<=max) && (len%page==0)`
 * (3 conditions, all AND), driven with entry_ok=T (result == len_ok):
 * - L1: 0x4000        -> true  (control: all true)
 * - L2: 0             -> false (varies len!=0)
 * - L3: img_max+page  -> false (varies len<=max)
 * - L4: 0x21          -> false (varies len%page==0)
 * N+1 vectors per AND: minimal MC/DC. Boundary img_max -> true (<= inclusive).
 *
 * @pre None.
 * @pre None.
 * @post No persistent side effects.
 * @post No global state changes.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_run_target_valid_mcdc(void)
{
  TEST_BEGIN("ra8_dfu: run_target_valid entry/len (MC/DC)");

  const uint32_t run       = (uint32_t)k_ra8_dfu_run_base;
  const uint32_t bad_entry = (uint32_t)k_ra8_dfu_slot_a_base; /* a real address != run base */

  /* Outer AND: entry_ok && len_ok. */
  TEST_ASSERT(ra8_dfu_run_target_valid(run, (uint32_t)k_test_len_ok));        /* T T -> true  */
  TEST_ASSERT(!ra8_dfu_run_target_valid(bad_entry, (uint32_t)k_test_len_ok)); /* F T -> false */
  TEST_ASSERT(!ra8_dfu_run_target_valid(run, 0U));                            /* T F -> false */

  /* Inner len_ok vectors (entry_ok=T held, so result == len_ok). */
  TEST_ASSERT(!ra8_dfu_run_target_valid(run, (uint32_t)k_test_len_toobig));  /* len > max   */
  TEST_ASSERT(!ra8_dfu_run_target_valid(run, (uint32_t)k_test_len_unalign)); /* len%page!=0 */

  /* Boundary: exactly img_max (page-aligned) is valid. */
  TEST_ASSERT(ra8_dfu_run_target_valid(run, (uint32_t)k_ra8_dfu_img_max));

  TEST_END("ra8_dfu: run_target_valid entry/len (MC/DC)");
}

/**
 * @brief `ra8_dfu_select_slot` A/B/none selection, with MC/DC vectors.
 *
 * @par MC/DC:
 * Decision 1 (none): `!a_valid && !b_valid` (2 conditions).
 * - V1: a=F,b=F -> true  (control)
 * - V2: a=T,b=F -> false (varies !a_valid)
 * - V3: a=F,b=T -> false (varies !b_valid)
 * Decision 2 (slot A): `a_valid && (!b_valid || (a_seq >= b_seq))`
 * (A=a_valid, B=!b_valid, C=(a_seq>=b_seq)):
 * - (a=T,b=F)            -> A T, B T          -> slot_a (control true; B-vary high)
 * - (a=F,b=T)            -> A F               -> slot_b (varies A vs T,T,a>=b)
 * - (a=T,b=T,a_seq>=b)   -> A T, B F, C T     -> slot_a (varies C high)
 * - (a=T,b=T,a_seq<b)    -> A T, B F, C F     -> slot_b (varies C low; B-vary low vs a=T,b=F,a<b)
 * Minimal MC/DC for each decision.
 *
 * @pre None.
 * @pre None.
 * @post No persistent side effects.
 * @post No global state changes.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_select_slot_mcdc(void)
{
  TEST_BEGIN("ra8_dfu: select_slot A/B/none (MC/DC)");

  /* none-decision MC/DC. */
  TEST_ASSERT_EQ(k_ra8_dfu_slot_none, ra8_dfu_select_slot(false, 0U, false, 0U));
  TEST_ASSERT_EQ(k_ra8_dfu_slot_a, ra8_dfu_select_slot(true, 5U, false, 0U));
  TEST_ASSERT_EQ(k_ra8_dfu_slot_b, ra8_dfu_select_slot(false, 0U, true, 5U));

  /* slot-A-decision MC/DC. */
  TEST_ASSERT_EQ(k_ra8_dfu_slot_a, ra8_dfu_select_slot(true, 9U, true, 3U));  /* a>=b high   */
  TEST_ASSERT_EQ(k_ra8_dfu_slot_b, ra8_dfu_select_slot(true, 3U, true, 9U));  /* a<b low     */
  TEST_ASSERT_EQ(k_ra8_dfu_slot_a, ra8_dfu_select_slot(true, 4U, true, 4U));  /* tie -> A    */
  TEST_ASSERT_EQ(k_ra8_dfu_slot_a, ra8_dfu_select_slot(true, 1U, false, 9U)); /* B-vary high */

  TEST_END("ra8_dfu: select_slot A/B/none (MC/DC)");
}

/**
 * @brief `ra8_dfu_boot_decide` trigger + slot mapping, with MC/DC vectors.
 *
 * @par MC/DC:
 * Decision: `if (dfu_trigger)` (1 condition).
 * - V1: trigger=T -> DFU   (varies trigger on)
 * - V2: trigger=F -> jump  (varies trigger off)
 * N+1 = 2 vectors. The slot mapping is delegated to select_slot (covered
 * by test_select_slot_mcdc); here we confirm each select_slot outcome
 * maps to the right action and that "no valid slot" also enters DFU.
 *
 * @pre None.
 * @pre None.
 * @post No persistent side effects.
 * @post No global state changes.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_boot_decide_mcdc(void)
{
  TEST_BEGIN("ra8_dfu: boot_decide trigger + mapping (MC/DC)");

  /* Trigger wins even with a valid slot. */
  TEST_ASSERT_EQ(k_ra8_dfu_action_dfu, ra8_dfu_boot_decide(true, true, 5U, false, 0U));

  /* No trigger: map select_slot. */
  TEST_ASSERT_EQ(k_ra8_dfu_action_jump_a, ra8_dfu_boot_decide(false, true, 5U, false, 0U));
  TEST_ASSERT_EQ(k_ra8_dfu_action_jump_b, ra8_dfu_boot_decide(false, false, 0U, true, 5U));
  TEST_ASSERT_EQ(k_ra8_dfu_action_jump_b, ra8_dfu_boot_decide(false, true, 3U, true, 9U));
  TEST_ASSERT_EQ(k_ra8_dfu_action_jump_a, ra8_dfu_boot_decide(false, true, 9U, true, 3U));

  /* No trigger, no valid slot -> DFU. */
  TEST_ASSERT_EQ(k_ra8_dfu_action_dfu, ra8_dfu_boot_decide(false, false, 0U, false, 0U));

  TEST_END("ra8_dfu: boot_decide trigger + mapping (MC/DC)");
}

/**
 * @brief Validate ::ra8_dfu_boot_state_valid checks (null, magic, seq, range, CRC).
 *
 * @par MC/DC:
 * Checks each guard condition independently: NULL pointer, bad magic, zero
 * `write_seq`, out-of-range `trial_slot`, out-of-range `confirmed`, and bit-flipped
 * payload causing CRC32 mismatch against an otherwise valid baseline record.
 *
 * @pre None.
 * @post No persistent side effects.
 * @note Test-only. @since 0.1.0
 */
static void test_boot_state_valid_mcdc(void)
{
  TEST_BEGIN("ra8_dfu: boot_state_valid invariants + CRC (MC/DC)");

  TEST_ASSERT(!ra8_dfu_boot_state_valid(nullptr));

  ra8_dfu_boot_state_t good = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_dfu_boot_state_prepare(k_ra8_dfu_slot_b,
                                            (uint8_t)k_ra8_dfu_boot_state_default_tries,
                                            false,
                                            7U,
                                            1U,
                                            &good));
  TEST_ASSERT(ra8_dfu_boot_state_valid(&good));

  ra8_dfu_boot_state_t bad_magic = good;
  bad_magic.magic ^= 1U;
  TEST_ASSERT(!ra8_dfu_boot_state_valid(&bad_magic));

  ra8_dfu_boot_state_t bad_seq = good;
  bad_seq.write_seq            = 0U;
  TEST_ASSERT(!ra8_dfu_boot_state_valid(&bad_seq));

  /* Re-CRC with corrupted enum/bool bytes to prove range guards run too. */
  ra8_dfu_boot_state_t bad_slot = good;
  bad_slot.trial_slot           = (uint8_t)k_ra8_dfu_slot_none + 1U;
  bad_slot.crc32 =
    ra8_dfu_crc32((const uint8_t*)&bad_slot, (uint32_t)k_ra8_dfu_boot_state_crc_span);
  TEST_ASSERT(!ra8_dfu_boot_state_valid(&bad_slot));

  ra8_dfu_boot_state_t bad_conf = good;
  bad_conf.confirmed            = 2U;
  bad_conf.crc32 =
    ra8_dfu_crc32((const uint8_t*)&bad_conf, (uint32_t)k_ra8_dfu_boot_state_crc_span);
  TEST_ASSERT(!ra8_dfu_boot_state_valid(&bad_conf));

  ra8_dfu_boot_state_t bad_crc = good;
  bad_crc.tries_left ^= 1U;
  TEST_ASSERT(!ra8_dfu_boot_state_valid(&bad_crc));

  TEST_END("ra8_dfu: boot_state_valid invariants + CRC (MC/DC)");
}

/**
 * @brief Arbitrate ping-pong copies cleanly across torn and corrupt records.
 *
 * @par MC/DC:
 * Drives all outcomes of `!v0 && !v1` and `v0 && (!v1 || s0 >= s1)` (none valid,
 * copy 0 only, copy 1 only, copy 1 newer, copy 0 newer, tie -> copy 0) plus null
 * guards.
 *
 * @pre None.
 * @post `*out_state` unchanged when `k_ra8_err_not_found` is returned.
 * @note Test-only. @since 0.1.0
 */
static void test_boot_state_pick_mcdc(void)
{
  TEST_BEGIN("ra8_dfu: boot_state_pick arbitration + torn copies (MC/DC)");

  ra8_dfu_boot_state_t erased = {};
  (void)memset(&erased, 0xFF, sizeof(erased));
  ra8_dfu_boot_state_t r1 = {};
  ra8_dfu_boot_state_t r2 = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dfu_boot_state_prepare(k_ra8_dfu_slot_a, 3U, false, 4U, 1U, &r1));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dfu_boot_state_prepare(k_ra8_dfu_slot_b, 2U, false, 5U, 2U, &r2));

  ra8_dfu_boot_state_t out = {};
  uint8_t              idx = 99U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dfu_boot_state_pick(nullptr, &r1, &out, &idx));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dfu_boot_state_pick(&r1, nullptr, &out, &idx));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dfu_boot_state_pick(&r1, &r2, nullptr, &idx));

  /* Neither valid -> not found, output preserved. */
  out = r1;
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_dfu_boot_state_pick(&erased, &erased, &out, &idx));
  TEST_ASSERT_EQ(1U, out.write_seq);

  /* Only copy 0 valid (e.g. copy 1 torn mid-write). */
  ra8_dfu_boot_state_t torn = r2;
  torn.crc32 ^= 0xA5U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dfu_boot_state_pick(&r1, &torn, &out, &idx));
  TEST_ASSERT_EQ(0U, idx);
  TEST_ASSERT_EQ(1U, out.write_seq);

  /* Only copy 1 valid -> copy 1 wins. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dfu_boot_state_pick(&erased, &r2, &out, nullptr));
  TEST_ASSERT_EQ(2U, out.write_seq);

  /* Both valid -> higher write_seq wins regardless of slot position; tie -> 0. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dfu_boot_state_pick(&r1, &r2, &out, &idx));
  TEST_ASSERT_EQ(1U, idx);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dfu_boot_state_pick(&r2, &r1, &out, &idx));
  TEST_ASSERT_EQ(0U, idx);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dfu_boot_state_pick(&r2, &r2, &out, &idx));
  TEST_ASSERT_EQ(0U, idx);

  TEST_END("ra8_dfu: boot_state_pick arbitration + torn copies (MC/DC)");
}

/**
 * @brief Round-trip store read/write, torn-write recovery, and corrupt-record rejection.
 *
 * @par MC/DC: not applicable -- sequential store fault-injection checks.
 * @pre Host off-target store shadow available.
 * @post Host shadow left erased.
 * @note Test-only. @since 0.1.0
 */
static void test_boot_state_store_torn_and_corrupt(void)
{
  TEST_BEGIN("ra8_dfu: boot_state store read/write + torn/corrupt");

  priv_dfu_boot_state_reset_store();

  ra8_dfu_boot_state_t out = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dfu_boot_state_read(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_dfu_boot_state_prepare(k_ra8_dfu_slot_a, 1U, false, 1U, 1U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_dfu_boot_state_prepare((ra8_dfu_slot_t)((uint8_t)k_ra8_dfu_slot_none + 1U),
                                            1U,
                                            false,
                                            1U,
                                            1U,
                                            &out));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_dfu_boot_state_prepare(k_ra8_dfu_slot_a, 1U, false, 1U, 0U, &out));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_dfu_boot_state_read(&out));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_dfu_boot_state_write((ra8_dfu_slot_t)((uint8_t)k_ra8_dfu_slot_none + 1U), 1U, false, 1U));

  /* Write generation 1 -> lands on copy 0; generation 2 -> lands on copy 1. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dfu_boot_state_write(k_ra8_dfu_slot_b, 3U, false, 10U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dfu_boot_state_read(&out));
  TEST_ASSERT_EQ(1U, out.write_seq);
  TEST_ASSERT_EQ((uint8_t)k_ra8_dfu_slot_b, out.trial_slot);
  TEST_ASSERT_EQ(3U, out.tries_left);
  TEST_ASSERT_EQ(0U, out.confirmed);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_dfu_boot_state_write(k_ra8_dfu_slot_b, 2U, false, 10U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dfu_boot_state_read(&out));
  TEST_ASSERT_EQ(2U, out.write_seq);
  TEST_ASSERT_EQ(2U, out.tries_left);

  /* Torn write on copy 1 (erased mid-write) recovers generation 1 from copy 0. */
  uint8_t erased[sizeof(ra8_dfu_boot_state_t)] = {};
  (void)memset(erased, 0xFF, sizeof(erased));
  TEST_ASSERT_EQ(k_ra8_ok, priv_dfu_boot_state_corrupt_copy(1U, erased));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dfu_boot_state_read(&out));
  TEST_ASSERT_EQ(1U, out.write_seq);
  TEST_ASSERT_EQ(3U, out.tries_left);

  /* Write confirmed record (seq 2 -> copy 1), then confirm gen 3 -> copy 0,
   * then corrupt copy 0 -> falls back to intact gen 2 in copy 1. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dfu_boot_state_write(k_ra8_dfu_slot_b, 2U, false, 10U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dfu_boot_state_write(k_ra8_dfu_slot_b, 2U, true, 10U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dfu_boot_state_read(&out));
  TEST_ASSERT_EQ(3U, out.write_seq);
  TEST_ASSERT_EQ(1U, out.confirmed);

  uint8_t bad_bytes[sizeof(ra8_dfu_boot_state_t)] = {};
  (void)memcpy(bad_bytes, &out, sizeof(bad_bytes));
  bad_bytes[0] ^= 0x55U;
  TEST_ASSERT_EQ(k_ra8_ok, priv_dfu_boot_state_corrupt_copy(0U, bad_bytes));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dfu_boot_state_read(&out));
  TEST_ASSERT_EQ(2U, out.write_seq);
  TEST_ASSERT_EQ(0U, out.confirmed);

  /* Corrupting both copies yields k_ra8_err_not_found cleanly. */
  TEST_ASSERT_EQ(k_ra8_ok, priv_dfu_boot_state_corrupt_copy(1U, bad_bytes));
  out.write_seq = 0xCAFEBABEU;
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_dfu_boot_state_read(&out));
  TEST_ASSERT_EQ(0xCAFEBABEU, out.write_seq);

  /* Erase wipes both copies. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dfu_boot_state_write(k_ra8_dfu_slot_a, 1U, true, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dfu_boot_state_erase_all());
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_dfu_boot_state_read(&out));

  TEST_END("ra8_dfu: boot_state store read/write + torn/corrupt");
}

int main(void)
{
  test_crc32_known_vectors();
  test_hdr_valid_mcdc();
  test_run_target_valid_mcdc();
  test_select_slot_mcdc();
  test_boot_decide_mcdc();
  test_boot_state_valid_mcdc();
  test_boot_state_pick_mcdc();
  test_boot_state_store_torn_and_corrupt();
  return 0;
}
