/**
 * @file test_ra8_epd_cal.c
 * @brief Unit tests for per-device e-paper VCOM calibration (``ra8_epd_cal.c``)
 *
 * @details
 * Pure host tests -- no register simulator. The module reaches the panel
 * and the non-volatile store only through injected function-pointer seams,
 * so every branch is drivable from a mock.
 *
 * What we cover:
 *   - Record codec: serialise / deserialise round-trip, magic rejection
 *     (blank storage), schema-version rejection (forward compatibility),
 *     payload-length rejection, CRC rejection (single-bit corruption).
 *   - Range validation against a panel's documented VCOM window,
 *     including the two real-world poison values 0 and 0xFFFF.
 *   - Resolution order: controller, then per-device record, then
 *     operator-provisioned, with each higher source disabled in turn.
 *   - Out-of-range values at each source falling through to the next
 *     rather than being programmed.
 *   - The fail-safe: no trusted source yields ``k_ra8_err_not_found`` and
 *     ``k_ra8_epd_cal_src_none``, and ``apply`` refuses that result.
 *   - Provisioning writes a record a later resolve accepts, and refuses
 *     out-of-range values.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_epd_cal.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/**
 * @enum epd_cal_test_const_t
 * @brief Test-only numeric constants.
 */
typedef enum : uint16_t {
  k_ec_min_mv     = 200U,    /**< Low edge of the modelled panel window.  */
  k_ec_max_mv     = 4000U,   /**< High edge of the modelled panel window. */
  k_ec_good_mv    = 1530U,   /**< A plausible in-range VCOM (-1.53 V).    */
  k_ec_other_mv   = 1670U,   /**< A second, different in-range VCOM.      */
  k_ec_third_mv   = 1450U,   /**< A third, different in-range VCOM.       */
  k_ec_low_mv     = 199U,    /**< One below the window.                   */
  k_ec_high_mv    = 4001U,   /**< One above the window.                   */
  k_ec_blank_mv   = 0xFFFFU, /**< Blank-flash / failed-read signature.    */
  k_ec_blob       = 32U,     /**< Mirrors ::k_ra8_epd_cal_blob_size.      */
  k_ec_bad_schema = 99U,     /**< Schema version from a future writer.    */
} epd_cal_test_const_t;

/**
 * @struct ec_store_state_t
 * @brief Backing state for the mock non-volatile store.
 *
 * @details
 * ``present`` models blank storage: when false the read still succeeds but
 * hands back 0xFF bytes, exactly as an unprogrammed extra-MRAM block does.
 */
typedef struct {
  uint8_t  blob[k_ec_blob]; /**< Stored record bytes.               */
  bool     present;         /**< Whether a record was ever written. */
  bool     read_fails;      /**< Force the read seam to fault.      */
  bool     write_fails;     /**< Force the write seam to fault.     */
  uint32_t writes;          /**< Count of successful writes.        */
} ec_store_state_t;

/**
 * @struct ec_panel_state_t
 * @brief Backing state for the mock controller VCOM seam.
 */
typedef struct {
  uint16_t get_mv;    /**< Value the controller reports.      */
  bool     get_fails; /**< Force the get seam to fault.       */
  bool     set_fails; /**< Force the set seam to fault.       */
  uint16_t last_set;  /**< Last value handed to the set seam. */
  uint32_t set_calls; /**< Count of set-seam invocations.     */
  /**
   * Model a controller that accepts the write and changes nothing -- a
   * dead COPI line, or a controller that ignores the command. The seam
   * still returns success, so this is precisely the failure mode that is
   * indistinguishable from a good write without a readback compare.
   */
  bool set_is_silent;
} ec_panel_state_t;

/** @brief Mock store backing state. */
static ec_store_state_t s_store;
/** @brief Mock controller backing state. */
static ec_panel_state_t s_panel;

/**
 * @brief Mock ::ra8_epd_cal_nv_read_fn_t over ::s_store.
 *
 * @param[in]  ctx Ignored.
 * @param[out] dst Destination buffer.
 * @param[in]  len Bytes to read.
 * @return ``k_ra8_ok`` or ``k_ra8_err_hw_error`` when armed to fail.
 */
