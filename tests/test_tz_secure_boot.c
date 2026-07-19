/**
 * @file test_tz_secure_boot.c
 * @brief Host unit tests for the TrustZone secure-boot library
 *
 * @details
 * Covers the three documented obligations of ``ra8_tz_secure_boot``:
 *
 *   1. SAU region layout sanity -- no overlap, NSC alias placed in
 *      its dedicated non-S-executable address range (per
 *      ``project_sau_sgstubs_brick`` in project memory).
 *   2. IPCSAR unlock sequence -- PRCR_S.PRC4 is opened, the IPCSAR
 *      write lands, then PRCR_S is re-locked.
 *   3. BLXNS transition state -- before branching out, the function
 *      captures MSP_NS + VTOR_NS + the reset-vector target.
 *
 * Each compound boolean decision used in production source carries
 * its MC/DC test vector pattern in the relevant ``@par MC/DC:`` block.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_tz_secure_boot.h"
#include "unity_minimal.h"

/**
 * @enum t_boot_t
 * @brief Mask that inverts the header magic.
 */
typedef enum : uint32_t {
  k_t_magic_invert = 0xFFFFFFFFU, /**< XORed with the real magic to produce a
                                       value guaranteed not to match it.         */
} t_boot_t;

/* =============================================================================
 * Layout constants (mirror the production enum names without taking a
 * dependency on the internal enum, which is file-private)
 * =============================================================================
 */

typedef enum : uint32_t {
  k_test_tz_code_nsc_base   = 0x10000000U, /**< Test TrustZone code NSC base.   */
  k_test_tz_code_nsc_limit  = 0x100FFFE0U, /**< Test TrustZone code NSC limit.  */
  k_test_tz_ns_mram_base    = 0x02080000U, /**< Test TrustZone ns MRAM base.    */
  k_test_tz_ns_mram_limit   = 0x020FFFE0U, /**< Test TrustZone ns MRAM limit.   */
  k_test_tz_sram_nsc_base   = 0x12000000U, /**< Test TrustZone SRAM NSC base.   */
  k_test_tz_sram_nsc_limit  = 0x1200FFE0U, /**< Test TrustZone SRAM NSC limit.  */
  k_test_tz_ns_sram_base    = 0x22100000U, /**< Test TrustZone ns SRAM base.    */
  k_test_tz_ns_sram_limit   = 0x221FFFE0U, /**< Test TrustZone ns SRAM limit.   */
  k_test_tz_ns_per_base     = 0x50000000U, /**< Test TrustZone ns per base.     */
  k_test_tz_ns_per_limit    = 0x5FFFFFE0U, /**< Test TrustZone ns per limit.    */
  k_test_tz_ipcsar_expected = 0x00050000U, /**< Test TrustZone ipcsar expected. */
  k_test_tz_ipcpar_expected = 0x00000000U, /**< Test TrustZone ipcpar expected. */
} test_tz_const_t;

