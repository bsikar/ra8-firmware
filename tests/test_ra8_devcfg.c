/**
 * @file test_ra8_devcfg.c
 * @brief Unit tests for the per-device configuration record (``ra8_devcfg``).
 *
 * @details
 * Pure host tests. The durable medium is reached only through the injected
 * ::ra8_devcfg_store_t seam, so every branch is drivable from a RAM mock. A
 * final section drives the production ::ra8_devcfg_default_store binding
 * through its ``RA8_OFF_TARGET`` RAM shadow.
 *
 * What we cover:
 *   - Codec round-trip: commit then load reproduces every body field.
 *   - Integrity: single-bit body corruption is rejected by CRC; a wrong
 *     magic, an unknown (future) schema, and a wrong ``record_len`` are each
 *     rejected independently (the four-gate validation chain).
 *   - Two-copy resolution: both valid picks the higher ``seq`` (and a tie
 *     takes copy 0); one valid uses it; neither is UNPROVISIONED.
 *   - Torn-write recovery: a commit whose header write fails leaves the other
 *     copy the sole survivor.
 *   - VCOM gate: the explicit valid flag, the plausible-range window (with the
 *     poison values 0 and 0xFFFF), and the not-initialised guard.
 *   - Null-argument guards on every public entry point.
 *   - The extra-MRAM backend: blank shadow resolves to UNPROVISIONED, a
 *     round-trip persists, and out-of-range / null accesses are refused.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_devcfg.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/**
 * @enum dc_test_const_t
 * @brief Test-only numeric constants (no bare magic-number literals).
 */
typedef enum : uint32_t {
  k_dc_region      = 448U,        /**< copy1_off (0x100) + slot (192).          */
  k_dc_good_mv     = 1530U,       /**< In-range VCOM (-1.53 V).                 */
  k_dc_other_mv    = 1670U,       /**< A second in-range VCOM.                  */
  k_dc_low_mv      = 100U,        /**< Below k_ra8_devcfg_vcom_min_mv.          */
  k_dc_high_mv     = 5000U,       /**< Above k_ra8_devcfg_vcom_max_mv.          */
  k_dc_zero_mv     = 0U,          /**< Controller-still-booting poison.         */
  k_dc_blank_mv    = 0xFFFFU,     /**< Blank / failed-read poison.              */
  k_dc_hw_rev      = 2U,          /**< Sample board revision.                   */
  k_dc_fixture     = 7U,          /**< Sample fixture id.                       */
  k_dc_mfg_date    = 20260723U,   /**< Sample packed YYYYMMDD.                  */
  k_dc_key_id      = 0xDEADBEEFU, /**< Sample device-key identifier.            */
  k_dc_bit_flip    = 0x01U,       /**< XOR mask for single-bit corruption.      */
  k_dc_bad_schema  = 99U,         /**< Schema from an unknown future writer.    */
  k_dc_scratch_len = 4U,          /**< Scratch buffer length for edge reads.    */
  k_dc_blank_byte  = 0xFFU,       /**< Value an unprogrammed window byte reads. */
} dc_test_const_t;

/* ===========================================================================
 * RAM mock store (offset-addressed; models blank = 0xFF)
 * ===========================================================================
 */

/**
 * @var s_mem
 * @brief Backing bytes for the mock store; 0xFF models an unprogrammed window.
 */
static uint8_t s_mem[k_dc_region];

/**
 * @var s_write_budget
 * @brief Successful writes remaining before the mock forces a fault.
 * @details ``-1`` allows unlimited writes; a non-negative value fails once it
 *          reaches zero, modelling a power cut mid-commit.
 */
static int s_write_budget = -1;