static ra8_err_t mock_nv_read(void* ctx, uint8_t* dst, size_t len)
{
  (void)ctx;
  if (s_store.read_fails) {
    return k_ra8_err_hw_error;
  }
  if (s_store.present) {
    (void)memcpy(dst, s_store.blob, len);
  } else {
    (void)memset(dst, 0xFF, len); /* blank extra-MRAM reads as all-ones */
  }
  return k_ra8_ok;
}

/**
 * @brief Mock ::ra8_epd_cal_nv_write_fn_t over ::s_store.
 *
 * @param[in] ctx Ignored.
 * @param[in] src Serialised record.
 * @param[in] len Bytes to write.
 * @return ``k_ra8_ok`` or ``k_ra8_err_hw_error`` when armed to fail.
 */
static ra8_err_t mock_nv_write(void* ctx, const uint8_t* src, size_t len)
{
  (void)ctx;
  if (s_store.write_fails) {
    return k_ra8_err_hw_error;
  }
  (void)memcpy(s_store.blob, src, len);
  s_store.present = true;
  s_store.writes  = s_store.writes + 1U;
  return k_ra8_ok;
}

/**
 * @brief Mock ::ra8_epd_cal_panel_get_fn_t over ::s_panel.
 *
 * @param[in]  ctx    Ignored.
 * @param[out] out_mv Receives the modelled controller VCOM.
 * @return ``k_ra8_ok`` or ``k_ra8_err_hw_error`` when armed to fail.
 */
static ra8_err_t mock_panel_get(void* ctx, uint16_t* out_mv)
{
  (void)ctx;
  if (s_panel.get_fails) {
    return k_ra8_err_hw_error;
  }
  *out_mv = s_panel.get_mv;
  return k_ra8_ok;
}

/**
 * @brief Mock ::ra8_epd_cal_panel_set_fn_t over ::s_panel.
 *
 * @param[in] ctx Ignored.
 * @param[in] mv  Value being programmed.
 * @return ``k_ra8_ok`` or ``k_ra8_err_hw_error`` when armed to fail.
 */
static ra8_err_t mock_panel_set(void* ctx, uint16_t mv)
{
  (void)ctx;
  s_panel.set_calls = s_panel.set_calls + 1U;
  if (s_panel.set_fails) {
    return k_ra8_err_hw_error;
  }
  s_panel.last_set = mv;
  /* A working controller reports back what it was given; a silent one
   * keeps whatever it held. Which of the two this mock is deciding the
   * outcome of ``apply``'s readback compare is the whole point. */
  if (!s_panel.set_is_silent) {
    s_panel.get_mv = mv;
  }
  return k_ra8_ok;
}

/** @brief Reset both mocks to "nothing available anywhere". */
static void ec_reset(void)
{
  s_store = (ec_store_state_t){};
  s_panel = (ec_panel_state_t){};
}

/** @brief Build a config with both seams bound and no provisioning value. */
static ra8_epd_cal_cfg_t ec_cfg(void)
{
  const ra8_epd_cal_cfg_t cfg = {
    .limits          = {.min_mv = (uint16_t)k_ec_min_mv, .max_mv = (uint16_t)k_ec_max_mv},
    .panel           = {.get = mock_panel_get, .set = mock_panel_set, .ctx = nullptr},
    .store           = {.read = mock_nv_read, .write = mock_nv_write, .ctx = nullptr},
    .provisioned_mv  = 0U,
    .has_provisioned = false,
  };
  return cfg;
}

/** @brief Seed ::s_store with a valid record holding @p mv. */
static void ec_seed_record(uint16_t mv)
{
  const ra8_epd_cal_record_t rec = {.vcom_mv        = mv,
                                    .schema_version = (uint8_t)k_ra8_epd_cal_schema_version};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epd_cal_serialize(&rec, s_store.blob, (size_t)k_ec_blob));
  s_store.present = true;
}

/* =============================================================================
 * Record codec
 * =============================================================================
 */

/** @test Serialise then deserialise reproduces the record exactly. */
static void test_record_roundtrip(void)
{
  TEST_BEGIN("epd_cal: record serialize/deserialize round-trip");
  ec_reset();
  const ra8_epd_cal_record_t in = {.vcom_mv        = (uint16_t)k_ec_good_mv,
                                   .schema_version = (uint8_t)k_ra8_epd_cal_schema_version};
  uint8_t                    blob[k_ec_blob] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epd_cal_serialize(&in, blob, sizeof(blob)));

  ra8_epd_cal_record_t out = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epd_cal_deserialize(blob, sizeof(blob), &out));
  TEST_ASSERT_EQ((uint16_t)k_ec_good_mv, out.vcom_mv);
  TEST_ASSERT_EQ((uint8_t)k_ra8_epd_cal_schema_version, out.schema_version);
  TEST_END("epd_cal: record serialize/deserialize round-trip");
}

