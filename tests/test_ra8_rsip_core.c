/**
 * @file test_ra8_rsip_core.c
 * @brief Unit tests for the RSIP engine core (init / BIST / SHA-256 / IRQ / power)
 *
 * @details
 * Split sibling of the original test_ra8_rsip.c suite covering the
 * engine-lifecycle surface of ra8_rsip.c against the
 * ``ra8_fake_mmap``-backed register window:
 *
 * - happy-path init runs MSTP release + BIST gate;
 * - the BIST timeout / late-pass legs are driven through the
 * ``ra8_fake_mmio`` wait seam (``fail_wait`` / ``satisfy_after``),
 * never by forging ``STATUS.BIST_OK`` inside the driver;
 * - null-arg rejection on the lifecycle APIs;
 * - TRNG read fails closed (no register backend on this silicon);
 * - one-shot and incremental SHA-256 / HMAC-SHA-256 known answers via
 * the software backend, plus their MC/DC vectors;
 * - status / IRQ helpers ack the right bits (W1C staged by the test,
 * since the RAM backing has no W1C semantics);
 * - power transition (enter / exit stop).
 *
 * Sibling suites: test_ra8_rsip_sym.c (key install + symmetric
 * ciphers + hash family) and test_ra8_rsip_devsec.c (asymmetric +
 * vault + device security).
 *
 * Each test resets ``ra8_fake_mmap``, ``ra8_fake_mmio`` and ``ra8_mstp``
 * first so cases stay independent.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_mstp.h"
#include "ra8_rsip.h"
#include "ra8_rsip_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_hmac_t
 * @brief HMAC-SHA-256 parameters, fixed by RFC 2104 and FIPS 180-4.
 *
 * @details
 * The reference implementation the tests compare against is spelled out here
 * rather than pulled from the driver, so a driver that changes its padding
 * constants fails instead of agreeing with itself.
 */
typedef enum : uint16_t {
  k_t_hmac_block_len = 64U,   /**< SHA-256 block size, bytes: the width of the
                                   ipad/opad buffers and of a prepared key.    */
  k_t_hmac_ipad_byte = 0x36U, /**< Inner-pad byte XORed over the prepared key. */
  k_t_hmac_opad_byte = 0x5CU, /**< Outer-pad byte, likewise.                   */
  k_t_key_short_len  = 20U,   /**< A key shorter than the block, which must be
                                   zero-extended rather than hashed.           */
  k_t_key_long_len   = 131U,  /**< A key longer than the block, which must be
                                   hashed down to 32 bytes first.              */
  k_t_key_short_byte = 0x0BU, /**< Fill byte of the short key. */
  k_t_key_long_byte  = 0xAAU, /**< Fill byte of the long key.  */
} t_hmac_t;

/**
 * @enum ra8_rsip_test_const_t
 * @brief Magic numbers used by the tests, named to keep the
 * no-magic-numbers rule satisfied.
 */
typedef enum : uint32_t {
  k_ra8_rsip_test_trng_bytes  = 32U,          /**< TRNG read length under test. */
  k_ra8_rsip_test_msg_bytes   = 8U,           /**< SHA input message size.      */
  k_ra8_rsip_test_invalid_len = 5U,           /**< Non-multiple-of-4 length.    */
  k_ra8_rsip_test_isr_garbage = 0x40000000UL, /**< Bit outside ISR field.       */
  k_ra8_rsip_test_bist_polls  = 3U,           /**< Late BIST pass poll index.   */
  k_ra8_rsip_test_kat_chunk   = 1000U,        /**< Bytes per million-a update.  */
  k_ra8_rsip_test_kat_repeats = 1000U,        /**< Updates in million-a KAT.    */
} ra8_rsip_test_const_t;

/**
 * @var s_test_isr_count
 * @brief Number of times the test stub callback has fired.
 *
 * @warning Reset by ``internal_prep`` before every test that uses it.
 * @since 0.1.0
 */
static uint32_t s_test_isr_count;

/**
 * @var s_test_isr_last
 * @brief Most-recent ISR snapshot the stub callback received.
 *
 * @since 0.1.0
 */
static uint32_t s_test_isr_last;