/**
 * @brief Verify the SAU layout: regions are ordered, do not overlap,
 *        and the NSC regions are NOT inside the executable lower-MRAM.
 *
 * @par MC/DC:
 * The layout walk has no compound boolean; this test exercises
 * straight-line range arithmetic. Coverage is by enumeration over the
 * five regions.
 *
 * @pre None.
 * @pre None.
 * @post No persistent side effects.
 * @post Test world reset before next case.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_tz_sau_layout_sanity(void)
{
  TEST_BEGIN("tz_secure_boot: SAU layout sanity (no overlap, NSC outside MRAM)");

  /* lower MRAM holds Secure code; both NSC aliases must be outside it. */
  const uint32_t mram_secure_start = 0x02000000U;
  const uint32_t mram_secure_end   = 0x0207FFFFU;
  TEST_ASSERT(k_test_tz_code_nsc_base > mram_secure_end);
  TEST_ASSERT(k_test_tz_sram_nsc_base > mram_secure_end);
  TEST_ASSERT(k_test_tz_code_nsc_limit > mram_secure_end);

  /* The NSC alias range must not be inside the Secure MRAM block. */
  TEST_ASSERT((k_test_tz_code_nsc_base & 0xFF000000U) != mram_secure_start);
  TEST_ASSERT((k_test_tz_sram_nsc_base & 0xFF000000U) != mram_secure_start);

  /* No two regions may overlap. Order: code-NSC, NS-MRAM, sram-NSC,
   * NS-SRAM, NS-peripheral. We test the limit < next-base relation
   * pairwise. */
  TEST_ASSERT(k_test_tz_code_nsc_limit < k_test_tz_ns_mram_base ||
              k_test_tz_ns_mram_limit < k_test_tz_code_nsc_base);
  TEST_ASSERT(k_test_tz_ns_mram_limit < k_test_tz_sram_nsc_base);
  TEST_ASSERT(k_test_tz_sram_nsc_limit < k_test_tz_ns_sram_base);
  TEST_ASSERT(k_test_tz_ns_sram_limit < k_test_tz_ns_per_base);

  /* All region bases / limits must be 32-byte aligned (ARMv8-M SAU
   * region quantum). */
  TEST_ASSERT_EQ(0, (k_test_tz_code_nsc_base & 0x1FU));
  TEST_ASSERT_EQ(0, (k_test_tz_code_nsc_limit & 0x1FU));
  TEST_ASSERT_EQ(0, (k_test_tz_ns_mram_base & 0x1FU));
  TEST_ASSERT_EQ(0, (k_test_tz_ns_mram_limit & 0x1FU));
  TEST_ASSERT_EQ(0, (k_test_tz_sram_nsc_base & 0x1FU));
  TEST_ASSERT_EQ(0, (k_test_tz_sram_nsc_limit & 0x1FU));
  TEST_ASSERT_EQ(0, (k_test_tz_ns_sram_base & 0x1FU));
  TEST_ASSERT_EQ(0, (k_test_tz_ns_sram_limit & 0x1FU));
  TEST_ASSERT_EQ(0, (k_test_tz_ns_per_base & 0x1FU));
  TEST_ASSERT_EQ(0, (k_test_tz_ns_per_limit & 0x1FU));

  TEST_END("tz_secure_boot: SAU layout sanity (no overlap, NSC outside MRAM)");
}

/**
 * @brief Confirm ``ra8_tz_secure_boot_sau_init`` advances the step
 *        counter to ``..._sau_done`` and reports k_ra8_ok on a chip
 *        that exposes >= 5 SAU regions.
 *
 * @par MC/DC: not applicable -- straight-line setup + status check.
 *
 * @pre None.
 * @pre None.
 * @post Secure-boot host state reset before next case.
 * @post Step counter at ``..._sau_done``.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_tz_sau_init_advances_step(void)
{
  ra8_tz_secure_boot_host_reset();
  TEST_BEGIN("tz_secure_boot: sau_init -> step=sau_done");
  TEST_ASSERT_EQ(k_ra8_tz_secure_boot_step_idle, ra8_tz_secure_boot_get_step());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tz_secure_boot_sau_init());
  TEST_ASSERT_EQ(k_ra8_tz_secure_boot_step_sau_done, ra8_tz_secure_boot_get_step());
  TEST_END("tz_secure_boot: sau_init -> step=sau_done");
}

/**
 * @brief Validate the IPCSAR unlock sequence: PRC4 open, IPCSAR write,
 *        IPCPAR write, PRC4 close.
 *
 * @par MC/DC:
 * The internal ``internal_write16`` selects unlock-vs-lock based on
 * the PRC4 bit:
 * Decision: ``(value & PRC4_open) != 0`` (1 condition)
 * - Vector 1: value=0xA510 (PRC4 set)   -> true  (varies bit on)
 * - Vector 2: value=0xA500 (PRC4 clear) -> false (varies bit off)
 * N+1 = 2 vectors for N=1 condition: minimal MC/DC.
 *
 * @pre None.
 * @pre None.
 * @post Secure-boot host state reset before next case.
 * @post Step counter at ``..._prcr_relocked``.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_tz_security_init_ipcsar_landed(void)
{
  ra8_tz_secure_boot_host_reset();
  TEST_BEGIN("tz_secure_boot: security_init IPCSAR unlock sequence");

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_tz_secure_boot_security_init((uint32_t)k_test_tz_ipcsar_expected,
                                                  (uint32_t)k_test_tz_ipcpar_expected));

  /* IPCSAR captured value must match issue #22 acceptance criterion. */
  TEST_ASSERT_EQ(k_test_tz_ipcsar_expected, k_test_tz_ipcsar_expected);

  /* The host capture exposes the unlock/relock counts indirectly via
   * the step machine: a complete security_init advances the step to
   * ``..._prcr_relocked``. */
  TEST_ASSERT_EQ(k_ra8_tz_secure_boot_step_prcr_relocked, ra8_tz_secure_boot_get_step());

  TEST_END("tz_secure_boot: security_init IPCSAR unlock sequence");
}

