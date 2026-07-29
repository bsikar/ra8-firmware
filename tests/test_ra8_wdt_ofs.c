/**
 * @file test_ra8_wdt_ofs.c
 * @brief Unit tests for the OFSm option-setting decode in ra8_wdt.c
 *
 * @details
 * Split out of test_ra8_wdt_extended.c, which the 1000-line file-size cap
 * would otherwise reject. The seam is a real responsibility boundary: every
 * case here exercises ra8_wdt_ofs_get() and its reader hook -- the read-only
 * view of the boot-latched option-setting words -- and touches no WDT
 * register.
 *
 * These tests exist in their current shape because the previous ones could
 * not fail. `ra8_wdt_regs.h` named OFS0 / OFS3 at 0x03001E04 / 0x03001E20,
 * addresses that appear in neither Hardware User's Manual and that land in a
 * Reserved window, and every assertion still passed: the address helper was
 * compared against the very constant it returned, and each stub reader began
 * `(void)addr`, so the dereference never ran and no test could observe a wrong
 * address (#545).
 *
 * The stub below is therefore address-KEYED. It records every address the
 * driver asks for and raises ::s_ofs_saw_unknown for any address it was not
 * explicitly programmed with, so a constant that drifts fails the suite
 * instead of being quietly serviced.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_ofs.h"
#include "ra8_wdt.h"
#include "ra8_wdt_regs.h"
#include "unity_minimal.h"

/* ---------------------------------------------------------------------------
 * Address-keyed stub reader for the ra8_wdt_ofs_reader_set DI seam
 * ---------------------------------------------------------------------------
 */

/**
 * @struct ofs_cell_t
 * @brief One address-to-word mapping programmed into the stub OFSm reader.
 *
 * @details
 * The stub is deliberately address-KEYED. The previous stubs discarded the
 * address (`(void)addr`), which is why `ra8_wdt_regs.h` could name
 * `0x03001E04` / `0x03001E20` -- addresses in a Reserved window that appear in
 * neither manual -- and still pass every test (#545). A reader that ignores
 * the address cannot detect a wrong address.
 *
 * @invariant `addr` is one of the `ra8_ofs_addr_t` constants.
 * @see ofs_stub_set()
 */
typedef struct {
  uintptr_t addr; /**< Address the driver is expected to request. */
  uint32_t  word; /**< Word handed back for that address.         */
} ofs_cell_t;

/** @brief Capacity of the stub's address map and request log. */
enum : uint8_t {
  k_ofs_cells_max = 4U, /**< OFS0 + OFS3 + OFS3_SEC + OFS3_SEL. */
};

/** @brief Word handed back for an address the stub was never programmed with. */
enum : uint32_t {
  k_ofs_unprogrammed_word = 0xDEADBEEFUL, /**< Deliberately implausible sentinel. */
};

/** @var s_ofs_cells @brief Programmed address-to-word map. @note Test-local. */
static ofs_cell_t s_ofs_cells[k_ofs_cells_max];
/** @var s_ofs_cell_count @brief Number of live entries in ::s_ofs_cells. */
static uint8_t s_ofs_cell_count;
/** @var s_ofs_seen @brief Addresses the driver actually requested, in order. */
static uintptr_t s_ofs_seen[k_ofs_cells_max];
/** @var s_ofs_seen_count @brief Number of live entries in ::s_ofs_seen. */
static uint8_t s_ofs_seen_count;
/** @var s_ofs_saw_unknown @brief Set when an unprogrammed address is read. */
static bool s_ofs_saw_unknown;
/** @var s_ofs_force_err @brief Error to inject, or k_ra8_ok for none. */
static ra8_err_t s_ofs_force_err;
/** @var s_ofs_err_addr @brief Address at which ::s_ofs_force_err is injected. */
static uintptr_t s_ofs_err_addr;

/**
 * @brief Clear the stub reader's map, request log and error injection.
 *
 * @pre None.
 * @post The map is empty and no error is armed.
 * @since 0.1.0
 */
