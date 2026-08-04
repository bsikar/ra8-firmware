/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_cnecc_cov.c
 * @brief Coverage-closure unit tests for ra8_cnecc.c (CANFD ECC driver)
 *
 * @details
 * Companion to ``test_ra8_cnecc.c``. Drives the handful of branches the
 * primary suite leaves cold:
 *
 *  - The BBR-mirror zeroing inside ``internal_apply_instance`` that only
 *    runs when a counter mirror is attached and the driver replays the
 *    cached config on Software-Standby exit (HUM 42.5.2 p 2876).
 *  - The ``ra8_cnecc_attach_isr`` rollback loop that unregisters the
 *    already-opened CAN0 slot when the CAN1 registration is rejected.
 *  - The software CRC32 fall-back reachable through ``ra8_cnecc_open`` /
 *    ``ra8_cnecc_compute`` / ``ra8_cnecc_verify``.
 *
 * The compute / verify cases feed addresses that live inside the
 * fake's mapped SRAM window (base ``0x22000000``) rather than a
 * static test-binary buffer: the driver takes a ``uint32_t addr`` and
 * casts it back to a pointer, so a real 64-bit host pointer would be
 * truncated. The mapped SRAM window is sub-4-GiB and is zeroed by
 * ``ra8_fake_mmap_reset``, so the round trip is exact on both the target
 * and the x86_64 test host.
 */

#include "ra8_cnecc.h"
#include "ra8_cnecc_regs.h"
#include "ra8_elc_regs.h"
#include "ra8_err.h"
#include "ra8_fake_irq.h"
#include "ra8_fake_mmap.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

/**
 * @enum ra8_cnecc_cov_const_t
 * @brief Magic-number-free constants used by the coverage test bodies.
 */
typedef enum : uint16_t {
  k_ra8_cnecc_cov_inst_first = 0U,    /**< ECCMB0.                            */
  k_ra8_cnecc_cov_inst_last  = 1U,    /**< ECCMB1.                            */
  k_ra8_cnecc_cov_addr_a     = 0x11U, /**< Sample fault address (a).          */
  k_ra8_cnecc_cov_addr_b     = 0x22U, /**< Sample fault address (b).          */
  k_ra8_cnecc_cov_prio       = 5U,    /**< Sample NVIC priority for ISR wire. */
} ra8_cnecc_cov_const_t;

/**
 * @enum ra8_cnecc_cov_ram_t
 * @brief Fake-mapped SRAM window used as a compute / verify source.
 *
 * @details
 * ``0x22000000`` is region ``k_ra8_fake_region_sram`` in ra8_fake_mmap.c
 * (2 MiB, zeroed by ``ra8_fake_mmap_reset``). It fits in a ``uint32_t``,
 * so the driver's ``(const uint8_t*)(uintptr_t)addr`` round trip is
 * lossless on the 64-bit test host.
 */
typedef enum : uint32_t {
  k_ra8_cnecc_cov_ram_base = 0x22000000UL, /**< Fake SRAM window base.      */
  k_ra8_cnecc_cov_ram_len  = 16U,          /**< Bytes hashed by compute.    */
  k_ra8_cnecc_cov_word0    = 0xDEADBEEFUL, /**< Seed word 0 of the source.  */
  k_ra8_cnecc_cov_word1    = 0xCAFEBABEUL, /**< Seed word 1 of the source.  */
  k_ra8_cnecc_cov_word2    = 0x12345678UL, /**< Seed word 2 of the source.  */
  k_ra8_cnecc_cov_word3    = 0x9ABCDEF0UL, /**< Seed word 3 of the source.  */
  k_ra8_cnecc_cov_ecc_flip = 0x000000FFUL, /**< XOR delta to break a match. */
} ra8_cnecc_cov_ram_t;

static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_isr_init();
}

static ra8_cnecc_config_t make_default_cfg(void)
{
  const ra8_cnecc_config_t cfg = {
    .instances =
      {
        [0] = {.correct_1bit = true, .irq_1bit = true, .irq_2bit = true, .enable = true},
        [1] = {.correct_1bit = true, .irq_1bit = true, .irq_2bit = true, .enable = true},
      },
  };
  return cfg;
}

