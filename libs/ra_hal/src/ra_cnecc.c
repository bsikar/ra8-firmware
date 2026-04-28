/**
 * @file ra_cnecc.c
 * @brief CANFD ECC (CNECC) driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Driver for the RA8D2 CANFD message-buffer SRAM ECC block (HUM
 * Ch 42 "CANFD ECC (CNECC)", p 2868-2876). Two instances
 * (``ECCMB0`` for ``CAN0``, ``ECCMB1`` for ``CAN1``) share the same
 * register map. The block sits behind the same module-stop bit as
 * the CANFD it monitors, so init walks ``ra_mstp_enable`` for both
 * ``CANFDn`` ids before it touches any CNECC register. Every
 * register access carries a HUM Ch 42 citation.
 *
 * ## Coverage
 *
 * Every ``EC710CTL`` field (HUM 42.2.1 p 2868-2871), the
 * ``EC710TMC`` test-mode register (HUM 42.2.2 p 2871-2872), the
 * ``EC710TED`` substitute-data register (HUM 42.2.3 p 2872), and
 * the ``EC710EAD0`` captured-address register (HUM 42.2.4
 * p 2872-2873) are exercised. The driver also implements the
 * Software Standby enter / exit flows from HUM 42.5.1 / 42.5.2
 * p 2876.
 *
 * ## State machine
 *
 * @startuml
 * [*] --> Uninit
 * Uninit --> Configured : ra_cnecc_init
 * Configured --> Active : ra_cnecc_enable_instance
 * Active --> Configured : ra_cnecc_disable_instance
 * Configured --> TestMode : ra_cnecc_inject_fault
 * TestMode --> Configured : ra_cnecc_test_mode_disable
 * Active --> Standby : ra_cnecc_enter_standby
 * Standby --> Active : ra_cnecc_exit_standby
 * Configured --> Uninit : ra_cnecc_deinit
 * Active --> Uninit : ra_cnecc_deinit
 * @enduml
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_cnecc.h"

#include <stdint.h>

#include "ra8d2_cnecc_regs.h"
#include "ra8d2_elc_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_isr.h"
#include "ra_log.h"
#include "ra_mstp.h"

/**
 * @var s_tag
 * @brief Logging tag.
 * @note  Static, file-local; never modified after initialisation.
 */
static const char* s_tag = "CNECC";

/**
 * @var s_cnecc_mstp_table
 * @brief Per-instance MSTP id used to ungate the parent CANFD block.
 *
 * @details
 * HUM 11.2.8 p 447 places ``MSTPC26`` over CANFD1 and ``MSTPC27``
 * over CANFD0 (HUM 42.1 overview p 2868 also notes the ECC sits
 * inside the CANFD module so it shares the parent gate).
 */
static const ra_mstp_t s_cnecc_mstp_table[k_ra_cnecc_instance_count] = {
  k_ra_mstp_canfd0,
  k_ra_mstp_canfd1,
};

/**
 * @var s_cnecc_event_table
 * @brief Per-instance ICU event for the shared CANn_MRAM_ERI vector.
 *
 * @details
 * HUM 42.4 p 2875 lists ``CAN0_MRAM_ERI`` for instance 0 and
 * ``CAN1_MRAM_ERI`` for instance 1. The ELC event numbers themselves
 * live in ``ra8d2_elc_regs.h``.
 */
static const ra_elc_event_t s_cnecc_event_table[k_ra_cnecc_instance_count] = {
  k_ra_elc_event_can0_mram_eri,
  k_ra_elc_event_can1_mram_eri,
};

/**
 * @var s_cnecc_fn
 * @brief Currently registered fault callback.
 * @note  Updated only by ``ra_cnecc_attach_handler``.
 */
static ra_cnecc_error_fn_t s_cnecc_fn;

/**
 * @var s_cnecc_ctx
 * @brief Opaque context passed to ``s_cnecc_fn``.
 */
static void* s_cnecc_ctx;

/**
 * @var s_cnecc_one_bit_count
 * @brief Cumulative 1-bit fault counter, per instance.
 */