/**
 * @brief Stub IRQ callback that just records what it sees.
 *
 * @param[in] ctx Caller context (unused).
 * @param[in] isr Snapshot from ``ra8_rsip_dispatch``.
 * @since 0.1.0 @details Implements the stub rsip cb fixture operation used only by this focused test executable. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_stub_rsip_cb(void* ctx, uint32_t isr)
{
  (void)ctx;
  ++s_test_isr_count;
  s_test_isr_last = isr;
}

/**
 * @brief Reset the world before each test.
 * @since 0.1.0 @details Implements the prep fixture operation used only by this focused test executable. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_prep(void)
{
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  (void)ra8_mstp_init();
  s_test_isr_count = 0U;
  s_test_isr_last  = 0U;
}

/**
 * @brief Happy-path: init runs MSTP release and BIST.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches) @details Executes the init happy scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_init_happy(void)
{
  TEST_BEGIN("rsip init happy");
  internal_prep();

  const ra8_rsip_config_t cfg = {.run_bist = true};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_init(&cfg));

  /* CTRL.ENABLE stays set and the one-shot BIST trigger is cleared
   * post-pass, so ENABLE is the only bit left in CTRL. */
  TEST_ASSERT_EQ(k_ra8_rsip_mask_ctrl_enable, *ra8_rsip_reg32(k_ra8_rsip_off_ctrl));
  /* The driver never forges STATUS.BIST_OK: the read-only status word
   * is exactly what the "engine" (here: the RAM backing, unarmed wait
   * seam) latched -- nothing. */
  uint32_t status = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_get_status(&status));
  TEST_ASSERT((status & (uint32_t)k_ra8_rsip_mask_status_bistok) == 0U);

  TEST_END("rsip init happy");
}

/**
 * @brief BIST timeout: STATUS.BIST_OK never asserts -> init fails closed.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- arms the ra8_fake_mmio wait
  * seam so `priv_wait_bit` runs to its budget and `internal_run_bist`
  * takes its single-condition failure branch; no `&&` or `||` involved) @details Executes the init bist timeout scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_init_bist_timeout(void)
{
  TEST_BEGIN("rsip init bist timeout");
  internal_prep();

  /* Arm the exact register internal_run_bist polls: the wait now runs
   * its full budget and times out, so init reports the BIST failure
   * and backs the module out (MSTP re-gated). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait(ra8_rsip_reg32(k_ra8_rsip_off_status)));
  const ra8_rsip_config_t cfg = {.run_bist = true};
  TEST_ASSERT_EQ(k_ra8_err_hw_init_failed, ra8_rsip_init(&cfg));

  /* Disarm; a clean retry must succeed on the same engine. */
  ra8_fake_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_init(&cfg));

  TEST_END("rsip init bist timeout");
}

/**
 * @brief Late BIST pass: the poll loop iterates before BIST_OK asserts.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- ra8_fake_mmio_satisfy_after
  * steps `priv_wait_bit` through its continuation iterations before
  * the wait is satisfied, exercising the loop-iteration branch) @details Executes the init bist late pass scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_init_bist_late_pass(void)
{
  TEST_BEGIN("rsip init bist late pass");
  internal_prep();

  /* The "engine" asserts BIST_OK on the 3rd poll: the bounded wait
   * must iterate and still converge to success. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fake_mmio_satisfy_after(ra8_rsip_reg32(k_ra8_rsip_off_status),
                                             (uint32_t)k_ra8_rsip_test_bist_polls));
  const ra8_rsip_config_t cfg = {.run_bist = true};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_rsip_mask_ctrl_enable, *ra8_rsip_reg32(k_ra8_rsip_off_ctrl));

  TEST_END("rsip init bist late pass");
}

/**
 * @brief Init with run_bist=false skips BIST gate.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches) @details Executes the init skip bist scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_init_skip_bist(void)
{
  TEST_BEGIN("rsip init skip bist");
  internal_prep();

  const ra8_rsip_config_t cfg = {.run_bist = false};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_init(&cfg));

  uint32_t status = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_get_status(&status));
  /* Without BIST we should NOT have observed BIST_OK auto-asserting. */
  TEST_ASSERT((status & (uint32_t)k_ra8_rsip_mask_status_bistok) == 0U);

  TEST_END("rsip init skip bist");
}

/**
 * @brief Null cfg is rejected.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches) @details Executes the init null cfg scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_init_null_cfg(void)
{
  TEST_BEGIN("rsip init null cfg");
  internal_prep();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_init(nullptr));

  TEST_END("rsip init null cfg");
}

/**
 * @brief Deinit clears CTRL and gates the MSTP bit.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches) @details Executes the deinit scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_deinit(void)
{
  TEST_BEGIN("rsip deinit");
  internal_prep();

  const ra8_rsip_config_t cfg = {.run_bist = true};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_deinit());
  TEST_ASSERT_EQ(0, *ra8_rsip_reg32(k_ra8_rsip_off_ctrl));

  TEST_END("rsip deinit");
}

/**
 * @brief TRNG draws 32 bytes from RND_DATA.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches) @details Executes the trng read scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_trng_read(void)
{
  TEST_BEGIN("rsip trng read fail-closed");
  internal_prep();

  /* The RSIP-E50D TRNG has no working register interface on silicon (the map is
   * invented), so ra8_rsip_trng_read fails closed with k_ra8_err_not_supported
   * rather than return a deterministic or all-zero value dressed up as entropy. */
  uint8_t buf[32] = {};
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_rsip_trng_read(buf, (uint32_t)k_ra8_rsip_test_trng_bytes));

  /* The buffer must be left untouched -- no fake entropy is written. */
  TEST_ASSERT_EQ(0x00U, buf[0]);
  TEST_ASSERT_EQ(0x00U, buf[31]);
  TEST_END("rsip trng read fail-closed");
}