static void ofs_stub_reset(void)
{
  s_ofs_cell_count  = 0U;
  s_ofs_seen_count  = 0U;
  s_ofs_saw_unknown = false;
  s_ofs_force_err   = k_ra8_ok;
  s_ofs_err_addr    = 0U;
}

/**
 * @brief Program one address-to-word mapping into the stub reader.
 *
 * @param[in] addr Address the driver is expected to request.
 * @param[in] word Word to return for it.
 *
 * @pre Fewer than ::k_ofs_cells_max entries are already programmed.
 * @post A read of @p addr yields @p word.
 * @since 0.1.0
 */
static void ofs_stub_set(uintptr_t addr, uint32_t word)
{
  if (s_ofs_cell_count < k_ofs_cells_max) {
    s_ofs_cells[s_ofs_cell_count].addr = addr;
    s_ofs_cells[s_ofs_cell_count].word = word;
    s_ofs_cell_count++;
  }
}

/**
 * @brief Report whether the driver requested a given address.
 *
 * @param[in] addr Address to look for in the request log.
 *
 * @return Whether @p addr appears in ::s_ofs_seen.
 * @retval true  The driver read that address.
 * @retval false It did not.
 *
 * @pre ofs_stub_reset() ran before the call under test.
 * @post No state is modified.
 * @since 0.1.0
 */
static bool ofs_stub_saw(uintptr_t addr)
{
  bool found = false;
  for (uint8_t i = 0U; i < k_ofs_cells_max; i++) {
    if ((i < s_ofs_seen_count) && (s_ofs_seen[i] == addr)) {
      found = true;
    }
  }
  return found;
}

/* The pointer parameters below cannot be const: this mock implements a
 * function-pointer interface (the DI seam under test), so its signature is
 * fixed by the typedef it is assigned to -- adding const changes the
 * function type and the assignment stops compiling. */
// NOLINTNEXTLINE(readability-non-const-parameter)
static ra8_err_t stub_ofs_reader_map(uintptr_t addr, uint32_t* out)
{
  if (s_ofs_seen_count < k_ofs_cells_max) {
    s_ofs_seen[s_ofs_seen_count] = addr;
    s_ofs_seen_count++;
  }
  if ((s_ofs_force_err != k_ra8_ok) && (addr == s_ofs_err_addr)) {
    return s_ofs_force_err;
  }
  for (uint8_t i = 0U; i < k_ofs_cells_max; i++) {
    if ((i < s_ofs_cell_count) && (s_ofs_cells[i].addr == addr)) {
      *out = s_ofs_cells[i].word;
      return k_ra8_ok;
    }
  }
  /* The driver asked for an address this test never programmed -- exactly the
   * #545 defect. Record it so the assertion fails loudly rather than the
   * driver silently receiving a plausible word. */
  s_ofs_saw_unknown = true;
  *out              = k_ofs_unprogrammed_word;
  return k_ra8_ok;
}

/**
 * @brief Program the stub with a WDT0 OFS0 word and install it.
 *
 * @param[in] ofs0 Word to return for ``k_ra8_ofs0_addr``.
 *
 * @pre None.
 * @post The stub reader is active with exactly one mapping.
 * @since 0.1.0
 */
static void ofs_stub_install_wdt0(uint32_t ofs0)
{
  ofs_stub_reset();
  ofs_stub_set((uintptr_t)k_ra8_ofs0_addr, ofs0);
  (void)ra8_wdt_ofs_reader_set(stub_ofs_reader_map);
}

/**
 * @brief Program the stub with the full OFS3 family and install it.
 *
 * @param[in] sel Word returned for ``k_ra8_ofs3_sel_addr``.
 * @param[in] sec Word returned for ``k_ra8_ofs3_sec_addr``.
 * @param[in] ns  Word returned for ``k_ra8_ofs3_addr``.
 *
 * @pre None.
 * @post The stub reader is active with exactly three mappings.
 * @since 0.1.0
 */
static void ofs_stub_install_wdt1(uint32_t sel, uint32_t sec, uint32_t ns)
{
  ofs_stub_reset();
  ofs_stub_set((uintptr_t)k_ra8_ofs3_sel_addr, sel);
  ofs_stub_set((uintptr_t)k_ra8_ofs3_sec_addr, sec);
  ofs_stub_set((uintptr_t)k_ra8_ofs3_addr, ns);
  (void)ra8_wdt_ofs_reader_set(stub_ofs_reader_map);
}