static uint32_t s_cnecc_one_bit_count[k_ra_cnecc_instance_count];

/**
 * @var s_cnecc_two_bit_count
 * @brief Cumulative 2-bit fault counter, per instance.
 */
static uint32_t s_cnecc_two_bit_count[k_ra_cnecc_instance_count];

/**
 * @var s_cnecc_overflow_count
 * @brief Cumulative ECOVFF counter, per instance.
 */
static uint32_t s_cnecc_overflow_count[k_ra_cnecc_instance_count];

/**
 * @var s_cnecc_bbr_mirror
 * @brief Optional BBR-mirrored counter pointer, per instance.
 *
 * @details
 * If non-NULL, every increment to the driver-local counters is
 * mirrored into the caller-supplied storage (typically a
 * battery-backed VBTBKRn slot). Mirror lifetime is the caller's
 * responsibility; see HUM Ch 12.2.7 p 505 for the BBR window the
 * test pattern uses.
 */
static ra_cnecc_counters_t* s_cnecc_bbr_mirror[k_ra_cnecc_instance_count];

/**
 * @var s_cnecc_cached_cfg
 * @brief Last-applied configuration -- replayed by ``exit_standby``.
 */
static ra_cnecc_config_t s_cnecc_cached_cfg;

/**
 * @var s_cnecc_initialised
 * @brief True between successful ``init`` and matching ``deinit``.
 */
static bool s_cnecc_initialised;

/**
 * @var s_cnecc_isr_attached
 * @brief True when ``ra_cnecc_attach_isr`` has wired the ICU vectors.
 */
static bool s_cnecc_isr_attached;

/**
 * @enum ra_cnecc_isr_const_t
 * @brief Numeric constants for ISR ctx packing.
 */
typedef enum : uint16_t {
  k_ra_cnecc_isr_ctx_inst_mask = 0x00FFU, /**< Low byte of ctx -> instance.   */
} ra_cnecc_isr_const_t;

/**
 * @brief Apply one instance's configuration.
 *
 * @details
 * Walks the HUM 42.3.1 figure-42.1 procedure (p 2874): ungate the
 * parent CANFD module, clear any latched fault state, set the IRQ
 * enables / correction permission, then unlock and write
 * ``ECERVF`` per ``cfg->enable``.
 *
 * @param[in] instance Instance index already bounded.
 * @param[in] cfg      Per-instance settings.
 *
 * @return ``k_ra_ok`` on success, propagated MSTP error otherwise.
 *
 * @pre  ``instance`` < ``k_ra_cnecc_instance_count``.
 * @pre  ``cfg`` is non-NULL.
 * @post On success the instance's ``EC710CTL`` matches ``cfg``.
 * @post Driver-local counters for the instance are zeroed.
 */