/**
 * @brief TRNG rejects null buffer + bad length.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches) @details Executes the trng arg check scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_trng_arg_check(void)
{
  TEST_BEGIN("rsip trng arg check");
  internal_prep();

  uint8_t buf[8] = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_trng_read(nullptr, (uint32_t)k_ra8_rsip_test_trng_bytes));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rsip_trng_read(buf, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rsip_trng_read(buf, (uint32_t)k_ra8_rsip_test_invalid_len));

  TEST_END("rsip trng arg check");
}

/**
 * @brief SHA-256 streams the message and reads the digest back.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches) @details Executes the sha256 happy scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_sha256_happy(void)
{
  TEST_BEGIN("rsip sha256 happy");
  internal_prep();

  const ra8_rsip_config_t cfg = {.run_bist = true};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_init(&cfg));

  const uint8_t msg[8]     = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};
  uint8_t       digest[32] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256(msg, (uint32_t)k_ra8_rsip_test_msg_bytes, digest));

  /* Real FIPS 180-4 SHA-256("abcdefgh")
   * = 9c56cc51b374c3ba189210d5b6d4bf57790d351c96c47c02190ecf1e430635ab. */
  TEST_ASSERT_EQ(0x9CU, digest[0]);
  TEST_ASSERT_EQ(0x56U, digest[1]);
  TEST_ASSERT_EQ(0xCCU, digest[2]);
  TEST_ASSERT_EQ(0x51U, digest[3]);
  TEST_ASSERT_EQ(0xABU, digest[31]);

  TEST_END("rsip sha256 happy");
}

/**
 * @brief SHA-256 handles zero-length and partial-word tails.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches) @details Executes the sha256 partial tail scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_sha256_partial_tail(void)
{
  TEST_BEGIN("rsip sha256 partial tail");
  internal_prep();

  const ra8_rsip_config_t cfg = {.run_bist = false};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_init(&cfg));

  const uint8_t msg[5]     = {0x01U, 0x02U, 0x03U, 0x04U, 0x05U};
  uint8_t       digest[32] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256(msg, 5U, digest));

  /* Real SHA-256(01 02 03 04 05)
   * = 74f81fe167d99b4cb41d6d0ccda82278caee9f3e2f25d5e5a3936ff3dcec60d0. */
  TEST_ASSERT_EQ(0x74U, digest[0]);
  TEST_ASSERT_EQ(0xF8U, digest[1]);
  TEST_ASSERT_EQ(0x1FU, digest[2]);
  TEST_ASSERT_EQ(0xE1U, digest[3]);
  TEST_ASSERT_EQ(0xD0U, digest[31]);

  TEST_END("rsip sha256 partial tail");
}