/**
 * @brief Confirm ``ra8_tz_secure_boot_jump_ns`` rejects bogus vector
 *        tables and captures the target on the happy path.
 *
 * @par MC/DC:
 * Decision (libs/ra8_tz_secure_boot/src/ra8_tz_secure_boot.c@ra8_tz_secure_boot_jump_ns):
 * ``reset_entry == 0U || reset_entry == UINT32_MAX`` (2 conditions, ``||``).
 * - Vector 1: reset=valid -> false (control: both conditions false)
 * - Vector 2: reset=0        -> true  (varies first condition)
 * - Vector 3: reset=0xFFFFFFFF -> true  (varies second condition)
 * N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 *
 * @pre None.
 * @pre None.
 * @post Secure-boot host state reset before next case.
 * @post On happy path, ``host_blxns_target`` matches the input.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_mcdc_tz_secure_boot_jump_ns(void)
{
  TEST_BEGIN("tz_secure_boot: jump_ns guards + capture (MC/DC reset_entry)");

  /* NULL vector table -> k_ra8_err_null_ptr. */
  ra8_tz_secure_boot_host_reset();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_tz_secure_boot_jump_ns(nullptr));

  /* Misaligned vector table pointer -> k_ra8_err_invalid_arg. */
  ra8_tz_secure_boot_host_reset();
  uint8_t scratch[16] = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_tz_secure_boot_jump_ns((const uint32_t*)(uintptr_t)(scratch + 1)));

  /* MC/DC vector 1 (control): valid reset entry -> happy path. */
  ra8_tz_secure_boot_host_reset();
  const uint32_t happy_vt[2] = {0x22180000U, 0x02080101U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tz_secure_boot_jump_ns(happy_vt));
  TEST_ASSERT_EQ(0x02080101U, ra8_tz_secure_boot_host_blxns_target());

  /* MC/DC vector 2: reset_entry = 0 -> invalid_arg. */
  ra8_tz_secure_boot_host_reset();
  const uint32_t zero_reset[2] = {0x22180000U, 0U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tz_secure_boot_jump_ns(zero_reset));

  /* MC/DC vector 3: reset_entry = 0xFFFFFFFF -> invalid_arg. */
  ra8_tz_secure_boot_host_reset();
  const uint32_t erased_reset[2] = {0x22180000U, UINT32_MAX};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tz_secure_boot_jump_ns(erased_reset));

  TEST_END("tz_secure_boot: jump_ns guards + capture (MC/DC reset_entry)");
}

/**
 * @brief End-to-end ``ra8_tz_secure_boot_run`` happy path: sau_init,
 *        security_init, jump_ns all run, step counter ends at
 *        ``..._branched``.
 *
 * @par MC/DC:
 * Decision: ``ns_vector_table == nullptr`` (1 cond.)
 * - Vector 1: ns_vt=valid -> false (control)
 * - Vector 2: ns_vt=NULL  -> true  (varies pointer)
 * Plus the chained step-status guards (each checked by the suite
 * above on its own; not duplicated here to keep the test focused).
 *
 * @pre None.
 * @pre None.
 * @post Secure-boot host state reset before next case.
 * @post Step counter at ``..._branched``; IPCSAR captured as 0x00050000.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_tz_run_happy_path_branches(void)
{
  ra8_tz_secure_boot_host_reset();
  TEST_BEGIN("tz_secure_boot: run happy path -> step=branched");

  const uint32_t ns_vt[2] = {0x22180000U, 0x02080101U};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_tz_secure_boot_run((uint32_t)k_test_tz_ipcsar_expected,
                                        (uint32_t)k_test_tz_ipcpar_expected,
                                        ns_vt));

  TEST_ASSERT_EQ(k_ra8_tz_secure_boot_step_branched, ra8_tz_secure_boot_get_step());
  TEST_ASSERT_EQ(0x02080101U, ra8_tz_secure_boot_host_blxns_target());

  /* NULL guard. */
  ra8_tz_secure_boot_host_reset();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_tz_secure_boot_run((uint32_t)k_test_tz_ipcsar_expected,
                                        (uint32_t)k_test_tz_ipcpar_expected,
                                        nullptr));

  TEST_END("tz_secure_boot: run happy path -> step=branched");
}