/**
 * @brief Reset the mock to a blank (all-0xFF) window with unlimited writes.

 *
 * @details Restores every simulated medium byte to its erased value, clears the write-fault budget, and resets loaded configuration state.
 * @pre The RAM medium and configuration singleton are exclusively owned by this test vector.
 * @pre Every supplied range lies within its documented mock-store or record capacity.
 * @post Every write remains within the fixed RAM medium and caller-owned output storage.
 * @post No heap allocation, host stream, or physical nonvolatile medium is accessed.
 * @note Test-only and not reentrant because the medium and driver state have file scope.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_dc_mock_blank(void)
{
  (void)memset(s_mem, (int)k_dc_blank_byte, sizeof s_mem);
  s_write_budget = -1;
  ra8_devcfg_reset();
}

/** @brief Mock ::ra8_devcfg_read_fn_t reading from ::s_mem.
 *
 * @details Copies one bounded range from the erased-state RAM medium while rejecting null destinations and forged extents.
 * @param[in] offset First medium byte to read.
 * @param[out] dst Writable destination buffer.
 * @param[in] len Requested byte count.
 * @return Mock store read status.
 * @retval k_ra8_ok The complete bounded range was copied.
 * @pre The RAM medium and configuration singleton are exclusively owned by this test vector.
 * @pre Every supplied range lies within its documented mock-store or record capacity.
 * @post Every write remains within the fixed RAM medium and caller-owned output storage.
 * @post No heap allocation, host stream, or physical nonvolatile medium is accessed.
 * @note Test-only and not reentrant because the medium and driver state have file scope.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_dc_mock_read(uint32_t offset, uint8_t* dst, uint32_t len)
{
  if (dst == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if ((offset + len) > (uint32_t)sizeof s_mem) {
    return k_ra8_err_out_of_range;
  }
  (void)memcpy(dst, &s_mem[offset], (size_t)len);
  return k_ra8_ok;
}

/** @brief Mock ::ra8_devcfg_write_fn_t writing into ::s_mem with fault budget.
 *
 * @details Applies the configured write-fault budget, then copies one bounded source range into the simulated durable medium.
 * @param[in] offset First medium byte to program.
 * @param[in] src Source bytes to persist.
 * @param[in] len Requested byte count.
 * @return Mock store write status.
 * @retval k_ra8_ok The complete bounded range was persisted.
 * @pre The RAM medium and configuration singleton are exclusively owned by this test vector.
 * @pre Every supplied range lies within its documented mock-store or record capacity.
 * @post Every write remains within the fixed RAM medium and caller-owned output storage.
 * @post No heap allocation, host stream, or physical nonvolatile medium is accessed.
 * @note Test-only and not reentrant because the medium and driver state have file scope.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_dc_mock_write(uint32_t offset, const uint8_t* src, uint32_t len)
{
  if (src == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if ((offset + len) > (uint32_t)sizeof s_mem) {
    return k_ra8_err_out_of_range;
  }
  if (s_write_budget == 0) {
    return k_ra8_err_hw_error;
  }
  if (s_write_budget > 0) {
    s_write_budget--;
  }
  (void)memcpy(&s_mem[offset], src, (size_t)len);
  return k_ra8_ok;
}

/** @brief The mock store injected by every logic test. */
static const ra8_devcfg_store_t s_mock = {.read  = internal_dc_mock_read,
                                          .write = internal_dc_mock_write};