/**
 * @brief Verify ofs_get null out pointer is rejected.
 *
 * @pre None.
 * @post Returns k_ra8_err_null_ptr.
 *
 * @par MC/DC:
 * (null-ptr guard)
 *
 * @since 0.1.0
 */
static void test_wdt_ofs_get_null(void)
{
  TEST_BEGIN("ra8_wdt_ofs_get null out rejected");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wdt_ofs_get((ra8_wdt_instance_t)0U, nullptr));
  TEST_END("ra8_wdt_ofs_get null out rejected");
}

/**
 * @brief Verify ofs_get out-of-range instance is rejected.
 *
 * @pre None.
 * @post Returns k_ra8_err_invalid_arg.
 *
 * @par MC/DC:
 * Decision: ``(uint8_t)which >= k_ra8_wdt_instance_count``
 * - V1: which=0 (valid) -> false -> proceed.
 * - V2: which=count     -> true  -> error.
 *
 * @since 0.1.0
 */
static void test_wdt_ofs_get_oob(void)
{
  TEST_BEGIN("ra8_wdt_ofs_get oob instance rejected");
  ra8_wdt_ofs_decoded_t out = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_wdt_ofs_get((ra8_wdt_instance_t)k_ra8_wdt_instance_count, &out));
  TEST_END("ra8_wdt_ofs_get oob instance rejected");
}

/**
 * @brief Verify WDT0 reads OFS0 and nothing else.
 *
 * @details
 * HUM Ch 7.2.1 p 280 puts WDT0's boot fields in OFS0 at 0x02C9_F040, a single
 * secure-region word with no _SEC / _SEL companions. So exactly one read must
 * occur, at exactly that address. The unknown-address guard is what makes this
 * fail if the constant drifts.
 *
 * @pre Stub reader programmed with OFS0 only.
 * @post ofs_get returns ok having read exactly k_ra8_ofs0_addr once.
 *
 * @par MC/DC:
 * Decision: ``which == k_ra8_wdt0`` (instance dispatch)
 * - V1: which=k_ra8_wdt0 -> true  -> single OFS0 read (this test).
 * - V2: which=k_ra8_wdt1 -> false -> three-word OFS3 path (test below).
 *
 * @since 0.1.0
 */
static void test_wdt_ofs_get_wdt0_reads_ofs0(void)
{
  TEST_BEGIN("ra8_wdt_ofs_get(WDT0) reads OFS0 exactly once");
  ofs_stub_install_wdt0(0x00000000UL);
  ra8_wdt_ofs_decoded_t out = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_ofs_get(k_ra8_wdt0, &out));
  TEST_ASSERT(!s_ofs_saw_unknown);
  TEST_ASSERT(ofs_stub_saw((uintptr_t)k_ra8_ofs0_addr));
  TEST_ASSERT_EQ(1U, s_ofs_seen_count);
  /* Word 0 -> WDT0STRT = 0 -> auto-start. */
  TEST_ASSERT_EQ(k_ra8_wdt_ofs_strt_auto, out.start_mode);
  TEST_ASSERT(out.auto_start);
  (void)ra8_wdt_ofs_reader_set(nullptr);
  TEST_END("ra8_wdt_ofs_get(WDT0) reads OFS0 exactly once");
}

/**
 * @brief Verify WDT1 reads all three OFS3-family words.
 *
 * @details
 * HUM Ch 7.2.7 p 289 makes OFS3_SEL a per-field selector between OFS3_SEC and
 * OFS3, so resolving WDT1 requires all three. Reading fewer would report a
 * configuration the hardware may not be using.
 *
 * @pre Stub reader programmed with the full OFS3 family.
 * @post ofs_get read k_ra8_ofs3_sel_addr, k_ra8_ofs3_sec_addr and
 *       k_ra8_ofs3_addr, and no unprogrammed address.
 *
 * @par MC/DC:
 * Decision: ``which == k_ra8_wdt0`` -- V2 (false arm).
 *
 * @since 0.1.0
 */