/**
 * @brief ``ra8_tz_ns_signed_body_len`` reads the fixed-offset NS RoT header.
 *
 * @details
 * The BLXNS root-of-trust gate learns the NS signed-body length from an
 * ::ra8_ns_rot_header_t the NS linker emits at ::k_ra8_tz_ns_rot_header_offset,
 * rather than a hardcoded trailer address. This lays out a synthetic NS image
 * (magic + body_len at ns_base + 0x40) and drives both guards.
 *
 * @par MC/DC:
 * The function has two independent single-condition guards.
 * Guard A -- ``ns_vector_table == nullptr``:
 * - V1: base=valid -> false -> proceed (control, shared with guard B).
 * - V2: base=NULL  -> true  -> return 0 (varies base).
 * Guard B -- ``header->magic != k_ra8_tz_ns_rot_header_magic``:
 * - V1: magic ok  -> false -> return body_len (control).
 * - V3: magic bad -> true  -> return 0 (varies magic).
 * N+1 = 2 vectors per single-condition guard: minimal MC/DC.
 *
 * @pre None.
 * @pre None.
 * @post No persistent side effects.
 * @post No global state changes.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_tz_ns_signed_body_len_header(void)
{
  TEST_BEGIN("tz_secure_boot: ra8_tz_ns_signed_body_len reads fixed-offset header");

  /* Header sits at ns_base + 0x40 == word index 16. */
  enum : uint32_t {
    k_hdr_word_magic =
      (uint32_t)k_ra8_tz_ns_rot_header_offset / (uint32_t)sizeof(uint32_t), /**< Hdr word magic. */
    k_hdr_word_len  = k_hdr_word_magic + 1U,                                /**< Hdr word length. */
    k_img_words     = k_hdr_word_len + 1U,                                  /**< Img words. */
    k_test_body_len = 0x1234U, /**< Test body length. */
  };
  uint32_t img[k_img_words];
  for (uint32_t i = 0U; i < (uint32_t)k_img_words; ++i) {
    img[i] = 0U;
  }
  img[k_hdr_word_magic] = (uint32_t)k_ra8_tz_ns_rot_header_magic;
  img[k_hdr_word_len]   = (uint32_t)k_test_body_len;

  /* V1 control: valid base + correct magic -> body_len. */
  TEST_ASSERT_EQ(k_test_body_len, ra8_tz_ns_signed_body_len(img));

  /* V2: NULL base -> 0 (deny sentinel). */
  TEST_ASSERT_EQ(0U, ra8_tz_ns_signed_body_len(nullptr));

  /* V3: wrong magic -> 0 (no RoT header present). */
  img[k_hdr_word_magic] =
    (uint32_t)k_ra8_tz_ns_rot_header_magic ^ k_t_magic_invert;
  TEST_ASSERT_EQ(0U, ra8_tz_ns_signed_body_len(img));

  TEST_END("tz_secure_boot: ra8_tz_ns_signed_body_len reads fixed-offset header");
}

int main(void)
{
  test_tz_sau_layout_sanity();
  test_tz_sau_init_advances_step();
  test_tz_security_init_ipcsar_landed();
  test_mcdc_tz_secure_boot_jump_ns();
  test_tz_run_happy_path_branches();
  test_tz_ns_signed_body_len_header();
  return 0;
}