/**
 * @brief Build a populated record with the given VCOM and flags.

 *
 * @details Builds a deterministic configuration record with fixed identity/calibration fields and caller-selected VCOM and flags.
 * @param[in] vcom Panel VCOM magnitude in millivolts.
 * @param[in] flags Record validity and provisioning flags.
 * @return Populated value-owned configuration record.
 * @retval populated All deterministic body fields, VCOM, and flags are initialized.
 * @pre The RAM medium and configuration singleton are exclusively owned by this test vector.
 * @pre Every supplied range lies within its documented mock-store or record capacity.
 * @post Every write remains within the fixed RAM medium and caller-owned output storage.
 * @post No heap allocation, host stream, or physical nonvolatile medium is accessed.
 * @note Test-only and not reentrant because the medium and driver state have file scope.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_devcfg_record_t internal_dc_make_rec(uint16_t vcom, uint32_t flags)
{
  ra8_devcfg_record_t r = {};
  (void)memcpy(r.body.serial, "SN-0001", sizeof("SN-0001") - 1U);
  (void)memcpy(r.body.panel_serial, "PANEL-01", sizeof("PANEL-01") - 1U);
  (void)memcpy(r.body.panel_lut_id, "M641", sizeof("M641") - 1U);
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_devcfg_touch_cal_len; i++) {
    r.body.touch_cal[i] = i;
  }
  r.body.mfg_date      = (uint32_t)k_dc_mfg_date;
  r.body.device_key_id = (uint32_t)k_dc_key_id;
  r.body.hw_rev        = (uint16_t)k_dc_hw_rev;
  r.body.fixture_id    = (uint16_t)k_dc_fixture;
  r.body.panel_vcom_mv = vcom;
  r.flags              = flags;
  return r;
}

/** @brief Poke one byte inside copy 0 of the mock memory.
 *
 * @details Overwrites one byte in copy zero so individual header/body validation gates can be failed independently.
 * @param[in] field_off Byte offset within copy zero.
 * @param[in] value Replacement byte value.
 * @pre The RAM medium and configuration singleton are exclusively owned by this test vector.
 * @pre Every supplied range lies within its documented mock-store or record capacity.
 * @post Every write remains within the fixed RAM medium and caller-owned output storage.
 * @post No heap allocation, host stream, or physical nonvolatile medium is accessed.
 * @note Test-only and not reentrant because the medium and driver state have file scope.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_dc_poke_copy0(uint8_t field_off, uint8_t value)
{
  s_mem[(uint32_t)k_ra8_devcfg_copy0_off + field_off] = value;
}

/* ===========================================================================
 * Tests
 * ===========================================================================
 */