static void test_wdt_ofs_get_wdt1_reads_ofs3_family(void)
{
  TEST_BEGIN("ra8_wdt_ofs_get(WDT1) reads OFS3_SEL + OFS3_SEC + OFS3");
  ofs_stub_install_wdt1(0x00000000UL, 0x00000000UL, 0x00000000UL);
  ra8_wdt_ofs_decoded_t out = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_ofs_get(k_ra8_wdt1, &out));
  TEST_ASSERT(!s_ofs_saw_unknown);
  TEST_ASSERT(ofs_stub_saw((uintptr_t)k_ra8_ofs3_sel_addr));
  TEST_ASSERT(ofs_stub_saw((uintptr_t)k_ra8_ofs3_sec_addr));
  TEST_ASSERT(ofs_stub_saw((uintptr_t)k_ra8_ofs3_addr));
  TEST_ASSERT_EQ(3U, s_ofs_seen_count);
  (void)ra8_wdt_ofs_reader_set(nullptr);
  TEST_END("ra8_wdt_ofs_get(WDT1) reads OFS3_SEL + OFS3_SEC + OFS3");
}

/**
 * @brief Verify an all-zero OFS3_SEL takes every field from OFS3_SEC.
 *
 * @details
 * HUM Ch 7.2.7 p 289: selector 0 selects OFS3_SEC. With OFS3_SEC carrying all
 * field bits set and OFS3 carrying none, a correct mux yields the OFS3_SEC
 * pattern -- which a driver that wrongly read OFS3 alone would report as all
 * zeroes.
 *
 * @pre Stub programmed sel=0, sec=field_mask, ns=0.
 * @post Every decoded field carries its OFS3_SEC value.
 *
 * @par MC/DC:
 * (mux is bitwise, not a boolean decision; see the selector-legality vectors.)
 *
 * @since 0.1.0
 */
static void test_wdt_ofs_get_wdt1_sel_zero_takes_sec(void)
{
  TEST_BEGIN("ra8_wdt_ofs_get(WDT1) sel=0 takes OFS3_SEC");
  ofs_stub_install_wdt1(0x00000000UL, (uint32_t)k_ra8_wdt_ofs_field_mask, 0x00000000UL);
  ra8_wdt_ofs_decoded_t out = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_ofs_get(k_ra8_wdt1, &out));
  TEST_ASSERT(!s_ofs_saw_unknown);
  TEST_ASSERT_EQ(k_ra8_wdt_ofs_strt_register, out.start_mode);
  TEST_ASSERT(!out.auto_start);
  TEST_ASSERT_EQ(k_ra8_wdt_ofs_mask_tops, out.cfg.timeout);
  TEST_ASSERT_EQ(k_ra8_wdt_ofs_mask_cks, out.cfg.clock_div);
  (void)ra8_wdt_ofs_reader_set(nullptr);
  TEST_END("ra8_wdt_ofs_get(WDT1) sel=0 takes OFS3_SEC");
}

/**
 * @brief Verify an all-ones OFS3_SEL takes every field from OFS3.
 *
 * @details
 * The mirror of the previous case: selector 1 selects OFS3 (HUM Ch 7.2.7
 * p 289). With OFS3 carrying all field bits set and OFS3_SEC carrying none, a
 * driver that wrongly read OFS3_SEC alone would report all zeroes.
 *
 * @pre Stub programmed sel=field_mask, sec=0, ns=field_mask.
 * @post Every decoded field carries its OFS3 value.
 *
 * @par MC/DC:
 * (mux is bitwise; selector legality is covered separately.)
 *
 * @since 0.1.0
 */