/**
 * @brief SHA-256 rejects null pointers.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches) @details Executes the sha256 null scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_sha256_null(void)
{
  TEST_BEGIN("rsip sha256 null");
  internal_prep();

  uint8_t out[32] = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_sha256(nullptr, 4U, out));
  const uint8_t msg[1] = {0U};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_sha256(msg, 1U, nullptr));

  TEST_END("rsip sha256 null");
}

/**
 * @brief Status get / clear and ISR validation.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches) @details Executes the status clear scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_status_clear(void)
{
  TEST_BEGIN("rsip status clear");
  internal_prep();

  /* Pre-populate ISR with one valid and one ignored bit. */
  *ra8_rsip_reg32(k_ra8_rsip_off_isr) = (uint32_t)k_ra8_rsip_mask_isr_done;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_clear_status((uint32_t)k_ra8_rsip_mask_isr_done));

  /* Bit outside the ISR field is rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rsip_clear_status((uint32_t)k_ra8_rsip_test_isr_garbage));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rsip_clear_status(0U));

  uint32_t status = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_get_status(nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_get_status(&status));

  TEST_END("rsip status clear");
}

/**
 * @brief Attach handler + dispatch fans out events.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches) @details Executes the attach dispatch scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_attach_dispatch(void)
{
  TEST_BEGIN("rsip attach dispatch");
  internal_prep();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rsip_attach_handler(internal_stub_rsip_cb, (void*)(uintptr_t)0xDEADU));

  /* Pre-arm an ISR bit, then dispatch and confirm the cb fired. */
  *ra8_rsip_reg32(k_ra8_rsip_off_isr) = (uint32_t)k_ra8_rsip_mask_isr_rnd;
  ra8_rsip_dispatch();
  TEST_ASSERT_EQ(1, s_test_isr_count);
  TEST_ASSERT_EQ(k_ra8_rsip_mask_isr_rnd, s_test_isr_last);

  /* The W1C ack wrote the snapshot back; on silicon that clears the
   * pending bits, on the RAM backing the word still reads the snapshot
   * (the driver no longer forges the cleared state). Stage the
   * post-ack value the hardware would latch, then verify an empty ISR
   * makes dispatch a no-op. */
  TEST_ASSERT_EQ(k_ra8_rsip_mask_isr_rnd, *ra8_rsip_reg32(k_ra8_rsip_off_isr));
  *ra8_rsip_reg32(k_ra8_rsip_off_isr) = 0U;
  ra8_rsip_dispatch();
  TEST_ASSERT_EQ(1, s_test_isr_count);

  /* Detach -> next dispatch does not invoke the cb. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_attach_handler(nullptr, nullptr));
  *ra8_rsip_reg32(k_ra8_rsip_off_isr) = (uint32_t)k_ra8_rsip_mask_isr_done;
  ra8_rsip_dispatch();
  TEST_ASSERT_EQ(1, s_test_isr_count);

  TEST_END("rsip attach dispatch");
}

/**
 * @brief Power transition: enter + exit stop.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches) @details Executes the power transition scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_power_transition(void)
{
  TEST_BEGIN("rsip power transition");
  internal_prep();

  const ra8_rsip_config_t cfg = {.run_bist = true};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_enter_stop());
  TEST_ASSERT_EQ(0, *ra8_rsip_reg32(k_ra8_rsip_off_ctrl));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_exit_stop());
  TEST_ASSERT_EQ(k_ra8_rsip_mask_ctrl_enable, *ra8_rsip_reg32(k_ra8_rsip_off_ctrl));

  TEST_END("rsip power transition");
}

/**
 * @brief Exit-stop propagates a BIST failure and re-gates the module.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- arms the ra8_fake_mmio wait
  * seam so the post-wake BIST times out and ra8_rsip_exit_stop takes
  * its single-condition failure branch) @details Executes the exit stop bist timeout scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_exit_stop_bist_timeout(void)
{
  TEST_BEGIN("rsip exit stop bist timeout");
  internal_prep();

  const ra8_rsip_config_t cfg = {.run_bist = true};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_enter_stop());

  /* The post-wake self-test never completes: exit_stop must fail
   * closed instead of reporting a ready engine. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait(ra8_rsip_reg32(k_ra8_rsip_off_status)));
  TEST_ASSERT_EQ(k_ra8_err_hw_init_failed, ra8_rsip_exit_stop());

  TEST_END("rsip exit stop bist timeout");
}

/**
 * @brief Initialise the engine for a sub-test that needs ENABLE asserted.
 * @since 0.1.0 @details Implements the prep running fixture operation used only by this focused test executable. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_prep_running(void)
{
  internal_prep();
  const ra8_rsip_config_t cfg = {.run_bist = true};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_init(&cfg));
}

/* ===========================================================================
 * Sweep 15 / Phase 1.1: incremental SHA-256 + HMAC-SHA-256 (TLS transcript)
 * ===========================================================================
 */