/**
 * @test internal_dc_commit_load_roundtrip -- a committed record reloads field-for-field.

 * @brief Commits a fully populated record, reloads it through the injected store, and compares every persisted field exactly.
 *
 * @details Commits a fully populated record, reloads it through the injected store, and compares every persisted field exactly.
 * @pre The RAM medium and configuration singleton are exclusively owned by this test vector.
 * @pre Every supplied range lies within its documented mock-store or record capacity.
 * @post Every write remains within the fixed RAM medium and caller-owned output storage.
 * @post No heap allocation, host stream, or physical nonvolatile medium is accessed.
 * @note Test-only and not reentrant because the medium and driver state have file scope.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_dc_commit_load_roundtrip(void)
{
  TEST_BEGIN("devcfg: commit/load round-trip");
  internal_dc_mock_blank();
  const ra8_devcfg_record_t rec = internal_dc_make_rec((uint16_t)k_dc_good_mv,
                                                       (uint32_t)k_ra8_devcfg_flag_provisioned |
                                                         (uint32_t)k_ra8_devcfg_flag_vcom_valid |
                                                         (uint32_t)k_ra8_devcfg_flag_touch_valid);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_devcfg_commit(&s_mock, &rec));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_devcfg_load(&s_mock));

  const ra8_devcfg_body_t* body = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_devcfg_get_body(&body));
  TEST_ASSERT_NOT_NULL(body);
  TEST_ASSERT(memcmp(body->serial, rec.body.serial, (size_t)k_ra8_devcfg_serial_len) == 0);
  TEST_ASSERT(
    memcmp(body->panel_serial, rec.body.panel_serial, (size_t)k_ra8_devcfg_panel_serial_len) == 0);
  TEST_ASSERT(memcmp(body->touch_cal, rec.body.touch_cal, (size_t)k_ra8_devcfg_touch_cal_len) == 0);
  TEST_ASSERT_EQ(k_dc_mfg_date, body->mfg_date);
  TEST_ASSERT_EQ(k_dc_key_id, body->device_key_id);
  TEST_ASSERT_EQ(k_dc_hw_rev, body->hw_rev);
  TEST_ASSERT_EQ(k_dc_fixture, body->fixture_id);
  TEST_ASSERT_EQ(k_dc_good_mv, body->panel_vcom_mv);
  TEST_ASSERT(!ra8_devcfg_is_blank());
  TEST_END("devcfg: commit/load round-trip");
}

/**
 * @test internal_dc_validation_chain_mcdc -- the four independent integrity gates.
 *
 * @par MC/DC:
 * Decision (accept a copy): `magic_ok && schema_ok && reclen_ok && crc_ok`
 * (4 conditions, evaluated as a short-circuit AND chain). Seed one valid copy 0
 * (copy 1 blank) so exactly one condition can be flipped at a time:
 * - Vector 1: all four gates pass                        -> loaded (control).
 * - Vector 2: magic byte flipped, others pass            -> UNPROVISIONED.
 * - Vector 3: schema set to a future value, others pass  -> UNPROVISIONED.
 * - Vector 4: record_len byte flipped, others pass       -> UNPROVISIONED.
 * - Vector 5: one body byte flipped (CRC fails), others  -> UNPROVISIONED.
 * Vectors 1+2 show magic, 1+3 schema, 1+4 record_len and 1+5 CRC each
 * independently drive acceptance. N+1 = 5 vectors for N=4 conditions.

 * @brief Varies magic, schema, record length, and CRC independently around one valid copy to cover the validation conjunction.
 *
 * @details Varies magic, schema, record length, and CRC independently around one valid copy to cover the validation conjunction.
 * @pre The RAM medium and configuration singleton are exclusively owned by this test vector.
 * @pre Every supplied range lies within its documented mock-store or record capacity.
 * @post Every write remains within the fixed RAM medium and caller-owned output storage.
 * @post No heap allocation, host stream, or physical nonvolatile medium is accessed.
 * @note Test-only and not reentrant because the medium and driver state have file scope.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_dc_validation_chain_mcdc(void)
{
  TEST_BEGIN("devcfg MC/DC: validation 4-gate chain");
  const ra8_devcfg_record_t rec =
    internal_dc_make_rec((uint16_t)k_dc_good_mv, (uint32_t)k_ra8_devcfg_flag_vcom_valid);

  /* Vector 1 -- pristine valid copy 0. */
  internal_dc_mock_blank();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_devcfg_commit(&s_mock, &rec));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_devcfg_load(&s_mock));

  /* Vector 2 -- corrupt the magic word. */
  ra8_devcfg_reset();
  internal_dc_poke_copy0((uint8_t)k_ra8_devcfg_off_magic,
                         s_mem[k_ra8_devcfg_copy0_off] ^ (uint8_t)k_dc_bit_flip);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_devcfg_load(&s_mock));

  /* Vector 3 -- a schema from a newer, unknown writer. */
  internal_dc_mock_blank();
  (void)ra8_devcfg_commit(&s_mock, &rec);
  internal_dc_poke_copy0((uint8_t)k_ra8_devcfg_off_schema, (uint8_t)k_dc_bad_schema);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_devcfg_load(&s_mock));

  /* Vector 4 -- a wrong record_len. */
  internal_dc_mock_blank();
  (void)ra8_devcfg_commit(&s_mock, &rec);
  internal_dc_poke_copy0((uint8_t)k_ra8_devcfg_off_reclen,
                         s_mem[k_ra8_devcfg_copy0_off + k_ra8_devcfg_off_reclen] ^
                           (uint8_t)k_dc_bit_flip);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_devcfg_load(&s_mock));

  /* Vector 5 -- one body byte flipped so the CRC no longer matches. */
  internal_dc_mock_blank();
  (void)ra8_devcfg_commit(&s_mock, &rec);
  internal_dc_poke_copy0((uint8_t)k_ra8_devcfg_off_serial,
                         s_mem[k_ra8_devcfg_copy0_off + k_ra8_devcfg_off_serial] ^
                           (uint8_t)k_dc_bit_flip);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_devcfg_load(&s_mock));
  TEST_ASSERT(ra8_devcfg_is_blank());
  TEST_END("devcfg MC/DC: validation 4-gate chain");
}