static ra_err_t internal_apply_instance(uint8_t instance, const ra_cnecc_instance_cfg_t* cfg)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 447 */
  const ra_err_t mst_err = ra_mstp_enable(s_cnecc_mstp_table[instance]);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "cnecc_init: mstp enable");

  volatile r_cnecc_regs_t* reg = ra_cnecc(instance);
  if (reg == nullptr) {             /* GCOVR_EXCL_BR_LINE -- bounded by caller. */
    return k_ra_err_hw_init_failed; /* GCOVR_EXCL_LINE */
  }

  /* HUM Ch 42.2.1 "EC710CTL : ECC Control Register", p 2868
   * Step 1: clear any latched 1-bit / 2-bit / overflow / address
   * flags before turning judgment on, so we start from a clean
   * slate (HUM 42.2.1 p 2870 ECER1C/ECER2C clearing notes). */
  reg->EC710CTL = (uint32_t)k_ra_cnecc_mask_clear_all;

  /* HUM Ch 42.2.1 "EC710CTL : ECC Control Register", p 2868
   * Step 2: program IRQ enables + correction permission. The
   * ``EC1ECP`` bit is "correction NOT executed" when SET, so the
   * caller's "correct_1bit = true" maps to bit cleared. */
  uint32_t ctl = 0U;
  if (cfg->irq_1bit) {
    ctl |= (uint32_t)k_ra_cnecc_mask_ec1edic;
  }
  if (cfg->irq_2bit) {
    ctl |= (uint32_t)k_ra_cnecc_mask_ec2edic;
  }
  if (!cfg->correct_1bit) {
    ctl |= (uint32_t)k_ra_cnecc_mask_ec1ecp;
  }

  /* HUM Ch 42.2.1 "EC710CTL : ECC Control Register" EMCA notes,
   * p 2870: writes to ``ECERVF`` are ignored unless ``EMCA[1:0]``
   * is ``01b`` in the same write. Combine the unlock pattern with
   * the ECERVF bit (or omit it for "configured but not running"). */
  ctl |= (uint32_t)k_ra_cnecc_mask_emca_unlock;
  if (cfg->enable) {
    ctl |= (uint32_t)k_ra_cnecc_mask_ecervf;
  }

  /* HUM Ch 42.2.1 "EC710CTL : ECC Control Register", p 2868 */
  reg->EC710CTL = ctl;

  /* Always leave fault-injection mode disabled at init -- the test
   * register defaults to 0 anyway, but a previous test run could
   * have left ECTMCE set (HUM Ch 42.2.2 "EC710TMC : ECC Test Mode
   * Control Register", p 2871). */
  reg->EC710TMC = (uint16_t)k_ra_cnecc_mask_test_disable;

  s_cnecc_one_bit_count[instance]  = 0U;
  s_cnecc_two_bit_count[instance]  = 0U;
  s_cnecc_overflow_count[instance] = 0U;
  if (s_cnecc_bbr_mirror[instance] != nullptr) {
    s_cnecc_bbr_mirror[instance]->one_bit_count  = 0U;
    s_cnecc_bbr_mirror[instance]->two_bit_count  = 0U;
    s_cnecc_bbr_mirror[instance]->overflow_count = 0U;
  }
  return k_ra_ok;
}

/**
 * @brief Read-modify-write helper for ``EC710CTL`` that preserves the
 *        unlock pattern and clears RO/clear-on-write bits.
 *
 * @details
 * HUM 42.2.1 p 2870 EMCA[1:0] = 01b is required on every write that
 * may touch ECERVF; rather than tracking which writes do, the driver
 * always combines the unlock pattern. ECER1C / ECER2C are W1C, so we
 * mask them to 0 in the rewrite to avoid an accidental clear, and
 * we blank the read-only flag bits before OR-ing in the new value.
 *
 * @param[in] reg      Live register pointer (non-NULL).
 * @param[in] new_bits Bits to OR into the writable subset.
 * @param[in] mask     Subset of ``k_ra_cnecc_mask_ctl_writable`` to
 *                     replace; bits in ``mask`` are first cleared,
 *                     then ``new_bits & mask`` is OR-ed in.
 *
 * @pre  reg != nullptr.
 * @pre  new_bits & ~mask == 0 (caller's responsibility).
 * @post EMCA bits in the register are 01b on the write.
 * @post Writable bits outside ``mask`` are preserved.
 */
static void internal_ctl_rmw(volatile r_cnecc_regs_t* reg, uint32_t new_bits, uint32_t mask)
{
  /* HUM Ch 42.2.1 "EC710CTL : ECC Control Register", p 2868 */
  uint32_t live = reg->EC710CTL;
  /* Drop status / W1C bits (we never want to accidentally clear them
   * on a config rewrite -- callers go through ra_cnecc_clear_status
   * for that). */
  live &= (uint32_t)k_ra_cnecc_mask_ctl_writable;
  /* Drop the W1C clear bits -- they read 0 but writing 1 latches a
   * clear; we want this RMW to be neutral on the status flags. */
  live &= ~((uint32_t)k_ra_cnecc_mask_ecer1c | (uint32_t)k_ra_cnecc_mask_ecer2c);
  /* Replace just the requested bits. */
  live = ((live & ~mask) | (new_bits & mask));
  /* Always re-assert the EMCA = 01b unlock pattern (HUM 42.2.1 p 2870
   * EMCA notes). */
  live = ((live & ~(uint32_t)k_ra_cnecc_mask_emca) | (uint32_t)k_ra_cnecc_mask_emca_unlock);
  /* HUM Ch 42.2.1 "EC710CTL : ECC Control Register", p 2868 */
  reg->EC710CTL = live;
}