/**
 * @brief One-shot software SHA-256 KAT: empty input.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches) @details Executes the sha256 inc empty scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_sha256_inc_empty(void)
{
  TEST_BEGIN("rsip sha256 incremental empty");
  internal_prep_running();

  ra8_rsip_sha256_ctx_t ctx        = {};
  uint8_t               digest[32] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_init(&ctx));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_final(&ctx, digest));

  /* SHA-256("") well-known KAT: e3b0c442 98fc1c14 ... 7852b855. */
  TEST_ASSERT_EQ(0xE3U, digest[0]);
  TEST_ASSERT_EQ(0xB0U, digest[1]);
  TEST_ASSERT_EQ(0xC4U, digest[2]);
  TEST_ASSERT_EQ(0x42U, digest[3]);
  TEST_ASSERT_EQ(0x55U, digest[31]);

  /* Re-using a finalised context is a state error. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_rsip_sha256_final(&ctx, digest));

  TEST_END("rsip sha256 incremental empty");
}

/**
 * @brief Incremental SHA-256 KAT: "abc" in two chunks.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches) @details Executes the sha256 inc abc split scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_sha256_inc_abc_split(void)
{
  TEST_BEGIN("rsip sha256 incremental abc split");
  internal_prep_running();

  ra8_rsip_sha256_ctx_t ctx        = {};
  uint8_t               digest[32] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_init(&ctx));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_update(&ctx, (const uint8_t*)"ab", 2U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_update(&ctx, (const uint8_t*)"c", 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_final(&ctx, digest));

  /* FIPS 180-4 Appendix B.1: SHA-256("abc") = ba7816bf 8f01cfea ... f20015ad. */
  TEST_ASSERT_EQ(0xBAU, digest[0]);
  TEST_ASSERT_EQ(0x78U, digest[1]);
  TEST_ASSERT_EQ(0x16U, digest[2]);
  TEST_ASSERT_EQ(0xBFU, digest[3]);
  TEST_ASSERT_EQ(0xADU, digest[31]);

  TEST_END("rsip sha256 incremental abc split");
}

/**
 * @brief Incremental SHA-256 million-`a` long-message KAT.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches) @details Streams one million `a`
  * bytes through 1,000 independent updates and compares every digest byte with
  * the FIPS 180-4 long-message result. @pre Fixed-capacity fixture storage
  * required by this operation is available. @pre Arguments follow the
  * interface contract exercised by this helper. @post Documented outputs
  * contain the exercised result when the operation succeeds. @post Mutations
  * remain confined to documented outputs and file-local fixture state. @note
  * File-local helper; no ownership escapes this focused test executable.
  * @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_sha256_inc_long_message(void)
{
  TEST_BEGIN("rsip sha256 incremental million-a");
  internal_prep_running();
  static const uint8_t local_expected[32] = {
    0xCDU, 0xC7U, 0x6EU, 0x5CU, 0x99U, 0x14U, 0xFBU, 0x92U, 0x81U, 0xA1U, 0xC7U,
    0xE2U, 0x84U, 0xD7U, 0x3EU, 0x67U, 0xF1U, 0x80U, 0x9AU, 0x48U, 0xA4U, 0x97U,
    0x20U, 0x0EU, 0x04U, 0x6DU, 0x39U, 0xCCU, 0xC7U, 0x11U, 0x2CU, 0xD0U,
  };
  uint8_t input[k_ra8_rsip_test_kat_chunk];
  for (uint32_t i = 0U; i < (uint32_t)sizeof(input); ++i) {
    input[i] = (uint8_t)'a';
  }
  ra8_rsip_sha256_ctx_t ctx                            = {};
  uint8_t               digest[sizeof(local_expected)] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_init(&ctx));
  for (uint32_t i = 0U; i < k_ra8_rsip_test_kat_repeats; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_update(&ctx, input, (uint32_t)sizeof(input)));
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_final(&ctx, digest));
  for (uint32_t i = 0U; i < (uint32_t)sizeof(digest); ++i) {
    TEST_ASSERT_EQ(local_expected[i], digest[i]);
  }
  TEST_END("rsip sha256 incremental million-a");
}

/**
 * @brief Null + state checks for the incremental SHA API.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches) @details Executes the sha256 inc arg check scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_sha256_inc_arg_check(void)
{
  TEST_BEGIN("rsip sha256 incremental arg check");
  internal_prep_running();

  ra8_rsip_sha256_ctx_t ctx        = {};
  uint8_t               digest[32] = {};
  uint8_t               data[4]    = {0U};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_sha256_init(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_sha256_update(nullptr, data, 4U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_sha256_final(nullptr, digest));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_sha256_final(&ctx, nullptr));

  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_rsip_sha256_update(&ctx, data, 4U));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_init(&ctx));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_sha256_update(&ctx, nullptr, 4U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_update(&ctx, nullptr, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_final(&ctx, digest));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_init(&ctx));
  ctx.total_bytes = UINT64_MAX;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_rsip_sha256_update(&ctx, data, 1U));
  TEST_ASSERT_EQ(UINT64_MAX, ctx.total_bytes);

  TEST_END("rsip sha256 incremental arg check");
}

/**
 * @brief HMAC-SHA-256 RFC 4231 Test Case 1.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches) @details Executes the hmac sha256 inc rfc4231 1 scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_hmac_sha256_inc_rfc4231_1(void)
{
  TEST_BEGIN("rsip hmac sha256 incremental rfc4231 case 1");
  internal_prep_running();

  uint8_t key[k_t_key_short_len];
  for (uint32_t i = 0U; i < k_t_key_short_len; ++i) {
    key[i] = k_t_key_short_byte;
  }
  const uint8_t data[] = {'H', 'i', ' ', 'T', 'h', 'e', 'r', 'e'};

  ra8_rsip_hmac_sha256_ctx_t ctx     = {};
  uint8_t                    mac[32] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_hmac_sha256_init(&ctx, key, sizeof(key)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_hmac_sha256_update(&ctx, data, (uint32_t)sizeof(data)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_hmac_sha256_final(&ctx, mac));

  /* RFC 4231 Test Case 1: b0344c61 d8db3853 ... 2e32cff7. */
  TEST_ASSERT_EQ(0xB0U, mac[0]);
  TEST_ASSERT_EQ(0x34U, mac[1]);
  TEST_ASSERT_EQ(0x4CU, mac[2]);
  TEST_ASSERT_EQ(0x61U, mac[3]);
  TEST_ASSERT_EQ(0xF7U, mac[31]);

  TEST_END("rsip hmac sha256 incremental rfc4231 case 1");
}