/**
 * @test internal_dc_resolution_mcdc -- two-copy selection by validity and sequence.
 *
 * @par MC/DC:
 * Decision (both survive): `valid0 && valid1` (2 conditions).
 * - Vector 1: valid0=1, valid1=1 -> both valid, higher seq wins (control).
 * - Vector 2: valid0=0, valid1=1 -> copy 1 chosen (varies valid0).
 * - Vector 3: valid0=1, valid1=0 -> copy 0 chosen (varies valid1).
 * - Vector 4: valid0=0, valid1=0 -> UNPROVISIONED (the AND's else).
 * Vectors 1+2 prove valid0, 1+3 prove valid1 independently affect the outcome.
 * The higher-seq tie-break (`rec0.seq >= rec1.seq`) is exercised in both
 * directions: copy 0 newer and copy 1 newer.

 * @brief Exercises all two-copy validity combinations and both sequence-order outcomes, including the no-valid-copy failure.
 *
 * @details Exercises all two-copy validity combinations and both sequence-order outcomes, including the no-valid-copy failure.
 * @pre The RAM medium and configuration singleton are exclusively owned by this test vector.
 * @pre Every supplied range lies within its documented mock-store or record capacity.
 * @post Every write remains within the fixed RAM medium and caller-owned output storage.
 * @post No heap allocation, host stream, or physical nonvolatile medium is accessed.
 * @note Test-only and not reentrant because the medium and driver state have file scope.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_dc_resolution_mcdc(void)
{
  TEST_BEGIN("devcfg MC/DC: two-copy resolution");
  const ra8_devcfg_record_t r0 =
    internal_dc_make_rec((uint16_t)k_dc_good_mv, (uint32_t)k_ra8_devcfg_flag_vcom_valid);
  const ra8_devcfg_record_t r1 =
    internal_dc_make_rec((uint16_t)k_dc_other_mv, (uint32_t)k_ra8_devcfg_flag_vcom_valid);
  uint16_t mv = 0U;

  /* Vector 1 -- both valid. commit #1 -> copy0 seq1 (good_mv);
   * commit #2 -> copy1 seq2 (other_mv). Higher seq (copy1) wins. */
  internal_dc_mock_blank();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_devcfg_commit(&s_mock, &r0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_devcfg_commit(&s_mock, &r1));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_devcfg_load(&s_mock));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_devcfg_get_vcom_mv(&mv));
  TEST_ASSERT_EQ(k_dc_other_mv, mv); /* copy1 (seq2) newer -> rec0.seq(1) >= rec1.seq(2) is false */

  /* commit #3 overwrites the older slot (copy0) with seq3 -> copy0 now newest.
   * Exercises the tie-break the other way (rec0.seq >= rec1.seq true). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_devcfg_commit(&s_mock, &r0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_devcfg_load(&s_mock));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_devcfg_get_vcom_mv(&mv));
  TEST_ASSERT_EQ(k_dc_good_mv, mv);

  /* Vector 2 -- copy0 blank, copy1 valid. */
  internal_dc_mock_blank();
  (void)ra8_devcfg_commit(&s_mock, &r0); /* copy0 */
  (void)ra8_devcfg_commit(&s_mock, &r1); /* copy1 */
  (void)memset(&s_mem[k_ra8_devcfg_copy0_off],
               (int)k_dc_blank_byte,
               (size_t)k_ra8_devcfg_slot_bytes);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_devcfg_load(&s_mock));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_devcfg_get_vcom_mv(&mv));
  TEST_ASSERT_EQ(k_dc_other_mv, mv);

  /* Vector 3 -- copy0 valid, copy1 blank. */
  internal_dc_mock_blank();
  (void)ra8_devcfg_commit(&s_mock, &r0); /* copy0 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_devcfg_load(&s_mock));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_devcfg_get_vcom_mv(&mv));
  TEST_ASSERT_EQ(k_dc_good_mv, mv);

  /* Vector 4 -- neither valid. */
  internal_dc_mock_blank();
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_devcfg_load(&s_mock));
  TEST_END("devcfg MC/DC: two-copy resolution");
}