static void test_wdt_ofs_get_wdt1_sel_ones_takes_ofs3(void)
{
  TEST_BEGIN("ra8_wdt_ofs_get(WDT1) sel=1s takes OFS3");
  ofs_stub_install_wdt1((uint32_t)k_ra8_wdt_ofs_field_mask,
                        0x00000000UL,
                        (uint32_t)k_ra8_wdt_ofs_field_mask);
  ra8_wdt_ofs_decoded_t out = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_ofs_get(k_ra8_wdt1, &out));
  TEST_ASSERT(!s_ofs_saw_unknown);
  TEST_ASSERT_EQ(k_ra8_wdt_ofs_strt_register, out.start_mode);
  TEST_ASSERT_EQ(k_ra8_wdt_ofs_mask_cks, out.cfg.clock_div);
  (void)ra8_wdt_ofs_reader_set(nullptr);
  TEST_END("ra8_wdt_ofs_get(WDT1) sel=1s takes OFS3");
}

/**
 * @brief Verify a per-field OFS3_SEL genuinely mixes the two copies.
 *
 * @details
 * The case that distinguishes a real mux from either single-word read. The
 * selector picks OFS3 for TOPS only and OFS3_SEC for everything else, with
 * OFS3_SEC = all field bits and OFS3 = none. A correct resolution therefore
 * yields TOPS = 0 (from OFS3) and every other field saturated (from
 * OFS3_SEC). Reading OFS3 alone would give all zeroes; reading OFS3_SEC alone
 * would give TOPS = 3. Only the mux gives this pattern.
 *
 * @pre Stub programmed sel=TOPS-only, sec=field_mask, ns=0.
 * @post TOPS comes from OFS3; CKS / RPES / RPSS / STRT come from OFS3_SEC.
 *
 * @par MC/DC:
 * (mux is bitwise; the boolean decision under test is selector legality.)
 *
 * @since 0.1.0
 */
static void test_wdt_ofs_get_wdt1_sel_mixed_muxes(void)
{
  TEST_BEGIN("ra8_wdt_ofs_get(WDT1) per-field selector mixes both copies");
  const uint32_t sel_tops_only = (uint32_t)k_ra8_wdt_ofs_mask_tops
                                 << (uint32_t)k_ra8_wdt_ofs_shift_tops;
  ofs_stub_install_wdt1(sel_tops_only, (uint32_t)k_ra8_wdt_ofs_field_mask, 0x00000000UL);
  ra8_wdt_ofs_decoded_t out = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_ofs_get(k_ra8_wdt1, &out));
  TEST_ASSERT(!s_ofs_saw_unknown);
  /* TOPS selected from OFS3 (all zero). */
  TEST_ASSERT_EQ(0U, out.cfg.timeout);
  /* Everything else selected from OFS3_SEC (saturated). */
  TEST_ASSERT_EQ(k_ra8_wdt_ofs_mask_cks, out.cfg.clock_div);
  TEST_ASSERT_EQ(k_ra8_wdt_ofs_mask_rpes, out.cfg.window_end);
  TEST_ASSERT_EQ(k_ra8_wdt_ofs_mask_rpss, out.cfg.window_start);
  TEST_ASSERT_EQ(k_ra8_wdt_ofs_strt_register, out.start_mode);
  (void)ra8_wdt_ofs_reader_set(nullptr);
  TEST_END("ra8_wdt_ofs_get(WDT1) per-field selector mixes both copies");
}

/**
 * @brief Verify prohibited OFS3_SEL encodings are rejected, per field.
 *
 * @details
 * HUM Ch 7.2.7 p 289 marks every mixed multi-bit selector encoding "Setting
 * prohibit" -- the hardware's choice is undefined, so decoding one would
 * report a configuration that may not exist. Each multi-bit field is spoiled
 * in turn to prove it independently forces the rejection.
 *
 * @pre Stub programmed with the full OFS3 family.
 * @post Uniform selectors decode; each mixed field yields
 *       k_ra8_err_invalid_state.
 *
 * @par MC/DC:
 * Decision: `uniform(TOPS) && uniform(CKS) && uniform(RPES) && uniform(RPSS)`
 * cites libs/ra8_hal/src/ra8_wdt.c@internal_ofs3_sel_is_legal
 * (4 conditions) -- N+1 = 5 vectors:
 * - V1: all four uniform          -> true  (control).
 * - V2: TOPS mixed, rest uniform  -> false (varies TOPS only).
 * - V3: CKS mixed, rest uniform   -> false (varies CKS only).
 * - V4: RPES mixed, rest uniform  -> false (varies RPES only).
 * - V5: RPSS mixed, rest uniform  -> false (varies RPSS only).
 * V1 paired with each of V2..V5 proves that condition independently affects
 * the outcome.
 * Decision: `(field == 0U) || (field == mask)`
 * cites libs/ra8_hal/src/ra8_wdt.c@internal_sel_field_uniform
 * (2 conditions) -- N+1 = 3 vectors, supplied by the same call sequence:
 * - V1a: field=0    -> true  (first condition true; control for the second).
 * - V1b: field=mask -> true  (varies the second condition only; V1 sets TOPS
 *        to all-ones while the other three fields are all-zeroes, so one call
 *        takes each true arm).
 * - V2:  field=mixed -> false (both conditions false).
 *
 * @since 0.1.0
 */