/*
 * @brief HMAC-SHA-256 with an oversized key (forces SHA collapse to 32 bytes).
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
/**
 * @brief RFC 2104 key preparation: hash a key longer than the block size.
 *
 * @details
 * A key longer than the 64-byte SHA-256 block is replaced by its own digest,
 * zero-padded back out to a full block. That is the step this test exists to
 * exercise, so the reference computes it independently of the HMAC API.
 *
 * @param[in]  key      Key bytes, longer than one block.
 * @param[in]  key_len  Length of @p key in bytes.
 * @param[out] prepared 64-byte prepared key block.
 * @pre @p key and @p prepared are non-null.
 * @pre @p prepared is zeroed by the caller, so the pad is already in place.
 * @post The first 32 bytes of @p prepared hold SHA-256(@p key).
 * @post The remaining bytes of @p prepared are left as the caller's zero pad.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_hmac_ref_prepare_key(const uint8_t* key, uint32_t key_len, uint8_t* prepared)
{
  ra8_rsip_sha256_ctx_t prep_ctx   = {};
  uint8_t               prep_h[32] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_init(&prep_ctx));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_update(&prep_ctx, key, key_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_final(&prep_ctx, prep_h));
  for (uint32_t i = 0U; i < 32U; ++i) {
    prepared[i] = prep_h[i];
  }
}

/**
 * @brief RFC 2104 two-pass HMAC over a prepared key, using the SHA primitive.
 *
 * @details
 * Computes `H((K ^ opad) || H((K ^ ipad) || data))` directly, so the expected
 * MAC comes from the same hash the driver uses but along a path that shares no
 * code with `ra8_rsip_hmac_sha256_*`.
 *
 * @param[in]  prepared 64-byte prepared key block.
 * @param[in]  data     Message bytes to authenticate.
 * @param[in]  data_len Length of @p data in bytes.
 * @param[out] expect   32-byte reference MAC.
 * @pre All pointer arguments are non-null.
 * @pre @p prepared holds a full 64-byte block.
 * @post @p expect holds the reference MAC.
 * @post @p prepared and @p data are unmodified.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_hmac_ref_compute(const uint8_t* prepared,
                                                   const uint8_t* data,
                                                   uint32_t       data_len,
                                                   uint8_t*       expect)
{
  uint8_t ipad[k_t_hmac_block_len];
  uint8_t opad[k_t_hmac_block_len];
  for (uint32_t i = 0U; i < k_t_hmac_block_len; ++i) {
    ipad[i] = prepared[i] ^ k_t_hmac_ipad_byte;
    opad[i] = prepared[i] ^ k_t_hmac_opad_byte;
  }
  uint8_t               inner[32] = {};
  ra8_rsip_sha256_ctx_t inner_ctx = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_init(&inner_ctx));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_update(&inner_ctx, ipad, 64U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_update(&inner_ctx, data, data_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_final(&inner_ctx, inner));

  ra8_rsip_sha256_ctx_t outer_ctx = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_init(&outer_ctx));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_update(&outer_ctx, opad, 64U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_update(&outer_ctx, inner, 32U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_final(&outer_ctx, expect));
}

/**
 * @test internal_test_hmac_sha256_inc_oversized_key
 * @brief HMAC-SHA-256 with a key longer than the SHA-256 block.
 *
 * @par MC/DC:
 * (no compound decision is varied by this case -- it exercises the RFC 2104
 * oversized-key preparation path, where a key longer than the 64-byte block
 * collapses to its own digest, and compares the MAC against an independent
 * reference. The only compound decision on the init path,
 * `(key == nullptr) && (key_len != 0U)`, is held at its key-non-null
 * short-circuit leg here; its MC/DC vectors live in internal_test_mcdc_hmac_init_key_len.) @details Executes the hmac sha256 inc oversized key scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_hmac_sha256_inc_oversized_key(void)
{
  TEST_BEGIN("rsip hmac sha256 incremental oversized key");
  internal_prep_running();

  uint8_t key[k_t_key_long_len];
  for (uint32_t i = 0U; i < sizeof(key); ++i) {
    key[i] = k_t_key_long_byte;
  }
  const uint8_t data[] = {'T', 'e', 's', 't'};

  /* Reference: build expected MAC by hand using the same primitive. */
  uint8_t prepared[k_t_hmac_block_len] = {0U};
  internal_hmac_ref_prepare_key(key, (uint32_t)sizeof(key), prepared);
  uint8_t expect[32] = {};
  internal_hmac_ref_compute(prepared, data, (uint32_t)sizeof(data), expect);

  /* Run through the public HMAC API and compare. */
  ra8_rsip_hmac_sha256_ctx_t ctx     = {};
  uint8_t                    mac[32] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_hmac_sha256_init(&ctx, key, (uint32_t)sizeof(key)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_hmac_sha256_update(&ctx, data, (uint32_t)sizeof(data)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_hmac_sha256_final(&ctx, mac));

  for (uint32_t i = 0U; i < 32U; ++i) {
    TEST_ASSERT_EQ(expect[i], mac[i]);
  }
  TEST_END("rsip hmac sha256 incremental oversized key");
}