/**
 * @test internal_dc_torn_write_recovery -- a header-write failure keeps the last good copy.

 * @brief Fails the second copy header write after its body lands and proves the prior complete copy remains the sole survivor.
 *
 * @details Fails the second copy header write after its body lands and proves the prior complete copy remains the sole survivor.
 * @pre The RAM medium and configuration singleton are exclusively owned by this test vector.
 * @pre Every supplied range lies within its documented mock-store or record capacity.
 * @post Every write remains within the fixed RAM medium and caller-owned output storage.
 * @post No heap allocation, host stream, or physical nonvolatile medium is accessed.
 * @note Test-only and not reentrant because the medium and driver state have file scope.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_dc_torn_write_recovery(void)
{
  TEST_BEGIN("devcfg: torn-write recovery");
  const ra8_devcfg_record_t good =
    internal_dc_make_rec((uint16_t)k_dc_good_mv, (uint32_t)k_ra8_devcfg_flag_vcom_valid);
  const ra8_devcfg_record_t next =
    internal_dc_make_rec((uint16_t)k_dc_other_mv, (uint32_t)k_ra8_devcfg_flag_vcom_valid);

  /* Seed copy0 as the last-good record. */
  internal_dc_mock_blank();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_devcfg_commit(&s_mock, &good)); /* copy0, seq1 */

  /* Next commit targets copy1; allow the body write, fail the header write. */
  s_write_budget = 1;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_devcfg_commit(&s_mock, &next));

  /* copy1's header never landed -> it is invalid; copy0 still resolves. */
  s_write_budget = -1;
  uint16_t mv    = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_devcfg_load(&s_mock));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_devcfg_get_vcom_mv(&mv));
  TEST_ASSERT_EQ(k_dc_good_mv, mv);
  TEST_END("devcfg: torn-write recovery");
}

/**
 * @test internal_dc_commit_body_write_fault -- a body-write failure is propagated.

 * @brief Fails the first body write and verifies commit propagates the fault without creating a loadable record.
 *
 * @details Fails the first body write and verifies commit propagates the fault without creating a loadable record.
 * @pre The RAM medium and configuration singleton are exclusively owned by this test vector.
 * @pre Every supplied range lies within its documented mock-store or record capacity.
 * @post Every write remains within the fixed RAM medium and caller-owned output storage.
 * @post No heap allocation, host stream, or physical nonvolatile medium is accessed.
 * @note Test-only and not reentrant because the medium and driver state have file scope.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_dc_commit_body_write_fault(void)
{
  TEST_BEGIN("devcfg: body write fault propagates");
  const ra8_devcfg_record_t rec =
    internal_dc_make_rec((uint16_t)k_dc_good_mv, (uint32_t)k_ra8_devcfg_flag_vcom_valid);
  internal_dc_mock_blank();
  s_write_budget = 0; /* fail the very first (body) write */
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_devcfg_commit(&s_mock, &rec));
  s_write_budget = -1;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_devcfg_load(&s_mock));
  TEST_END("devcfg: body write fault propagates");
}