static void test_wdt_ofs_get_wdt1_rejects_prohibited_sel(void)
{
  TEST_BEGIN("ra8_wdt_ofs_get(WDT1) rejects prohibited OFS3_SEL encodings");
  ra8_wdt_ofs_decoded_t out = {};

  /* V1 -- control: TOPS all-ones, the other three all-zeroes. All uniform. */
  const uint32_t uniform = (uint32_t)k_ra8_wdt_ofs_mask_tops << (uint32_t)k_ra8_wdt_ofs_shift_tops;
  ofs_stub_install_wdt1(uniform, 0x00000000UL, 0x00000000UL);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_ofs_get(k_ra8_wdt1, &out));

  /* V2..V5 -- spoil exactly one multi-bit field each with a mixed encoding. */
  const uint32_t bad_tops = 1UL << (uint32_t)k_ra8_wdt_ofs_shift_tops;
  ofs_stub_install_wdt1(bad_tops, 0x00000000UL, 0x00000000UL);
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_wdt_ofs_get(k_ra8_wdt1, &out));

  const uint32_t bad_cks = 1UL << (uint32_t)k_ra8_wdt_ofs_shift_cks;
  ofs_stub_install_wdt1(bad_cks, 0x00000000UL, 0x00000000UL);
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_wdt_ofs_get(k_ra8_wdt1, &out));

  const uint32_t bad_rpes = 1UL << (uint32_t)k_ra8_wdt_ofs_shift_rpes;
  ofs_stub_install_wdt1(bad_rpes, 0x00000000UL, 0x00000000UL);
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_wdt_ofs_get(k_ra8_wdt1, &out));

  const uint32_t bad_rpss = 1UL << (uint32_t)k_ra8_wdt_ofs_shift_rpss;
  ofs_stub_install_wdt1(bad_rpss, 0x00000000UL, 0x00000000UL);
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_wdt_ofs_get(k_ra8_wdt1, &out));

  (void)ra8_wdt_ofs_reader_set(nullptr);
  TEST_END("ra8_wdt_ofs_get(WDT1) rejects prohibited OFS3_SEL encodings");
}

/**
 * @brief Verify a reader failure at each OFS3 word propagates out.
 *
 * @details
 * `internal_read_wdt1_word` checks the reader's result after each of its three
 * loads. Injecting the failure at a different address each time reaches all
 * three guards; a fourth case covers the single-read WDT0 path.
 *
 * @pre Stub reader armed to fail at one specific address.
 * @post ofs_get returns the reader's error code unchanged.
 *
 * @par MC/DC:
 * Decision: `e != k_ra8_ok`, evaluated once per read.
 * - V1: reader ok at every address -> false -> decode proceeds (other tests).
 * - V2: fails at OFS3_SEL  -> true at read 1.
 * - V3: fails at OFS3_SEC  -> true at read 2.
 * - V4: fails at OFS3      -> true at read 3.
 * - V5: fails at OFS0      -> true on the WDT0 path.
 *
 * @since 0.1.0
 */