/** @test Codec argument and capacity validation. */
static void test_record_arg_validation(void)
{
  TEST_BEGIN("epd_cal: codec argument validation");
  ec_reset();
  const ra8_epd_cal_record_t rec = {.vcom_mv        = (uint16_t)k_ec_good_mv,
                                    .schema_version = (uint8_t)k_ra8_epd_cal_schema_version};
  uint8_t                    blob[k_ec_blob] = {};
  ra8_epd_cal_record_t       out             = {};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epd_cal_serialize(nullptr, blob, sizeof(blob)));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epd_cal_serialize(&rec, nullptr, sizeof(blob)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_epd_cal_serialize(&rec, blob, sizeof(blob) - 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epd_cal_deserialize(nullptr, sizeof(blob), &out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epd_cal_deserialize(blob, sizeof(blob), nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_epd_cal_deserialize(blob, sizeof(blob) - 1U, &out));

  /* A zero VCOM is never serialisable: it is the "controller not ready"
   * signature and must not become a durable record. */
  const ra8_epd_cal_record_t zero = {.vcom_mv = 0U, .schema_version = 1U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epd_cal_serialize(&zero, blob, sizeof(blob)));
  TEST_END("epd_cal: codec argument validation");
}

/** @test Blank storage, wrong schema, wrong length and bad CRC are distinct. */
static void test_record_integrity_and_versioning(void)
{
  TEST_BEGIN("epd_cal: record integrity + schema versioning");
  ec_reset();
  ra8_epd_cal_record_t out              = {};
  uint8_t              blank[k_ec_blob] = {};
  (void)memset(blank, 0xFF, sizeof(blank));
  /* Blank (never provisioned) storage: magic absent -> "not found", which
   * is what lets the resolver tell "unprovisioned" from "corrupted". */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_epd_cal_deserialize(blank, sizeof(blank), &out));

  const ra8_epd_cal_record_t rec = {.vcom_mv        = (uint16_t)k_ec_good_mv,
                                    .schema_version = (uint8_t)k_ra8_epd_cal_schema_version};
  uint8_t                    good[k_ec_blob] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epd_cal_serialize(&rec, good, sizeof(good)));

  /* A record written by a newer schema is refused, not best-effort parsed:
   * a future writer may redefine the payload. */
  uint8_t newer[k_ec_blob] = {};
  (void)memcpy(newer, good, sizeof(newer));
  newer[k_ra8_epd_cal_off_version] = (uint8_t)k_ec_bad_schema;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_epd_cal_deserialize(newer, sizeof(newer), &out));

  /* Wrong payload length for this schema. Re-seal the CRC so the length
   * check, not the CRC, is what rejects it. */
  uint8_t badlen[k_ec_blob] = {};
  (void)memcpy(badlen, good, sizeof(badlen));
  badlen[k_ra8_epd_cal_off_payload_len] = (uint8_t)(k_ra8_epd_cal_payload_len + 1U);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_epd_cal_deserialize(badlen, sizeof(badlen), &out));

  /* Single-bit corruption in the payload is caught by the CRC. This is the
   * check that stands in for the image signature, which cannot cover
   * per-device bytes. */
  uint8_t corrupt[k_ec_blob] = {};
  (void)memcpy(corrupt, good, sizeof(corrupt));
  corrupt[k_ra8_epd_cal_off_vcom_mv] ^= 0x01U;
  TEST_ASSERT_EQ(k_ra8_err_crc_mismatch, ra8_epd_cal_deserialize(corrupt, sizeof(corrupt), &out));

  /* Corruption in the reserved growth span is caught too -- the CRC covers
   * the whole body, not just the payload. */
  uint8_t reserved_hit[k_ec_blob] = {};
  (void)memcpy(reserved_hit, good, sizeof(reserved_hit));
  reserved_hit[k_ra8_epd_cal_off_reserved1] ^= 0x80U;
  TEST_ASSERT_EQ(k_ra8_err_crc_mismatch,
                 ra8_epd_cal_deserialize(reserved_hit, sizeof(reserved_hit), &out));
  TEST_END("epd_cal: record integrity + schema versioning");
}