[[nodiscard]] ra_err_t ra_cnecc_init(const ra_cnecc_config_t* cfg)
{
  RA_CHECK_NULL_PTR((void*)cfg, s_tag, "cfg must not be nullptr");

  for (uint8_t i = 0U; i < (uint8_t)k_ra_cnecc_instance_count; ++i) {
    const ra_err_t err = internal_apply_instance(i, &cfg->instances[i]);
    RA_RETURN_ON_ERROR(err, s_tag, "cnecc_init apply");
  }
  s_cnecc_cached_cfg  = *cfg;
  s_cnecc_initialised = true;
  ra_log_info(s_tag, "cnecc_init");
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_cnecc_deinit(void)
{
  /* If the ISR was attached, drop those slots first so they don't
   * fire after the MSTP gate closes. */
  if (s_cnecc_isr_attached) {
    (void)ra_cnecc_detach_isr();
  }

  for (uint8_t i = 0U; i < (uint8_t)k_ra_cnecc_instance_count; ++i) {
    volatile r_cnecc_regs_t* reg = ra_cnecc(i);
    if (reg != nullptr) {
      /* HUM Ch 42.2.2 "EC710TMC : ECC Test Mode Control Register", p 2871
       * Always leave test mode off when tearing down. */
      reg->EC710TMC = (uint16_t)k_ra_cnecc_mask_test_disable;
      /* HUM Ch 42.2.1 "EC710CTL : ECC Control Register" EMCA notes,
       * p 2870: clearing ``ECERVF`` also requires the EMCA = 01b
       * unlock pattern in the same write. */
      reg->EC710CTL = (uint32_t)k_ra_cnecc_mask_emca_unlock;
    }
    /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 447 */
    (void)ra_mstp_disable(s_cnecc_mstp_table[i]);
    s_cnecc_one_bit_count[i]  = 0U;
    s_cnecc_two_bit_count[i]  = 0U;
    s_cnecc_overflow_count[i] = 0U;
  }
  s_cnecc_initialised = false;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_cnecc_enable_instance(uint8_t instance)
{
  if ((uint16_t)instance >= (uint16_t)k_ra_cnecc_instance_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_cnecc_regs_t* reg = ra_cnecc(instance);
  RA_CHECK_NULL_PTR((void*)reg, s_tag, "instance mapping failed");
  /* HUM Ch 42.2.1 "EC710CTL : ECC Control Register", p 2870 */
  internal_ctl_rmw(reg, (uint32_t)k_ra_cnecc_mask_ecervf, (uint32_t)k_ra_cnecc_mask_ecervf);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_cnecc_disable_instance(uint8_t instance)
{
  if ((uint16_t)instance >= (uint16_t)k_ra_cnecc_instance_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_cnecc_regs_t* reg = ra_cnecc(instance);
  RA_CHECK_NULL_PTR((void*)reg, s_tag, "instance mapping failed");
  /* HUM Ch 42.2.1 "EC710CTL : ECC Control Register", p 2870 */
  internal_ctl_rmw(reg, 0U, (uint32_t)k_ra_cnecc_mask_ecervf);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_cnecc_enter_standby(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra_cnecc_instance_count; ++i) {
    volatile r_cnecc_regs_t* reg = ra_cnecc(i);
    if (reg == nullptr) {
      continue; /* GCOVR_EXCL_LINE -- bounded loop, ra_cnecc never NULL here. */
    }
    /* HUM Ch 42.5.1 "Enter Software Standby Mode", p 2876
     * Step 1: clear ECC error flags + address. */
    reg->EC710CTL = (uint32_t)k_ra_cnecc_mask_clear_all;
    /* Step 2: clear ECERVF (with EMCA unlock). */
    reg->EC710CTL = (uint32_t)k_ra_cnecc_mask_emca_unlock;
  }
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_cnecc_exit_standby(void)
{
  if (!s_cnecc_initialised) {
    return k_ra_err_not_initialized;
  }
  for (uint8_t i = 0U; i < (uint8_t)k_ra_cnecc_instance_count; ++i) {
    /* HUM Ch 42.5.2 "Return from Software Standby Mode", p 2876
     * Step 1 + 2: replay cached cfg and re-enable judgment. */
    const ra_err_t err = internal_apply_instance(i, &s_cnecc_cached_cfg.instances[i]);
    RA_RETURN_ON_ERROR(err, s_tag, "cnecc_exit_standby apply"); /* GCOVR_EXCL_BR_LINE */
  }
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_cnecc_set_irq_enables(uint8_t instance, bool irq_1bit, bool irq_2bit)
{
  if ((uint16_t)instance >= (uint16_t)k_ra_cnecc_instance_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_cnecc_regs_t* reg = ra_cnecc(instance);
  RA_CHECK_NULL_PTR((void*)reg, s_tag, "instance mapping failed");
  uint32_t set_bits = 0U;
  if (irq_1bit) {
    set_bits |= (uint32_t)k_ra_cnecc_mask_ec1edic;
  }
  if (irq_2bit) {
    set_bits |= (uint32_t)k_ra_cnecc_mask_ec2edic;
  }
  /* HUM Ch 42.2.1 "EC710CTL : ECC Control Register", p 2870 */
  internal_ctl_rmw(reg, set_bits, (uint32_t)k_ra_cnecc_mask_irq_all);
  /* Refresh cached cfg so a later exit_standby preserves the change. */
  s_cnecc_cached_cfg.instances[instance].irq_1bit = irq_1bit;
  s_cnecc_cached_cfg.instances[instance].irq_2bit = irq_2bit;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_cnecc_set_correction_permission(uint8_t instance, bool correct_1bit)
{
  if ((uint16_t)instance >= (uint16_t)k_ra_cnecc_instance_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_cnecc_regs_t* reg = ra_cnecc(instance);
  RA_CHECK_NULL_PTR((void*)reg, s_tag, "instance mapping failed");
  /* EC1ECP semantics are inverted: bit set means "correction NOT
   * executed" per HUM Ch 42.2.1 "EC710CTL : ECC Control Register"
   * EC1ECP, p 2870. */
  uint32_t set_bits = (uint32_t)k_ra_cnecc_mask_ec1ecp;
  if (correct_1bit) {
    set_bits = 0U;
  }
  internal_ctl_rmw(reg, set_bits, (uint32_t)k_ra_cnecc_mask_ec1ecp);
  s_cnecc_cached_cfg.instances[instance].correct_1bit = correct_1bit;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_cnecc_get_status(uint8_t instance, ra_cnecc_status_t* out)
{
  RA_CHECK_NULL_PTR((void*)out, s_tag, "out must not be nullptr");
  if ((uint16_t)instance >= (uint16_t)k_ra_cnecc_instance_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_cnecc_regs_t* reg = ra_cnecc(instance);
  RA_CHECK_NULL_PTR((void*)reg, s_tag, "instance mapping failed");

  /* HUM Ch 42.2.1 "EC710CTL : ECC Control Register", p 2868 */
  const uint32_t ctl = reg->EC710CTL;
  /* HUM Ch 42.2.2 "EC710TMC : ECC Test Mode Control Register", p 2871 */
  const uint16_t tmc = reg->EC710TMC;
  /* HUM Ch 42.2.4 "EC710EAD0 : ECC Error Address Register", p 2872 */
  const uint32_t ead = reg->EC710EAD0;

  out->raw_ctl         = ctl;
  out->raw_tmc         = tmc;
  out->reserved0       = 0U;
  out->one_bit_count   = s_cnecc_one_bit_count[instance];
  out->two_bit_count   = s_cnecc_two_bit_count[instance];
  out->overflow_count  = s_cnecc_overflow_count[instance];
  out->last_addr       = (uint16_t)(ead & (uint32_t)k_ra_cnecc_mask_ecead);
  out->err_present     = (ctl & (uint32_t)k_ra_cnecc_mask_ecemf) != 0U;
  out->err_1bit        = (ctl & (uint32_t)k_ra_cnecc_mask_ecer1f) != 0U;
  out->err_2bit        = (ctl & (uint32_t)k_ra_cnecc_mask_ecer2f) != 0U;
  out->overflow        = (ctl & (uint32_t)k_ra_cnecc_mask_ecovff) != 0U;
  out->addr_is_1bit    = (ctl & (uint32_t)k_ra_cnecc_mask_ecsedf0) != 0U;
  out->addr_is_2bit    = (ctl & (uint32_t)k_ra_cnecc_mask_ecdedf0) != 0U;
  out->judgment_active = (ctl & (uint32_t)k_ra_cnecc_mask_ecervf) != 0U;
  out->correct_enabled = (ctl & (uint32_t)k_ra_cnecc_mask_ec1ecp) == 0U;
  out->irq1_enabled    = (ctl & (uint32_t)k_ra_cnecc_mask_ec1edic) != 0U;
  out->irq2_enabled    = (ctl & (uint32_t)k_ra_cnecc_mask_ec2edic) != 0U;
  out->test_mode       = (tmc & (uint16_t)k_ra_cnecc_mask_ectmce) != 0U;
  out->reserved1       = false;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_cnecc_get_counters(uint8_t instance, ra_cnecc_counters_t* out)
{
  RA_CHECK_NULL_PTR((void*)out, s_tag, "out must not be nullptr");
  if ((uint16_t)instance >= (uint16_t)k_ra_cnecc_instance_count) {
    return k_ra_err_invalid_arg;
  }
  out->one_bit_count  = s_cnecc_one_bit_count[instance];
  out->two_bit_count  = s_cnecc_two_bit_count[instance];
  out->overflow_count = s_cnecc_overflow_count[instance];
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_cnecc_reset_counters(uint8_t instance)
{
  if ((uint16_t)instance >= (uint16_t)k_ra_cnecc_instance_count) {
    return k_ra_err_invalid_arg;
  }
  s_cnecc_one_bit_count[instance]  = 0U;
  s_cnecc_two_bit_count[instance]  = 0U;
  s_cnecc_overflow_count[instance] = 0U;
  if (s_cnecc_bbr_mirror[instance] != nullptr) {
    s_cnecc_bbr_mirror[instance]->one_bit_count  = 0U;
    s_cnecc_bbr_mirror[instance]->two_bit_count  = 0U;
    s_cnecc_bbr_mirror[instance]->overflow_count = 0U;
  }
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_cnecc_set_counter_mirror(uint8_t instance, ra_cnecc_counters_t* mirror)
{
  if ((uint16_t)instance >= (uint16_t)k_ra_cnecc_instance_count) {
    return k_ra_err_invalid_arg;
  }
  s_cnecc_bbr_mirror[instance] = mirror;
  if (mirror != nullptr) {
    /* Seed the mirror with the current driver-local values so the
     * caller does not see a transient zero on first attach. */
    mirror->one_bit_count  = s_cnecc_one_bit_count[instance];
    mirror->two_bit_count  = s_cnecc_two_bit_count[instance];
    mirror->overflow_count = s_cnecc_overflow_count[instance];
  }
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_cnecc_clear_status(uint8_t instance)
{
  if ((uint16_t)instance >= (uint16_t)k_ra_cnecc_instance_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_cnecc_regs_t* reg = ra_cnecc(instance);
  RA_CHECK_NULL_PTR((void*)reg, s_tag, "instance mapping failed");

  /* HUM Ch 42.2.1 "EC710CTL : ECC Control Register" ECER1C/ECER2C,
   * p 2870: write-1 to both bits clears ECER1F, ECER2F, ECOVFF,
   * ECSEDF0 and ECDEDF0 in one shot, and resets EC710EAD0
   * (HUM 42.2.4 p 2873 "reset by clearing the status flag"). */
  reg->EC710CTL = (uint32_t)k_ra_cnecc_mask_clear_all;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_cnecc_inject_fault(uint8_t instance, const ra_cnecc_inject_t* req)
{
  RA_CHECK_NULL_PTR((void*)req, s_tag, "req must not be nullptr");
  if ((uint16_t)instance >= (uint16_t)k_ra_cnecc_instance_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_cnecc_regs_t* reg = ra_cnecc(instance);
  RA_CHECK_NULL_PTR((void*)reg, s_tag, "instance mapping failed");

  /* HUM Ch 42.3.2 "ECC Decoder Testing", figure 42.2 p 2875.
   * The procedure literal values come straight from the figure --
   * 0x8000 / 0x8080 / 0x8082 -- and re-assert the ETMA = 10b unlock
   * pattern on every write. */

  /* Step 1: disable test mode (also clears ECDCS per HUM Ch 42.2.2
   * "EC710TMC : ECC Test Mode Control Register" ECDCS notes, p 2872). */
  reg->EC710TMC = (uint16_t)k_ra_cnecc_mask_test_disable;

  /* Step 2: write the substitute value into EC710TED. The caller is
   * responsible for picking a value that differs by 1 or 2 bits
   * from the genuine MBRAM word at the target address; the
   * one_bit_flip flag is recorded in the descriptor for traceability
   * but does not change the register write itself. */
  /* HUM Ch 42.2.3 "EC710TED : ECC Test Substitute Data Register", p 2872 */
  reg->EC710TED = req->substitute;
  (void)req->one_bit_flip; /* Tracked for caller bookkeeping. */

  /* Step 3: enable test mode (ETMA unlock + ECTMCE). */
  reg->EC710TMC = (uint16_t)k_ra_cnecc_mask_test_enable;

  /* Step 4: select EC710TED as decoder input (sets ECDCS in addition
   * to ECTMCE). After the next MBRAM read the decoder will see the
   * substitute value and either ECER1F or ECER2F will latch
   * depending on the Hamming distance the caller chose. */
  reg->EC710TMC = (uint16_t)k_ra_cnecc_mask_test_subst;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_cnecc_test_mode_disable(uint8_t instance)
{
  if ((uint16_t)instance >= (uint16_t)k_ra_cnecc_instance_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_cnecc_regs_t* reg = ra_cnecc(instance);
  RA_CHECK_NULL_PTR((void*)reg, s_tag, "instance mapping failed");
  /* HUM Ch 42.2.2 "EC710TMC : ECC Test Mode Control Register", p 2871 */
  reg->EC710TMC = (uint16_t)k_ra_cnecc_mask_test_disable;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_cnecc_test_mode_active(uint8_t instance, bool* out)
{
  RA_CHECK_NULL_PTR((void*)out, s_tag, "out must not be nullptr");
  if ((uint16_t)instance >= (uint16_t)k_ra_cnecc_instance_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_cnecc_regs_t* reg = ra_cnecc(instance);
  RA_CHECK_NULL_PTR((void*)reg, s_tag, "instance mapping failed");
  /* HUM Ch 42.2.2 "EC710TMC : ECC Test Mode Control Register", p 2872 */
  *out = (reg->EC710TMC & (uint16_t)k_ra_cnecc_mask_ectmce) != 0U;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_cnecc_attach_handler(ra_cnecc_error_fn_t fn, void* ctx)
{
  RA_CHECK_NULL_PTR((void*)fn, s_tag, "fn must not be nullptr");
  s_cnecc_fn  = fn;
  s_cnecc_ctx = ctx;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_cnecc_attach_isr(uint8_t priority)
{
  if (priority > (uint8_t)k_ra_isr_prio_max) {
    return k_ra_err_invalid_arg;
  }
  for (uint8_t i = 0U; i < (uint8_t)k_ra_cnecc_instance_count; ++i) {
    /* HUM Ch 42.4 "Interrupts", p 2875 */
    void* const    instance_ctx = (void*)(uintptr_t)i;
    const ra_err_t err          = ra_isr_register(s_cnecc_event_table[i],
                                                  ra_cnecc_isr_handler,
                                                  instance_ctx,
                                                  priority,
                                                  nullptr);
    if (err != k_ra_ok) {
      /* Roll back any earlier slot we opened. */
      for (uint8_t j = 0U; j < i; ++j) {
        (void)ra_isr_unregister(s_cnecc_event_table[j]);
      }
      return err;
    }
  }
  s_cnecc_isr_attached = true;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_cnecc_detach_isr(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra_cnecc_instance_count; ++i) {
    /* HUM Ch 42.4 "Interrupts", p 2875 */
    (void)ra_isr_unregister(s_cnecc_event_table[i]);
  }
  s_cnecc_isr_attached = false;
  return k_ra_ok;
}

void ra_cnecc_isr_handler(void* ctx)
{
  const uint8_t instance = (uint8_t)((uintptr_t)ctx & (uintptr_t)k_ra_cnecc_isr_ctx_inst_mask);
  if ((uint16_t)instance >= (uint16_t)k_ra_cnecc_instance_count) {
    return; /* GCOVR_EXCL_LINE -- attach packs a valid index. */
  }
  volatile r_cnecc_regs_t* reg = ra_cnecc(instance);
  if (reg == nullptr) { /* GCOVR_EXCL_BR_LINE */
    return;             /* GCOVR_EXCL_LINE */
  }

  /* HUM Ch 42.3.1 figure 42.1 p 2874:
   *   - Confirm ECC error flags (ECER1F / ECER2F / ECOVFF).
   *   - Confirm ECC error address flags (ECSEDF0 / ECDEDF0).
   *   - Confirm ECC error address (EC710EAD0).
   *   - Clear ECC error flag (ECER1C = 1, ECER2C = 1). */
  const uint32_t ctl  = reg->EC710CTL;
  const uint32_t ead  = reg->EC710EAD0;
  const uint16_t addr = (uint16_t)(ead & (uint32_t)k_ra_cnecc_mask_ecead);

  const bool one_bit  = (ctl & (uint32_t)k_ra_cnecc_mask_ecer1f) != 0U;
  const bool two_bit  = (ctl & (uint32_t)k_ra_cnecc_mask_ecer2f) != 0U;
  const bool overflow = (ctl & (uint32_t)k_ra_cnecc_mask_ecovff) != 0U;

  /* Dispatch each kind separately so the host callback sees one
   * event per latched bit. 2-bit faults take priority for the
   * "is_2bit" classification when both happen to be set in the
   * same window. */
  if (two_bit) {
    ra_cnecc_dispatch(instance, true, addr);
  }
  if (one_bit) {
    ra_cnecc_dispatch(instance, false, addr);
  }
  if (overflow) {
    ra_cnecc_dispatch_overflow(instance);
  }

  /* HUM Ch 42.3.1 "ECC Function Setting", p 2874 */
  reg->EC710CTL = (uint32_t)k_ra_cnecc_mask_clear_all;
}

void ra_cnecc_dispatch(uint8_t instance, bool is_2bit, uint16_t err_addr)
{
  if ((uint16_t)instance >= (uint16_t)k_ra_cnecc_instance_count) {
    return;
  }
  if (is_2bit) {
    ++s_cnecc_two_bit_count[instance];
    if (s_cnecc_bbr_mirror[instance] != nullptr) {
      ++s_cnecc_bbr_mirror[instance]->two_bit_count;
    }
  } else {
    ++s_cnecc_one_bit_count[instance];
    if (s_cnecc_bbr_mirror[instance] != nullptr) {
      ++s_cnecc_bbr_mirror[instance]->one_bit_count;
    }
  }
  const ra_cnecc_error_fn_t fn  = s_cnecc_fn;
  void* const               ctx = s_cnecc_ctx;
  if (fn != nullptr) {
    fn(ctx, instance, is_2bit, err_addr);
  }
}

void ra_cnecc_dispatch_overflow(uint8_t instance)
{
  if ((uint16_t)instance >= (uint16_t)k_ra_cnecc_instance_count) {
    return;
  }
  ++s_cnecc_overflow_count[instance];
  if (s_cnecc_bbr_mirror[instance] != nullptr) {
    ++s_cnecc_bbr_mirror[instance]->overflow_count;
  }
}