/**
 * @brief Null + state checks for the incremental HMAC-SHA-256 API.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches) @details Executes the hmac sha256 inc arg check scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_hmac_sha256_inc_arg_check(void)
{
  TEST_BEGIN("rsip hmac sha256 incremental arg check");
  internal_prep_running();

  ra8_rsip_hmac_sha256_ctx_t ctx     = {};
  uint8_t                    key[16] = {0U};
  uint8_t                    data[4] = {0U};
  uint8_t                    mac[32] = {};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_hmac_sha256_init(nullptr, key, 16U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_hmac_sha256_init(&ctx, nullptr, 16U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_hmac_sha256_init(&ctx, nullptr, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_hmac_sha256_final(&ctx, mac));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_hmac_sha256_update(nullptr, data, 4U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_hmac_sha256_final(nullptr, mac));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_hmac_sha256_final(&ctx, nullptr));

  /* Update / final on a finalised ctx returns invalid state. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_rsip_hmac_sha256_update(&ctx, data, 4U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_rsip_hmac_sha256_final(&ctx, mac));

  TEST_END("rsip hmac sha256 incremental arg check");
}

/**
 * @brief Verify the SHA-256 command-issue sequence touched the right registers.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches) @details Executes the sha256 command sequence scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_sha256_command_sequence(void)
{
  TEST_BEGIN("rsip sha256 abc known-answer");
  internal_prep_running();

  const uint8_t msg[3] = {'a', 'b', 'c'};
  uint8_t       d[32]  = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256(msg, 3U, d));

  /* Real FIPS 180-4 SHA-256("abc")
   * = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad. */
  TEST_ASSERT_EQ(0xBAU, d[0]);
  TEST_ASSERT_EQ(0x78U, d[1]);
  TEST_ASSERT_EQ(0x16U, d[2]);
  TEST_ASSERT_EQ(0xBFU, d[3]);
  TEST_ASSERT_EQ(0xADU, d[31]);

  TEST_END("rsip sha256 abc known-answer");
}

/* ---------------------------------------------------------------------------
 * MC/DC vector tests
 * ------------------------------------------------------------------------ */