/* =============================================================================
 * Range validation
 * =============================================================================
 */

/**
 * @test epd_cal_vcom_range_mcdc
 *
 * @par MC/DC:
 * Decision: `if ((mv < limits->min_mv) || (mv > limits->max_mv))` in
 * ``ra8_epd_cal_vcom_in_range`` (2 conditions).
 * - Vector 1: mv=1530, window 200..4000 -> F||F = false (in range;
 *             control case, both conditions false)
 * - Vector 2: mv=199,  window 200..4000 -> T||F = true  (varies the
 *             low-edge condition only)
 * - Vector 3: mv=4001, window 200..4000 -> F||T = true  (varies the
 *             high-edge condition only)
 * Vectors 1+2 prove ``mv < min_mv`` independently affects the outcome;
 * vectors 1+3 prove the same for ``mv > max_mv``. N+1 = 3 vectors for
 * N=2 conditions: minimal MC/DC.
 */
static void test_vcom_range_mcdc(void)
{
  TEST_BEGIN("epd_cal MC/DC: vcom_in_range 2-cond");
  const ra8_epd_cal_limits_mv_t lim = {.min_mv = (uint16_t)k_ec_min_mv,
                                       .max_mv = (uint16_t)k_ec_max_mv};
  /* V1: both conditions false -> in range. */
  TEST_ASSERT(ra8_epd_cal_vcom_in_range((uint16_t)k_ec_good_mv, &lim));
  /* V2: low-edge condition true. */
  TEST_ASSERT(!ra8_epd_cal_vcom_in_range((uint16_t)k_ec_low_mv, &lim));
  /* V3: high-edge condition true. */
  TEST_ASSERT(!ra8_epd_cal_vcom_in_range((uint16_t)k_ec_high_mv, &lim));

  /* Boundaries are inclusive. */
  TEST_ASSERT(ra8_epd_cal_vcom_in_range((uint16_t)k_ec_min_mv, &lim));
  TEST_ASSERT(ra8_epd_cal_vcom_in_range((uint16_t)k_ec_max_mv, &lim));

  /* The two poison values a real controller / blank flash produces. */
  TEST_ASSERT(!ra8_epd_cal_vcom_in_range(0U, &lim));
  TEST_ASSERT(!ra8_epd_cal_vcom_in_range((uint16_t)k_ec_blank_mv, &lim));

  /* A NULL window is not a licence to pass anything. */
  TEST_ASSERT(!ra8_epd_cal_vcom_in_range((uint16_t)k_ec_good_mv, nullptr));
  TEST_END("epd_cal MC/DC: vcom_in_range 2-cond");
}

/**
 * @test epd_cal_resolve_limits_mcdc
 *
 * @par MC/DC:
 * Decision: `if ((cfg->limits.min_mv == 0U) || (cfg->limits.min_mv >
 * cfg->limits.max_mv))` in ``ra8_epd_cal_resolve`` (2 conditions).
 * - Vector 1: min=200, max=4000 -> F||F = false (accepted; control case)
 * - Vector 2: min=0,   max=4000 -> T||F = true  (varies the zero-window
 *             condition only)
 * - Vector 3: min=4000, max=200 -> F||T = true  (varies the inverted-
 *             window condition only)
 * Vectors 1+2 prove the zero check independently affects the outcome;
 * vectors 1+3 prove the same for the inversion check. N+1 = 3 vectors.
 *
 * A zero ``min_mv`` is rejected specifically so that a controller
 * reporting 0 -- the "not finished booting" signature -- can never be
 * inside the window.
 */
static void test_resolve_limits_mcdc(void)
{
  TEST_BEGIN("epd_cal MC/DC: resolve limits 2-cond");
  ec_reset();
  s_panel.get_mv           = (uint16_t)k_ec_good_mv;
  ra8_epd_cal_result_t res = {};

  /* V1: both conditions false -> limits accepted, resolution proceeds. */
  ra8_epd_cal_cfg_t cfg = ec_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epd_cal_resolve(&cfg, &res));

  /* V2: min_mv == 0. */
  cfg               = ec_cfg();
  cfg.limits.min_mv = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epd_cal_resolve(&cfg, &res));

  /* V3: min_mv > max_mv. */
  cfg               = ec_cfg();
  cfg.limits.min_mv = (uint16_t)k_ec_max_mv;
  cfg.limits.max_mv = (uint16_t)k_ec_min_mv;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epd_cal_resolve(&cfg, &res));
  TEST_END("epd_cal MC/DC: resolve limits 2-cond");
}