/** @brief No-op ISR used only to pre-occupy an ELC event slot. */
static void cov_dummy_isr(void* ctx)
{
  (void)ctx;
}

/** @brief Write the fixed 16-byte seed pattern into the fake SRAM source. */
static void cov_seed_source(void)
{
  volatile uint32_t* words = (volatile uint32_t*)(uintptr_t)k_ra8_cnecc_cov_ram_base;
  words[0]                 = (uint32_t)k_ra8_cnecc_cov_word0;
  words[1]                 = (uint32_t)k_ra8_cnecc_cov_word1;
  words[2]                 = (uint32_t)k_ra8_cnecc_cov_word2;
  words[3]                 = (uint32_t)k_ra8_cnecc_cov_word3;
}

/**
 * @par MC/DC:
 * (no compound decisions -- exercises the ``ra8_cnecc_open`` default
 * bring-up wrapper; no `&&` or `||` in the code under test that this
 * case touches)
 */
static void test_open_arms_both_instances(void)
{
  TEST_BEGIN("cnecc open brings up both instances with defaults");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_open());
  for (uint8_t i = (uint8_t)k_ra8_cnecc_cov_inst_first; i <= (uint8_t)k_ra8_cnecc_cov_inst_last;
       ++i) {
    volatile r_cnecc_regs_t* reg = ra8_cnecc(i);
    TEST_ASSERT_NOT_NULL((void*)reg);
    /* Default config sets ECERVF + both IRQ enables, clears EC1ECP. */
    TEST_ASSERT((reg->EC710CTL & (uint32_t)k_ra8_cnecc_mask_ecervf) != 0U);
    TEST_ASSERT((reg->EC710CTL & (uint32_t)k_ra8_cnecc_mask_ec1edic) != 0U);
    TEST_ASSERT((reg->EC710CTL & (uint32_t)k_ra8_cnecc_mask_ec2edic) != 0U);
    TEST_ASSERT_EQ(0, (reg->EC710CTL & (uint32_t)k_ra8_cnecc_mask_ec1ecp));
  }
  TEST_END("cnecc open brings up both instances with defaults");
}

/**
 * @par MC/DC:
 * Decision: the inner ``(crc & 1UL) != 0UL ? poly : 0`` selector in
 * ``internal_crc32`` (1 condition).
 * - Vector 1: a word whose reflected shift leaves LSB=1 -> poly XORed in.
 * - Vector 2: a word whose reflected shift leaves LSB=0 -> 0 XORed in.
 * Both outcomes occur within the 8-bit inner loop over the seeded
 * 16-byte pattern, so a single compute pass exercises both legs.
 * The four leading ``if`` guards in ``ra8_cnecc_compute`` are all taken
 * false here (valid, aligned, in-range args); their true legs are
 * covered by ``test_compute_rejects_bad_args``.
 */
static void test_compute_is_deterministic(void)
{
  TEST_BEGIN("cnecc compute returns a stable CRC over mapped SRAM");
  prep();
  cov_seed_source();

  uint32_t ecc_a = 0U;
  uint32_t ecc_b = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_cnecc_compute((uint32_t)k_ra8_cnecc_cov_ram_base,
                                   (uint32_t)k_ra8_cnecc_cov_ram_len,
                                   &ecc_a));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_cnecc_compute((uint32_t)k_ra8_cnecc_cov_ram_base,
                                   (uint32_t)k_ra8_cnecc_cov_ram_len,
                                   &ecc_b));
  /* Same bytes -> identical tag (the accumulator is a pure function). */
  TEST_ASSERT(ecc_a == ecc_b);
  /* A CRC32 with XOR-out 0xFFFFFFFF over non-trivial data is not 0. */
  TEST_ASSERT(ecc_a != 0U);
  TEST_END("cnecc compute returns a stable CRC over mapped SRAM");
}

/**
 * @par MC/DC:
 * Four independent single-condition guards in ``ra8_cnecc_compute``:
 * - ``out_ecc == nullptr``   true  -> k_ra8_err_null_ptr.
 * - ``addr == 0``            true  -> k_ra8_err_null_ptr.
 * - ``addr % 4 != 0``        true  -> k_ra8_err_invalid_arg.
 * - ``len < 4``              true  -> k_ra8_err_invalid_arg.
 * Each sub-case isolates one guard by keeping the earlier guards false,
 * proving each condition independently drives the reject outcome.
 */