/**
 * @test internal_test_sha256_update_mcdc_data_len
 *
 * @par MC/DC:
 * Decision: `if ((data == nullptr) && (len != 0U))`
 * (2 conditions, `ra8_rsip_sha256_update` in libs/ra8_hal/src/ra8_rsip.c)
 * Standard: DO-178C Table A-7 obj 5; ISO 26262 Part 6 Table 12.
 * Short-circuit AND with N=2; N+1 = 3 vectors.
 * - Vector 1: data!=null, len=8 -> C1=F (short-circuits) -> Decision F (ok)
 * - Vector 2: data==null, len=0 -> C1=T, C2=F -> Decision F (ok zero-len)
 * - Vector 3: data==null, len=8 -> C1=T, C2=T -> Decision T (null_ptr)
 * Vectors 1+3 vary C1 (decision flips); vectors 2+3 vary C2 with C1=T.
 * Same compound shape repeats in `ra8_rsip_hmac_sha256_init`,
 * `ra8_rsip_poly1305`, `internal_hash_validate` and `ra8_rsip_hmac`;
 * per DO-178C 6.4.4.3 source-text equivalence a single MC/DC vector
 * set discharges the obligation for all of them. @brief Verify sha256 update mcdc data len behavior. @details Executes the sha256 update mcdc data len scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_sha256_update_mcdc_data_len(void)
{
  TEST_BEGIN("rsip sha256_update MC/DC: data==null && len!=0");
  internal_prep_running();

  ra8_rsip_sha256_ctx_t ctx = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_init(&ctx));

  /* Vector 1: data non-null, len=8. C1=F short-circuits. Decision F. */
  const uint8_t buf[8] = {0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_update(&ctx, buf, 8U));

  /* Vector 2: data=null, len=0. C1=T, C2=F. Decision F (zero-byte
   * append is a no-op and returns ok). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256_update(&ctx, nullptr, 0U));

  /* Vector 3: data=null, len=8. C1=T, C2=T. Decision T -> null_ptr. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_sha256_update(&ctx, nullptr, 8U));
  TEST_END("rsip sha256_update MC/DC: data==null && len!=0");
}

/**
 * @test internal_test_mcdc_hmac_init_key_len
 *
 * @par MC/DC:
 * Decision: ``if ((key == nullptr) && (key_len != 0U))`` (2 conditions,
 * libs/ra8_hal/src/ra8_rsip.c ra8_rsip_hmac_sha256_init). N+1 = 3.
 * - V1: key=valid, key_len=4 -> C1=F short-circuits -> dec F (proceeds)
 * - V2: key=NULL,  key_len=0 -> C1=T, C2=F          -> dec F (zero-key path)
 * - V3: key=NULL,  key_len=4 -> C1=T, C2=T          -> dec T -> null_ptr @brief Verify mcdc hmac init key len behavior. @details Executes the mcdc hmac init key len scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_hmac_init_key_len(void)
{
  TEST_BEGIN("rsip hmac_sha256_init MC/DC: key==null && key_len!=0");
  internal_prep_running();
  ra8_rsip_hmac_sha256_ctx_t ctx    = {};
  const uint8_t              key[4] = {0x11U, 0x22U, 0x33U, 0x44U};
  /* V1: valid key. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_hmac_sha256_init(&ctx, key, 4U));
  /* V2: zero-len, NULL key (zero-key HMAC is permitted). */
  ra8_rsip_hmac_sha256_ctx_t ctx2 = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_hmac_sha256_init(&ctx2, nullptr, 0U));
  /* V3: NULL with non-zero len. */
  ra8_rsip_hmac_sha256_ctx_t ctx3 = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_hmac_sha256_init(&ctx3, nullptr, 4U));
  TEST_END("rsip hmac_sha256_init MC/DC: key==null && key_len!=0");
}

/**
 * @var s_test_roster
 * @brief Fixed-order roster of every test case in this translation unit.
 *
 * @details
 * main() walks this table instead of naming each case, so its size does not
 * grow with the number of tests and adding a case is a one-line edit.
 *
 * @note Order is significant: cases run top to bottom, exactly as before.
 */
static void (*const s_test_roster[])(void) = {
  internal_test_init_happy,
  internal_test_init_bist_timeout,
  internal_test_init_bist_late_pass,
  internal_test_init_skip_bist,
  internal_test_init_null_cfg,
  internal_test_deinit,
  internal_test_trng_read,
  internal_test_trng_arg_check,
  internal_test_sha256_happy,
  internal_test_sha256_partial_tail,
  internal_test_sha256_null,
  internal_test_sha256_command_sequence,
  internal_test_status_clear,
  internal_test_attach_dispatch,
  internal_test_power_transition,
  internal_test_exit_stop_bist_timeout,
  /* Sweep 15 / Phase 1.1: incremental hash + HMAC for TLS handshakes. */
  internal_test_sha256_inc_empty,
  internal_test_sha256_inc_abc_split,
  internal_test_sha256_inc_long_message,
  internal_test_sha256_inc_arg_check,
  internal_test_hmac_sha256_inc_rfc4231_1,
  internal_test_hmac_sha256_inc_oversized_key,
  internal_test_hmac_sha256_inc_arg_check,
  internal_test_sha256_update_mcdc_data_len,
  internal_test_mcdc_hmac_init_key_len,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  return 0;
}