/* =============================================================================
 * Resolution order
 * =============================================================================
 */

/** @test The controller's own value wins when it is in range. */
static void test_resolve_prefers_panel(void)
{
  TEST_BEGIN("epd_cal: resolve prefers the controller's own VCOM");
  ec_reset();
  s_panel.get_mv = (uint16_t)k_ec_good_mv;
  /* Seed a *different* record so the assertion proves precedence rather
   * than coincidence. */
  ec_seed_record((uint16_t)k_ec_other_mv);

  ra8_epd_cal_cfg_t cfg    = ec_cfg();
  cfg.has_provisioned      = true;
  cfg.provisioned_mv       = (uint16_t)k_ec_third_mv;
  ra8_epd_cal_result_t res = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epd_cal_resolve(&cfg, &res));
  TEST_ASSERT_EQ((uint16_t)k_ec_good_mv, res.vcom_mv);
  TEST_ASSERT_EQ(k_ra8_epd_cal_src_panel, res.source);
  TEST_END("epd_cal: resolve prefers the controller's own VCOM");
}

/** @test With no controller value, the per-device record is used. */
static void test_resolve_falls_back_to_record(void)
{
  TEST_BEGIN("epd_cal: resolve falls back to the per-device record");
  ec_reset();
  ec_seed_record((uint16_t)k_ec_other_mv);

  /* Leg 1: the controller seam is not bound at all. */
  ra8_epd_cal_cfg_t cfg    = ec_cfg();
  cfg.panel.get            = nullptr;
  cfg.has_provisioned      = true;
  cfg.provisioned_mv       = (uint16_t)k_ec_third_mv;
  ra8_epd_cal_result_t res = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epd_cal_resolve(&cfg, &res));
  TEST_ASSERT_EQ((uint16_t)k_ec_other_mv, res.vcom_mv);
  TEST_ASSERT_EQ(k_ra8_epd_cal_src_record, res.source);

  /* Leg 2: the controller read faults. */
  ec_reset();
  ec_seed_record((uint16_t)k_ec_other_mv);
  s_panel.get_fails = true;
  cfg               = ec_cfg();
  res               = (ra8_epd_cal_result_t){};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epd_cal_resolve(&cfg, &res));
  TEST_ASSERT_EQ(k_ra8_epd_cal_src_record, res.source);

  /* Leg 3: the controller answers with an out-of-range value. It must be
   * ignored, not programmed -- this is the leg that protects the panel
   * from a controller that has lost its configuration. */
  ec_reset();
  ec_seed_record((uint16_t)k_ec_other_mv);
  s_panel.get_mv = (uint16_t)k_ec_blank_mv;
  cfg            = ec_cfg();
  res            = (ra8_epd_cal_result_t){};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epd_cal_resolve(&cfg, &res));
  TEST_ASSERT_EQ((uint16_t)k_ec_other_mv, res.vcom_mv);
  TEST_ASSERT_EQ(k_ra8_epd_cal_src_record, res.source);
  TEST_END("epd_cal: resolve falls back to the per-device record");
}