/**
 * @test internal_dc_vcom_gate_mcdc -- the VCOM validity flag and plausible-range window.
 *
 * @par MC/DC:
 * Decision (reject VCOM): `(mv < vcom_min) || (mv > vcom_max)` (2 conditions).
 * - Vector 1: mv=1530 -> min<=mv<=max: both false -> in range (accept).
 * - Vector 2: mv=100  -> mv<min true,  mv>max false -> reject (varies cond 1).
 * - Vector 3: mv=5000 -> mv<min false, mv>max true  -> reject (varies cond 2).
 * Vectors 1+2 prove the low bound, 1+3 the high bound, each independently
 * affects the outcome. The separate valid-flag guard is checked both ways, and
 * the two poison values 0 and 0xFFFF are rejected by the range.

 * @brief Covers initialization, validity-flag, lower-bound, upper-bound, and poison-value checks for published panel VCOM.
 *
 * @details Covers initialization, validity-flag, lower-bound, upper-bound, and poison-value checks for published panel VCOM.
 * @pre The RAM medium and configuration singleton are exclusively owned by this test vector.
 * @pre Every supplied range lies within its documented mock-store or record capacity.
 * @post Every write remains within the fixed RAM medium and caller-owned output storage.
 * @post No heap allocation, host stream, or physical nonvolatile medium is accessed.
 * @note Test-only and not reentrant because the medium and driver state have file scope.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_dc_vcom_gate_mcdc(void)
{
  TEST_BEGIN("devcfg MC/DC: VCOM validity + range");
  uint16_t mv = 0U;

  /* not-initialised guard. */
  ra8_devcfg_reset();
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_devcfg_get_vcom_mv(&mv));

  /* Vector 1 -- valid flag set, value in range. */
  internal_dc_mock_blank();
  ra8_devcfg_record_t rec =
    internal_dc_make_rec((uint16_t)k_dc_good_mv, (uint32_t)k_ra8_devcfg_flag_vcom_valid);
  (void)ra8_devcfg_commit(&s_mock, &rec);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_devcfg_load(&s_mock));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_devcfg_get_vcom_mv(&mv));
  TEST_ASSERT_EQ(k_dc_good_mv, mv);

  /* valid flag CLEAR -> refused even though the value is fine. */
  internal_dc_mock_blank();
  rec = internal_dc_make_rec((uint16_t)k_dc_good_mv, (uint32_t)k_ra8_devcfg_flag_provisioned);
  (void)ra8_devcfg_commit(&s_mock, &rec);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_devcfg_load(&s_mock));
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_devcfg_get_vcom_mv(&mv));

  /* Vector 2 -- below the window. */
  internal_dc_mock_blank();
  rec = internal_dc_make_rec((uint16_t)k_dc_low_mv, (uint32_t)k_ra8_devcfg_flag_vcom_valid);
  (void)ra8_devcfg_commit(&s_mock, &rec);
  (void)ra8_devcfg_load(&s_mock);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_devcfg_get_vcom_mv(&mv));

  /* Vector 3 -- above the window. */
  internal_dc_mock_blank();
  rec = internal_dc_make_rec((uint16_t)k_dc_high_mv, (uint32_t)k_ra8_devcfg_flag_vcom_valid);
  (void)ra8_devcfg_commit(&s_mock, &rec);
  (void)ra8_devcfg_load(&s_mock);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_devcfg_get_vcom_mv(&mv));

  /* poison values 0 and 0xFFFF. */
  internal_dc_mock_blank();
  rec = internal_dc_make_rec((uint16_t)k_dc_zero_mv, (uint32_t)k_ra8_devcfg_flag_vcom_valid);
  (void)ra8_devcfg_commit(&s_mock, &rec);
  (void)ra8_devcfg_load(&s_mock);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_devcfg_get_vcom_mv(&mv));
  internal_dc_mock_blank();
  rec = internal_dc_make_rec((uint16_t)k_dc_blank_mv, (uint32_t)k_ra8_devcfg_flag_vcom_valid);
  (void)ra8_devcfg_commit(&s_mock, &rec);
  (void)ra8_devcfg_load(&s_mock);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_devcfg_get_vcom_mv(&mv));
  TEST_END("devcfg MC/DC: VCOM validity + range");
}