static void test_wdt_ofs_get_propagates_reader_error(void)
{
  TEST_BEGIN("ra8_wdt_ofs_get propagates reader error from every word");
  ra8_wdt_ofs_decoded_t out = {};

  ofs_stub_install_wdt1(0x00000000UL, 0x00000000UL, 0x00000000UL);
  s_ofs_force_err = k_ra8_err_hw_error;
  s_ofs_err_addr  = (uintptr_t)k_ra8_ofs3_sel_addr;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_wdt_ofs_get(k_ra8_wdt1, &out));

  ofs_stub_install_wdt1(0x00000000UL, 0x00000000UL, 0x00000000UL);
  s_ofs_force_err = k_ra8_err_hw_error;
  s_ofs_err_addr  = (uintptr_t)k_ra8_ofs3_sec_addr;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_wdt_ofs_get(k_ra8_wdt1, &out));

  ofs_stub_install_wdt1(0x00000000UL, 0x00000000UL, 0x00000000UL);
  s_ofs_force_err = k_ra8_err_hw_error;
  s_ofs_err_addr  = (uintptr_t)k_ra8_ofs3_addr;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_wdt_ofs_get(k_ra8_wdt1, &out));

  ofs_stub_install_wdt0(0x00000000UL);
  s_ofs_force_err = k_ra8_err_hw_error;
  s_ofs_err_addr  = (uintptr_t)k_ra8_ofs0_addr;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_wdt_ofs_get(k_ra8_wdt0, &out));

  (void)ra8_wdt_ofs_reader_set(nullptr);
  TEST_END("ra8_wdt_ofs_get propagates reader error from every word");
}

/**
 * @brief Verify the register-start encoding decodes to auto_start = false.
 *
 * @details
 * Complements the auto-start assertion in the WDT0 test: WDT0STRT = 1 selects
 * register-start mode (HUM Ch 7.2.1 p 280), in which the WDTCR / WDTRCR /
 * WDTCSTPR registers govern instead of OFS0.
 *
 * @pre Stub returns an OFS0 word with WDT0STRT set.
 * @post out.start_mode is register-start and out.auto_start is false.
 *
 * @par MC/DC:
 * Decision: ``out->start_mode == k_ra8_wdt_ofs_strt_auto``
 * - V1: strt=0 -> true  -> auto_start = true (WDT0 test above).
 * - V2: strt=1 -> false -> auto_start = false (this test).
 *
 * @since 0.1.0
 */
static void test_wdt_ofs_get_register_start(void)
{
  TEST_BEGIN("ra8_wdt_ofs_get decodes register-start correctly");
  const uint32_t strt_set = (uint32_t)k_ra8_wdt_ofs_mask_strt << (uint32_t)k_ra8_wdt_ofs_shift_strt;
  ofs_stub_install_wdt0(strt_set);
  ra8_wdt_ofs_decoded_t out = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_ofs_get(k_ra8_wdt0, &out));
  TEST_ASSERT_EQ(k_ra8_wdt_ofs_strt_register, out.start_mode);
  TEST_ASSERT(!out.auto_start);
  (void)ra8_wdt_ofs_reader_set(nullptr);
  TEST_END("ra8_wdt_ofs_get decodes register-start correctly");
}

/**
 * @var s_test_roster
 * @brief Fixed-order roster of every test case in this translation unit.
 *
 * @details
 * main() walks this table instead of naming each case, so its size does not
 * grow with the number of tests and adding a case is a one-line edit.
 *
 * @note Read-only after static initialisation.
 * @since 0.1.0
 */
static void (*const s_test_roster[])(void) = {
  test_wdt_ofs_get_null,
  test_wdt_ofs_get_oob,
  test_wdt_ofs_get_wdt0_reads_ofs0,
  test_wdt_ofs_get_wdt1_reads_ofs3_family,
  test_wdt_ofs_get_wdt1_sel_zero_takes_sec,
  test_wdt_ofs_get_wdt1_sel_ones_takes_ofs3,
  test_wdt_ofs_get_wdt1_sel_mixed_muxes,
  test_wdt_ofs_get_wdt1_rejects_prohibited_sel,
  test_wdt_ofs_get_propagates_reader_error,
  test_wdt_ofs_get_register_start,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK  ] test_ra8_wdt_ofs.c\n");
  return 0;
}