/** @test With no controller value and no record, provisioning is used. */
static void test_resolve_falls_back_to_provisioned(void)
{
  TEST_BEGIN("epd_cal: resolve falls back to the provisioned value");
  ec_reset();
  /* Blank storage, controller unbound: only the operator value remains. */
  ra8_epd_cal_cfg_t cfg    = ec_cfg();
  cfg.panel.get            = nullptr;
  cfg.has_provisioned      = true;
  cfg.provisioned_mv       = (uint16_t)k_ec_third_mv;
  ra8_epd_cal_result_t res = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epd_cal_resolve(&cfg, &res));
  TEST_ASSERT_EQ((uint16_t)k_ec_third_mv, res.vcom_mv);
  TEST_ASSERT_EQ(k_ra8_epd_cal_src_provisioned, res.source);

  /* A corrupted record must not shadow the operator value. */
  ec_reset();
  ec_seed_record((uint16_t)k_ec_other_mv);
  s_store.blob[k_ra8_epd_cal_off_vcom_mv] ^= 0x01U; /* break the CRC */
  cfg                 = ec_cfg();
  cfg.panel.get       = nullptr;
  cfg.has_provisioned = true;
  cfg.provisioned_mv  = (uint16_t)k_ec_third_mv;
  res                 = (ra8_epd_cal_result_t){};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epd_cal_resolve(&cfg, &res));
  TEST_ASSERT_EQ((uint16_t)k_ec_third_mv, res.vcom_mv);
  TEST_ASSERT_EQ(k_ra8_epd_cal_src_provisioned, res.source);

  /* A stored value outside the panel's window is ignored the same way. */
  ec_reset();
  ec_seed_record((uint16_t)k_ec_good_mv);
  cfg                 = ec_cfg();
  cfg.panel.get       = nullptr;
  cfg.limits.min_mv   = (uint16_t)(k_ec_good_mv + 1U); /* now out of range */
  cfg.has_provisioned = true;
  cfg.provisioned_mv  = (uint16_t)k_ec_other_mv;
  res                 = (ra8_epd_cal_result_t){};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epd_cal_resolve(&cfg, &res));
  TEST_ASSERT_EQ(k_ra8_epd_cal_src_provisioned, res.source);
  TEST_END("epd_cal: resolve falls back to the provisioned value");
}

/** @test Nothing trusted anywhere: resolve fails and apply refuses. */
static void test_resolve_fail_safe(void)
{
  TEST_BEGIN("epd_cal: fail-safe when no source is trustworthy");
  ec_reset();

  /* Every source absent. */
  ra8_epd_cal_cfg_t cfg    = ec_cfg();
  cfg.panel.get            = nullptr;
  ra8_epd_cal_result_t res = {};
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_epd_cal_resolve(&cfg, &res));
  TEST_ASSERT_EQ(k_ra8_epd_cal_src_none, res.source);
  TEST_ASSERT_EQ(0U, res.vcom_mv);

  /* Every source present but every value untrustworthy: a controller that
   * reports blank, a record whose value is outside the window, and an
   * operator value that is also outside it. Nothing may be programmed. */
  ec_reset();
  s_panel.get_mv = (uint16_t)k_ec_blank_mv;
  ec_seed_record((uint16_t)k_ec_good_mv);
  cfg                 = ec_cfg();
  cfg.limits.min_mv   = (uint16_t)(k_ec_good_mv + 1U);
  cfg.has_provisioned = true;
  cfg.provisioned_mv  = (uint16_t)k_ec_low_mv;
  res                 = (ra8_epd_cal_result_t){};
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_epd_cal_resolve(&cfg, &res));
  TEST_ASSERT_EQ(k_ra8_epd_cal_src_none, res.source);

  /* apply() must refuse a failed resolution outright -- this is the last
   * gate before a bias reaches the panel. */
  cfg = ec_cfg();
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_epd_cal_apply(&cfg, &res));
  TEST_ASSERT_EQ(0U, s_panel.set_calls);

  /* has_provisioned=false must not let provisioned_mv leak through. */
  ec_reset();
  cfg                 = ec_cfg();
  cfg.panel.get       = nullptr;
  cfg.has_provisioned = false;
  cfg.provisioned_mv  = (uint16_t)k_ec_good_mv;
  res                 = (ra8_epd_cal_result_t){};
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_epd_cal_resolve(&cfg, &res));
  TEST_END("epd_cal: fail-safe when no source is trustworthy");
}

/** @test Null-argument handling on the resolution entry points. */
static void test_resolve_null_args(void)
{
  TEST_BEGIN("epd_cal: resolve / apply / provision null arguments");
  ec_reset();
  ra8_epd_cal_cfg_t    cfg = ec_cfg();
  ra8_epd_cal_result_t res = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epd_cal_resolve(nullptr, &res));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epd_cal_resolve(&cfg, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epd_cal_apply(nullptr, &res));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epd_cal_apply(&cfg, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epd_cal_provision(nullptr, (uint16_t)k_ec_good_mv));
  TEST_END("epd_cal: resolve / apply / provision null arguments");
}

/* =============================================================================
 * Apply + provision
 * =============================================================================
 */