/**
 * @test internal_dc_null_arg_guards -- every public entry rejects NULL arguments.

 * @brief Drives every public null callback, record, store, and output guard plus pre-load access rejection.
 *
 * @details Drives every public null callback, record, store, and output guard plus pre-load access rejection.
 * @pre The RAM medium and configuration singleton are exclusively owned by this test vector.
 * @pre Every supplied range lies within its documented mock-store or record capacity.
 * @post Every write remains within the fixed RAM medium and caller-owned output storage.
 * @post No heap allocation, host stream, or physical nonvolatile medium is accessed.
 * @note Test-only and not reentrant because the medium and driver state have file scope.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_dc_null_arg_guards(void)
{
  TEST_BEGIN("devcfg: null-argument guards");
  const ra8_devcfg_store_t  no_read  = {.read = nullptr, .write = internal_dc_mock_write};
  const ra8_devcfg_store_t  no_write = {.read = internal_dc_mock_read, .write = nullptr};
  const ra8_devcfg_record_t rec =
    internal_dc_make_rec((uint16_t)k_dc_good_mv, (uint32_t)k_ra8_devcfg_flag_vcom_valid);
  const ra8_devcfg_body_t* body = nullptr;

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_devcfg_load(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_devcfg_load(&no_read));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_devcfg_get_vcom_mv(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_devcfg_get_body(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_devcfg_commit(nullptr, &rec));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_devcfg_commit(&no_read, &rec));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_devcfg_commit(&no_write, &rec));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_devcfg_commit(&s_mock, nullptr));

  /* get_body before a successful load. */
  ra8_devcfg_reset();
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_devcfg_get_body(&body));
  TEST_END("devcfg: null-argument guards");
}

/**
 * @test internal_dc_default_store_backend -- the extra-MRAM store via its RAM shadow.

 * @brief Exercises the production default-store RAM shadow for blank detection, persistence, range guards, and null buffers.
 *
 * @details Exercises the production default-store RAM shadow for blank detection, persistence, range guards, and null buffers.
 * @pre The RAM medium and configuration singleton are exclusively owned by this test vector.
 * @pre Every supplied range lies within its documented mock-store or record capacity.
 * @post Every write remains within the fixed RAM medium and caller-owned output storage.
 * @post No heap allocation, host stream, or physical nonvolatile medium is accessed.
 * @note Test-only and not reentrant because the medium and driver state have file scope.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_dc_default_store_backend(void)
{
  TEST_BEGIN("devcfg: extra-MRAM default store");
  const ra8_devcfg_store_t* ds = ra8_devcfg_default_store();
  TEST_ASSERT_NOT_NULL(ds);
  TEST_ASSERT_NOT_NULL(ds->read);
  TEST_ASSERT_NOT_NULL(ds->write);

  /* Blank shadow -> UNPROVISIONED. */
  ra8_devcfg_reset();
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_devcfg_load(ds));
  TEST_ASSERT(ra8_devcfg_is_blank());

  /* Commit then load persists through the shadow. */
  const ra8_devcfg_record_t rec =
    internal_dc_make_rec((uint16_t)k_dc_good_mv, (uint32_t)k_ra8_devcfg_flag_vcom_valid);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_devcfg_commit(ds, &rec));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_devcfg_load(ds));
  uint16_t mv = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_devcfg_get_vcom_mv(&mv));
  TEST_ASSERT_EQ(k_dc_good_mv, mv);

  /* Backend edge branches: out-of-range and NULL are refused. */
  uint8_t        scratch[k_dc_scratch_len] = {};
  const uint32_t past = (uint32_t)k_ra8_devcfg_copy1_off + (uint32_t)k_ra8_devcfg_slot_bytes;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ds->read(past, scratch, (uint32_t)k_dc_scratch_len));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ds->write(past, scratch, (uint32_t)k_dc_scratch_len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ds->read(0U, nullptr, (uint32_t)k_dc_scratch_len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ds->write(0U, nullptr, (uint32_t)k_dc_scratch_len));
  TEST_END("devcfg: extra-MRAM default store");
}

int main(void)
{
  internal_dc_commit_load_roundtrip();
  internal_dc_validation_chain_mcdc();
  internal_dc_resolution_mcdc();
  internal_dc_torn_write_recovery();
  internal_dc_commit_body_write_fault();
  internal_dc_vcom_gate_mcdc();
  internal_dc_null_arg_guards();
  internal_dc_default_store_backend();
  return 0;
}