static void test_compute_rejects_bad_args(void)
{
  TEST_BEGIN("cnecc compute rejects null/zero/misaligned/short args");
  prep();
  uint32_t ecc = 0U;

  /* Guard 1: null receiver (earlier guards N/A). */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_cnecc_compute((uint32_t)k_ra8_cnecc_cov_ram_base,
                                   (uint32_t)k_ra8_cnecc_cov_ram_len,
                                   nullptr));
  /* Guard 2: zero base address (receiver valid). */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_cnecc_compute(0U, (uint32_t)k_ra8_cnecc_cov_ram_len, &ecc));
  /* Guard 3: misaligned base (non-zero, receiver valid; never deref). */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_cnecc_compute((uint32_t)k_ra8_cnecc_cov_ram_base + 1U,
                                   (uint32_t)k_ra8_cnecc_cov_ram_len,
                                   &ecc));
  /* Guard 4: length under one word (aligned, non-zero, valid receiver). */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_cnecc_compute((uint32_t)k_ra8_cnecc_cov_ram_base, 2U, &ecc));
  TEST_END("cnecc compute rejects null/zero/misaligned/short args");
}

/**
 * @par MC/DC:
 * Decision: ``if (got != expected_ecc)`` in ``ra8_cnecc_verify`` (1
 * condition).
 * - Vector 1: got == expected -> false -> k_ra8_ok.
 * - Vector 2: got != expected -> true  -> k_ra8_err_crc_mismatch.
 * The two vectors flip only the expected value, proving the comparison
 * independently drives the result. The ``RA8_RETURN_ON_ERROR`` compute
 * guard stays on its success leg (valid args) throughout.
 */
static void test_verify_match_then_mismatch(void)
{
  TEST_BEGIN("cnecc verify accepts the computed tag and rejects a flipped one");
  prep();
  cov_seed_source();

  uint32_t ecc = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_cnecc_compute((uint32_t)k_ra8_cnecc_cov_ram_base, (uint32_t)k_ra8_cnecc_cov_ram_len, &ecc));
  /* Round-trip: verify against the freshly computed tag -> match. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_cnecc_verify((uint32_t)k_ra8_cnecc_cov_ram_base, (uint32_t)k_ra8_cnecc_cov_ram_len, ecc));
  /* Flip the low byte of the expected tag -> guaranteed mismatch. */
  TEST_ASSERT_EQ(k_ra8_err_crc_mismatch,
                 ra8_cnecc_verify((uint32_t)k_ra8_cnecc_cov_ram_base,
                                  (uint32_t)k_ra8_cnecc_cov_ram_len,
                                  ecc ^ (uint32_t)k_ra8_cnecc_cov_ecc_flip));
  TEST_END("cnecc verify accepts the computed tag and rejects a flipped one");
}

/**
 * @par MC/DC:
 * Decision (per instance in ``internal_apply_instance``):
 * ``if (s_cnecc_bbr_mirror[instance] != nullptr)`` (1 condition).
 * - Vector 1 (both instances): mirror attached -> true  -> mirror zeroed.
 * - The false leg is already covered by the primary suite's mirror-less
 *   init. Here both instances carry a mirror, so the standby-exit replay
 *   drives the true leg and zeroes the previously non-zero mirrors.
 */