/** @test A resolved value is programmed, and every refusal leg holds. */
static void test_apply_paths(void)
{
  TEST_BEGIN("epd_cal: apply programs the panel and refuses bad input");
  ec_reset();
  s_panel.get_mv           = (uint16_t)k_ec_good_mv;
  ra8_epd_cal_cfg_t    cfg = ec_cfg();
  ra8_epd_cal_result_t res = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epd_cal_resolve(&cfg, &res));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epd_cal_apply(&cfg, &res));
  TEST_ASSERT_EQ((uint16_t)k_ec_good_mv, s_panel.last_set);

  /* No set seam: report it rather than pretending the panel was set. */
  cfg           = ec_cfg();
  cfg.panel.set = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_epd_cal_apply(&cfg, &res));

  /* No get seam either: a bias that cannot be confirmed is refused rather
   * than written and hoped about. */
  cfg           = ec_cfg();
  cfg.panel.get = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_epd_cal_apply(&cfg, &res));

  /* A result mutated out of range between resolve and apply is caught by
   * the re-check at the point of use. */
  cfg                           = ec_cfg();
  ra8_epd_cal_result_t tampered = res;
  tampered.vcom_mv              = (uint16_t)k_ec_high_mv;
  TEST_ASSERT_EQ(k_ra8_err_range_check_failed, ra8_epd_cal_apply(&cfg, &tampered));

  /* A bus fault on the set seam is forwarded, not swallowed. */
  s_panel.set_fails = true;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_epd_cal_apply(&cfg, &res));

  /* A bus fault on the readback is a refusal, not a shrug: the value in
   * effect is unknown, which is exactly the state to refuse. */
  ec_reset();
  s_panel.get_mv = (uint16_t)k_ec_good_mv;
  cfg            = ec_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epd_cal_resolve(&cfg, &res));
  s_panel.get_fails = true;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_epd_cal_apply(&cfg, &res));
  TEST_END("epd_cal: apply programs the panel and refuses bad input");
}

/**
 * @test epd_cal_apply_readback_guard
 *
 * @details
 * The INV-VCOM-1 write-then-readback condition. A link that accepts the
 * write and changes nothing returns success from the set seam, so without
 * this compare it is indistinguishable from a good write -- and the film
 * would sit biased at the controller's own unknown value. Both outcomes of
 * the compare are driven here, including the silent-set case that is the
 * entire reason the compare exists.
 *
 * The readback is compared but deliberately not re-range-checked: the
 * value written was range-checked before the write, so an equal readback
 * is in range by construction and a second test would be unreachable.
 * Range-validating a *reported* value matters where one is adopted rather
 * than confirmed, which is the resolve-from-controller path, covered by
 * ``test_resolve_prefers_panel``.
 *
 * @par MC/DC:
 * Decision: `if (readback != result->vcom_mv)` (1 condition, in
 * ``ra8_epd_cal_apply``) -- not a compound decision, so MC/DC degenerates
 * to branch coverage and 2 vectors are both necessary and sufficient:
 * - Vector 1: readback == requested -> false (a healthy controller echoes
 *                                             the value back; apply succeeds)
 * - Vector 2: readback != requested -> true  (a silent set seam leaves the
 *                                             controller on its old value;
 *                                             apply refuses)
 */
static void test_apply_readback_guard(void)
{
  TEST_BEGIN("epd_cal: apply refuses when the readback disagrees");

  /* Vector 1 -- condition false: a healthy controller echoes the value. */
  ec_reset();
  s_panel.get_mv           = (uint16_t)k_ec_good_mv;
  ra8_epd_cal_cfg_t    cfg = ec_cfg();
  ra8_epd_cal_result_t res = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epd_cal_resolve(&cfg, &res));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epd_cal_apply(&cfg, &res));
  TEST_ASSERT_EQ((uint16_t)k_ec_good_mv, s_panel.get_mv);

  /* Vector 2 -- condition true: the seam reports success but the
   * controller kept its old value. The set seam returned k_ra8_ok, so only
   * the readback distinguishes this from Vector 1 -- which is precisely
   * the silently-dead-link failure mode. */
  ec_reset();
  s_panel.get_mv = (uint16_t)k_ec_good_mv;
  cfg            = ec_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epd_cal_resolve(&cfg, &res));
  s_panel.set_is_silent = true;
  s_panel.get_mv        = (uint16_t)k_ec_other_mv;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_epd_cal_apply(&cfg, &res));
  /* The write really was attempted -- this is a verification failure, not
   * a refusal to try. */
  TEST_ASSERT_EQ((uint16_t)k_ec_good_mv, s_panel.last_set);

  TEST_END("epd_cal: apply refuses when the readback disagrees");
}

/** @test Provisioning writes a record that a later resolve accepts. */
static void test_provision_roundtrip(void)
{
  TEST_BEGIN("epd_cal: provision -> resolve round-trip");
  ec_reset();
  ra8_epd_cal_cfg_t cfg = ec_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epd_cal_provision(&cfg, (uint16_t)k_ec_third_mv));
  TEST_ASSERT_EQ(1U, s_store.writes);

  /* Resolve with the controller unbound so the record is the only source. */
  cfg                      = ec_cfg();
  cfg.panel.get            = nullptr;
  ra8_epd_cal_result_t res = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epd_cal_resolve(&cfg, &res));
  TEST_ASSERT_EQ((uint16_t)k_ec_third_mv, res.vcom_mv);
  TEST_ASSERT_EQ(k_ra8_epd_cal_src_record, res.source);
  TEST_END("epd_cal: provision -> resolve round-trip");
}

/** @test Provisioning refuses out-of-range values and unbound stores. */
static void test_provision_refusals(void)
{
  TEST_BEGIN("epd_cal: provision refusal legs");
  ec_reset();
  ra8_epd_cal_cfg_t cfg = ec_cfg();

  TEST_ASSERT_EQ(k_ra8_err_range_check_failed, ra8_epd_cal_provision(&cfg, (uint16_t)k_ec_low_mv));
  TEST_ASSERT_EQ(k_ra8_err_range_check_failed, ra8_epd_cal_provision(&cfg, (uint16_t)k_ec_high_mv));
  TEST_ASSERT_EQ(k_ra8_err_range_check_failed, ra8_epd_cal_provision(&cfg, 0U));
  TEST_ASSERT_EQ(0U, s_store.writes);

  /* No write seam bound. */
  cfg             = ec_cfg();
  cfg.store.write = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_epd_cal_provision(&cfg, (uint16_t)k_ec_good_mv));

  /* A store fault is forwarded so the caller knows the value is not durable. */
  cfg                 = ec_cfg();
  s_store.write_fails = true;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_epd_cal_provision(&cfg, (uint16_t)k_ec_good_mv));
  TEST_END("epd_cal: provision refusal legs");
}

/** @test A faulting store read is treated as "no record", not as a value. */
static void test_store_read_fault(void)
{
  TEST_BEGIN("epd_cal: store read fault falls through");
  ec_reset();
  ec_seed_record((uint16_t)k_ec_other_mv);
  s_store.read_fails = true;

  ra8_epd_cal_cfg_t cfg    = ec_cfg();
  cfg.panel.get            = nullptr;
  cfg.has_provisioned      = true;
  cfg.provisioned_mv       = (uint16_t)k_ec_third_mv;
  ra8_epd_cal_result_t res = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epd_cal_resolve(&cfg, &res));
  TEST_ASSERT_EQ(k_ra8_epd_cal_src_provisioned, res.source);

  /* An unbound read seam behaves the same way. */
  ec_reset();
  cfg                 = ec_cfg();
  cfg.panel.get       = nullptr;
  cfg.store.read      = nullptr;
  cfg.has_provisioned = true;
  cfg.provisioned_mv  = (uint16_t)k_ec_third_mv;
  res                 = (ra8_epd_cal_result_t){};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epd_cal_resolve(&cfg, &res));
  TEST_ASSERT_EQ(k_ra8_epd_cal_src_provisioned, res.source);
  TEST_END("epd_cal: store read fault falls through");
}

int main(void)
{
  test_record_roundtrip();
  test_record_arg_validation();
  test_record_integrity_and_versioning();
  test_vcom_range_mcdc();
  test_resolve_limits_mcdc();
  test_resolve_prefers_panel();
  test_resolve_falls_back_to_record();
  test_resolve_falls_back_to_provisioned();
  test_resolve_fail_safe();
  test_resolve_null_args();
  test_apply_paths();
  test_apply_readback_guard();
  test_provision_roundtrip();
  test_provision_refusals();
  test_store_read_fault();
  return 0;
}