static void test_exit_standby_zeroes_attached_mirror(void)
{
  TEST_BEGIN("cnecc exit_standby zeroes an attached BBR counter mirror");
  prep();

  const ra8_cnecc_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_init(&cfg));

  ra8_cnecc_counters_t mirror0 = {};
  ra8_cnecc_counters_t mirror1 = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_cnecc_set_counter_mirror((uint8_t)k_ra8_cnecc_cov_inst_first, &mirror0));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_cnecc_set_counter_mirror((uint8_t)k_ra8_cnecc_cov_inst_last, &mirror1));

  /* Drive faults so the mirrors hold non-zero counts before standby. */
  ra8_cnecc_dispatch((uint8_t)k_ra8_cnecc_cov_inst_first, false, (uint16_t)k_ra8_cnecc_cov_addr_a);
  ra8_cnecc_dispatch((uint8_t)k_ra8_cnecc_cov_inst_first, true, (uint16_t)k_ra8_cnecc_cov_addr_a);
  ra8_cnecc_dispatch_overflow((uint8_t)k_ra8_cnecc_cov_inst_first);
  ra8_cnecc_dispatch((uint8_t)k_ra8_cnecc_cov_inst_last, true, (uint16_t)k_ra8_cnecc_cov_addr_b);
  TEST_ASSERT_EQ(1, mirror0.one_bit_count);
  TEST_ASSERT_EQ(1, mirror0.two_bit_count);
  TEST_ASSERT_EQ(1, mirror0.overflow_count);
  TEST_ASSERT_EQ(1, mirror1.two_bit_count);

  /* Standby exit replays the cached cfg; the apply path zeroes the
   * driver-local counters AND every attached mirror. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_enter_standby());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_exit_standby());

  TEST_ASSERT_EQ(0, mirror0.one_bit_count);
  TEST_ASSERT_EQ(0, mirror0.two_bit_count);
  TEST_ASSERT_EQ(0, mirror0.overflow_count);
  TEST_ASSERT_EQ(0, mirror1.two_bit_count);

  /* Detach so the stack-local mirrors never outlive this frame. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_cnecc_set_counter_mirror((uint8_t)k_ra8_cnecc_cov_inst_first, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_cnecc_set_counter_mirror((uint8_t)k_ra8_cnecc_cov_inst_last, nullptr));
  TEST_END("cnecc exit_standby zeroes an attached BBR counter mirror");
}

/**
 * @par MC/DC:
 * Decision: ``if (err != k_ra8_ok)`` after the per-instance
 * ``ra8_isr_register`` in ``ra8_cnecc_attach_isr`` (1 condition).
 * - The primary suite covers the false leg (both slots wired).
 * - Here CAN1_MRAM_ERI is pre-occupied so the second register returns
 *   ``k_ra8_err_exists`` -> true leg -> the rollback loop runs for the
 *   already-opened CAN0 slot and the error propagates out.
 * The rollback ``for (j = 0; j < i; ++j)`` executes exactly once (i==1),
 * so the CAN0 slot is released while the pre-existing CAN1 slot stays.
 */
static void test_attach_isr_rolls_back_on_second_failure(void)
{
  TEST_BEGIN("cnecc attach_isr rolls back CAN0 when CAN1 registration fails");
  prep();

  /* Pre-occupy the CAN1 event so attach_isr's second register fails. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_isr_register(k_ra8_elc_event_can1_mram_eri,
                                  cov_dummy_isr,
                                  nullptr,
                                  (uint8_t)k_ra8_cnecc_cov_prio,
                                  nullptr));

  /* attach_isr wires CAN0 (ok), then CAN1 (exists) -> rollback path. */
  TEST_ASSERT_EQ(k_ra8_err_exists, ra8_cnecc_attach_isr((uint8_t)k_ra8_cnecc_cov_prio));

  /* CAN0 must have been unregistered by the rollback loop. */
  uint16_t slot_can0 = (uint16_t)k_ra8_isr_slot_none;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_isr_lookup_slot(k_ra8_elc_event_can0_mram_eri, &slot_can0));
  TEST_ASSERT_EQ(k_ra8_isr_slot_none, slot_can0);

  /* The pre-existing CAN1 slot is untouched (still owned by the stub). */
  uint16_t slot_can1 = (uint16_t)k_ra8_isr_slot_none;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_isr_lookup_slot(k_ra8_elc_event_can1_mram_eri, &slot_can1));
  TEST_ASSERT(slot_can1 != (uint16_t)k_ra8_isr_slot_none);

  /* Clean up the stub registration. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_isr_unregister(k_ra8_elc_event_can1_mram_eri));
  TEST_END("cnecc attach_isr rolls back CAN0 when CAN1 registration fails");
}

int32_t main(void)
{
  test_open_arms_both_instances();
  test_compute_is_deterministic();
  test_compute_rejects_bad_args();
  test_verify_match_then_mismatch();
  test_exit_standby_zeroes_attached_mirror();
  test_attach_isr_rolls_back_on_second_failure();
  (void)fprintf(stderr, "[OK  ] test_ra8_cnecc_cov.c\n");
  return 0;
}
