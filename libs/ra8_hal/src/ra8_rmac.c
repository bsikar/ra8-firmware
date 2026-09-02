/**
 * @file ra8_rmac.c
 * @brief Per-port Ethernet MAC (RMAC) driver implementation -- HUM Ch 33
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Full HAL coverage of the RA8D2 RMAC block (HUM Ch 33, p 1703-1786).
 * The driver owns the per-port MAC layer: lifecycle, MAC-address
 * program, RX address-filter table (perfect-match + multicast hash
 * via MRAFC), VLAN double-tagging (MTFFC), jumbo frame MTU
 * (MRFSCE/MRFSCP), 802.3x pause / 802.1Qbb PFC (MTPFC/MTPFC2/MTPFC3t),
 * Energy Efficient Ethernet / LPI (MEEEC + MMIS2 LPI bits), magic-
 * packet WoL (MRGC.MPDE + MMIS2.MPDIS), MAC-side loopback (MLBC),
 * link config across all interfaces / speeds / duplex modes
 * (MPIC.PIS / .LSC / .PIPP), Clause-22 + Clause-45 PHY MDIO (MPSM
 * driven by MMIS1 completion), the full statistic counter block
 * (MMPFTCT..MTXBCPL), and IRQ management of MEIS / MMIS0 / MMIS1 /
 * MMIS2.
 *
 * The shared MSTP gate ``k_ra8_mstp_eswm`` is reference-counted by
 * ra8_mstp, so init/deinit interleave safely with the other ethernet
 * sub-drivers.
 *
 * Every register access carries a HUM Ch 33 citation.
 *
 * @par State Machine
 * @dot
 * digraph ra8_rmac_states {
 *   bgcolor="transparent";
 *   rankdir=LR;
 *   node [shape=box, style="rounded,filled", fontname="Helvetica", fontsize=10,
 *         fillcolor="#e8eef7", color="#5a7ca6"];
 *   edge [fontname="Helvetica", fontsize=9, color="#5a7ca6"];
 *
 *   __start [shape=circle, width=0.18, label="", fillcolor="#5a7ca6", color="#5a7ca6"];
 *
 *   RESET [label="RESET"];
 *   CONFIG [label="CONFIG"];
 *   RUN [label="RUN"];
 *   SLEEP [label="SLEEP"];
 *   STOPPED [label="STOPPED"];
 *
 *   __start -> RESET [label="ra8_rmac_init()"];
 *   RESET -> CONFIG [label="caller programs MRMAC etc."];
 *   CONFIG -> RUN [label="ETHA enters OPERATION mode"];
 *   RUN -> SLEEP [label="ra8_rmac_set_lpi(true)"];
 *   SLEEP -> RUN [label="ra8_rmac_set_lpi(false)"];
 *   RUN -> STOPPED [label="ra8_rmac_deinit()"];
 * }
 * @enddot
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_rmac.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_ether_regs.h"
#include "ra8_hal_internal.h"
#include "ra8_hw_err.h"
#include "ra8_hw_intrinsics.h"
#include "ra8_log.h"
#include "ra8_mstp.h"
#include "ra8_mstp_regs.h"
#include "ra8_rmac_regs.h"

/**
 * @var s_tag
 * @brief Logger tag used by every ra8_rmac_* call.
 */
static const char* s_tag = "RMAC";

/** @brief RMAC field constants. */
typedef enum : uint32_t {
  k_rmac_pipp_pos   = 9U,    /**< PIPP (in-pause) bit position.  */
  k_rmac_pfrlv_mask = 0x1FU, /**< 5-bit pause-frame retry level. */
} rmac_field_t;

/**
 * @enum ra8_rmac_internal_t
 * @brief Driver-private constants (poll budgets, sentinel values).
 */
typedef enum : uint32_t {
  k_ra8_rmac_mdio_poll_budget = 100000UL, /**< Bounded MDIO poll loop. */
} ra8_rmac_internal_t;

/**
 * @enum ra8_rmac_bit_pos_t
 * @brief Bit positions for register fields not covered by the
 *        register-header enum file.
 *
 * @details
 * HUM Ch 33.4 "MPFC", "MTPFC", "MTPFC2" p 1707. The header file
 * exposes the data-byte mask/shift but leaves these single-bit / wide
 * field positions inline in the driver. Naming them avoids
 * readability-magic-numbers warnings and documents intent.
 */
typedef enum : uint8_t {
  k_ra8_rmac_shift_mpfc_te0    = 16U, /**< MPFC.TEF0 (PTP table entry 0 enable).  */
  k_ra8_rmac_shift_mpfc_te1    = 17U, /**< MPFC.TEF1 (PTP table entry 1 enable).  */
  k_ra8_rmac_shift_mtpfc_pfrlv = 27U, /**< MTPFC.PFRLV[31:27] retry-level field.  */
  k_ra8_rmac_shift_mtpfc2_pfm  = 26U, /**< MTPFC2.PFM (1 = PFC frame, 0 = pause). */
} ra8_rmac_bit_pos_t;

/**
 * @struct ra8_rmac_slot_t
 * @brief Per-port runtime state.
 */
typedef struct {
  ra8_rmac_event_fn_t cb;            /**< Attached callback (nullptr).         */
  void*               ctx;           /**< Opaque cookie passed to cb.          */
  ra8_rmac_mrafc_t    cached_filter; /**< Last filter for stop/exit.           */
  uint8_t             in_use;        /**< 1 once init succeeds, 0 after deinit */
} ra8_rmac_slot_t;

/**
 * @var s_slots
 * @brief Per-port handler / cache table; index = ::ra8_rmac_port_t.
 */
static ra8_rmac_slot_t s_slots[k_ra8_rmac_port_count];

/**
 * @brief Validate an MPIC.PIS interface selector.
 *
 * @details HUM Ch 33.4.1.2 p 1707: MPIC.PIS has only two defined
 * encodings -- MII (000b) and GMII (010b). Everything else is
 * Reserved. The enum is non-contiguous so this is an explicit
 * two-value check rather than an upper-bound comparison.
 *
 * @param[in] iface Candidate interface selector.
 *
 * @return true if iface is MII or GMII; false otherwise.
 * @retval true  iface == k_ra8_rmac_pis_mii or k_ra8_rmac_pis_gmii.
 * @retval false iface is a Reserved encoding.
 *
 * @pre None.
 * @pre iface holds a value of the ra8_rmac_pis_t underlying type.
 * @post Return value reflects whether iface is a defined PIS code.
 * @post No state is modified.
 *
 * @note Pure function; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static inline bool internal_pis_ok(ra8_rmac_pis_t iface)
{
  if (iface == k_ra8_rmac_pis_mii) {
    return true;
  }
  if (iface == k_ra8_rmac_pis_gmii) {
    return true;
  }
  return false;
}

/**
 * @brief Compute the MPIC.PSMCS divider code for a target MDC rate.
 *
 * @details
 * HUM Ch 33.4.1.2 "MPIC.PSMCS" + FSP r_rmac_phy_calculate_mpic:
 *   PSMCS = ((eswclk_hz / mdc_hz) / 2) - 1
 *
 * Clamps to the 7-bit field width. Returns the conservative ceiling
 * (slowest MDC) on a divide-by-zero or absurdly slow ESWCLK so the PHY
 * has the best chance of latching the frame.
 *
 * @param[in] eswclk_hz Live ESWCLK frequency in Hz (caller-provided).
 * @param[in] mdc_hz    Desired MDC frequency in Hz (e.g. 1 MHz).
 *
 * @return PSMCS divider code, clamped to ::k_ra8_rmac_mdc_psmcs_max.
 * @retval 0..0x7F Valid PSMCS divider code.
 *
 * @pre eswclk_hz > 0.
 * @pre mdc_hz    > 0.
 * @post Returned value fits in the 7-bit PSMCS field.
 * @post Pure function: no state changes.
 * @note Pure function.
 * @since 0.1.0
 */
RA8_INTERNAL
static inline uint32_t internal_calc_psmcs(uint32_t eswclk_hz, uint32_t mdc_hz)
{
  /* Defensive clamp: ESWCLK = 0 means the chip's CGC has not been
   * configured; pick the slowest MDC the field can encode so the PHY
   * has the best chance of latching. ``mdc_hz`` is guaranteed non-zero
   * by the only caller (``internal_program_mac_config`` substitutes
   * ``k_ra8_rmac_mdc_default_hz`` when ``cfg->mdc_hz`` is 0), so a
   * single early-return on eswclk_hz==0 is sufficient. */
  if (eswclk_hz == 0U) {
    return (uint32_t)k_ra8_rmac_mdc_psmcs_max;
  }
  /* Same defensive clamp on mdc_hz to avoid undefined integer div if
   * a future caller bypasses the substitution above. */
  if (mdc_hz == 0U) {
    return (uint32_t)k_ra8_rmac_mdc_psmcs_max;
  }
  uint32_t psmcs = ((eswclk_hz / mdc_hz) / 2U);
  if (psmcs == 0U) {
    psmcs = 1U;
  }
  psmcs -= 1U;
  if (psmcs > (uint32_t)k_ra8_rmac_mdc_psmcs_max) {
    psmcs = (uint32_t)k_ra8_rmac_mdc_psmcs_max;
  }
  return psmcs;
}

/**
 * @brief Build the MPIC value from interface / speed / duplex inputs.
 *
 * @param[in] iface   PHY interface.
 * @param[in] speed   Link speed.
 * @param[in] duplex  Duplex mode.
 * @param[in] psmcs   MPIC.PSMCS clock-divider code (see ::internal_calc_psmcs).
 * @return 32-bit MPIC value with PIS / LSC / PIPP / PSMCS fields packed.
 *
 * @details
 * Packs every MPIC field the driver owns:
 *   - PIS  [2:0]  -- physical interface selector.
 *   - LSC  [5:3]  -- link-speed selector.
 *   - PIPP [9]    -- 1 = full duplex, 0 = half.
 *   - PSMCS[22:16]-- PHY-station-management clock divider (MDC rate).
 *
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static inline uint32_t internal_make_mpic(ra8_rmac_pis_t    iface,
                                          ra8_rmac_lsc_t    speed,
                                          ra8_rmac_duplex_t duplex,
                                          uint32_t          psmcs)
{
  /* HUM Ch 33.4 "MPIC : PHY Interfaces Configuration Register" p 1709 */
  const uint32_t pis         = ((uint32_t)iface & k_ra8_rmac_mask_mpic_pis)
                               << (uint32_t)k_ra8_rmac_shift_mpic_pis;
  const uint32_t lsc         = ((uint32_t)speed & k_ra8_rmac_mask_mpic_lsc)
                               << (uint32_t)k_ra8_rmac_shift_mpic_lsc;
  const uint32_t pipp        = (duplex == k_ra8_rmac_duplex_full) ? (1U << k_rmac_pipp_pos) : 0U;
  const uint32_t psmcs_field = (psmcs & k_ra8_rmac_mask_mpic_psmcs)
                               << (uint32_t)k_ra8_rmac_shift_mpic_psmcs;
  return pis | lsc | pipp | psmcs_field;
}

/**
 * @brief Drain any in-flight MDIO transaction (poll MPSM.PSME -> 0).
 *
 * @details
 * HUM Ch 33.4.1.1 "MPSM" p 1707-1708, Note 2: "Accessing to MPSM
 * register while this [PSME] bit is asserted has no effect." So the
 * driver MUST confirm PSME == 0 BEFORE issuing a new transaction --
 * otherwise the issuing write is silently swallowed, the post-wait
 * polls the *previous* transaction, and the caller reads stale PRD.
 *
 * Call this immediately before every ::internal_mpsm_issue.
 *
 * @param[in] reg RMAC register window.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok             PSME is 0; the bus is idle.
 * @retval k_ra8_err_hw_timeout PSME never cleared within the budget.
 *
 * @pre reg points at a live RMAC register window.
 * @pre MPIC.PSMCS != 0 (MDC clock running).
 * @post On k_ra8_ok no MDIO transaction is in flight.
 * @post Side-effect free apart from the bounded busy-wait.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_mdio_drain(volatile r_rmac_regs_t* reg)
{
  /* Bounded wait for MPSM.PSME to self-clear to 0 (HUM Note 2: a write
   * to MPSM while PSME=1 has no effect, so the bus must be idle first).
   * ra8_hw_wait_flag_clear32 runs the same real poll/timeout loop on the
   * host unit-test build, where each iteration is routed through the
   * ra8_fake_mmio seam so a test can drive PSME clearing or a timeout. */
  return ra8_hw_wait_flag_clear32(&reg->MPSM,
                                  (uint32_t)k_ra8_rmac_mpsm_psme,
                                  (uint32_t)k_ra8_rmac_mdio_poll_budget);
}

/**
 * @brief Wait for an MDIO transaction to complete, with a bounded poll budget.
 *
 * @details
 * The HUM-canonical completion signal is **MPSM.PSME** self-clearing
 * to 0 (HUM Ch 33.4.1.1 p 1708: "When PHY register access is
 * completed, this bit returns automatically to 0"). The MMIS1 status
 * bits are RW1C and a *stale* MMIS1.PRACS left over from the previous
 * transaction caused spurious immediate returns on real silicon --
 * the caller then read PRD before the new transaction had even
 * started, observing 0. Production therefore polls **only** PSME.
 *
 * The real PSME poll runs on every build. On the host unit-test
 * build each poll's loop-exit decision is routed through the
 * ra8_fake_mmio fault seam keyed on MPSM: first-poll success when no
 * fault is armed, or a test-armed retry / timeout leg (T1-01). The
 * drain in ::internal_mdio_drain polls the same MPSM address, so a
 * test isolates THIS wait with ``ra8_fake_mmio_fail_nth_wait``.
 *
 * @param[in] reg  RMAC register window.
 * @param[in] mask MMIS1 completion bit this op sets; cleared via MMID1
 *                 on completion so a stale latch cannot outlive the op.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok             Transaction completed (PSME == 0).
 * @retval k_ra8_err_hw_timeout PSME never cleared within the budget.
 *
 * @pre ::internal_mpsm_issue launched a transaction on this window.
 * @pre The bus was drained via ::internal_mdio_drain before issuing.
 * @post On k_ra8_ok PRD holds the completed transaction's data.
 * @post The matching MMIS1 status bit is cleared via MMID1.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_mdio_wait(volatile r_rmac_regs_t* reg, uint32_t mask)
{
  for (uint32_t i = 0; i < k_ra8_rmac_mdio_poll_budget; ++i) {
    /* HUM Ch 33.4.1.1 "MPSM : PHY Station Management Register" p 1708 */
    const bool done = ((reg->MPSM & (uint32_t)k_ra8_rmac_mpsm_psme) == 0U);
#if defined(RA8_OFF_TARGET) && defined(UNIT_TEST)
    /* Host MMIO fault seam, keyed on MPSM (the polled register). */
    if (ra8_fake_mmio_wait_eval(&reg->MPSM, i, done)) {
      /* HUM Ch 33 "MMID1 / MMIS1" p 1707 */
      reg->MMID1 = mask;
      return k_ra8_ok;
    }
#else
    if (done) {
      /* HUM Ch 33 "MMID1 / MMIS1" p 1707 */
      reg->MMID1 = mask;
      return k_ra8_ok;
    }
#endif
  }
  return k_ra8_err_hw_timeout;
}

/**
 * @brief Issue a raw MPSM transaction.
 *
 * @param[in] reg      RMAC register window.
 * @param[in] pda_5bit 5-bit PDA field (PHY address for C22; same for C45).
 * @param[in] pra_5bit 5-bit PRA field (register address for C22; device
 *                     address for C45 -- the on-the-wire MMD selector).
 * @param[in] op       MPSM.POP encoding.
 * @param[in] prd_16bit 16-bit PRD payload (ignored on read; for C45 this
 *                     carries the 16-bit register address on the
 *                     ``c45_address`` op and the data word on
 *                     ``c45_write``).
 * @param[in] mff      MPSM.MFF (0 for C22, 1 for C45).
 *
 * @details See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_mpsm_issue(volatile r_rmac_regs_t* reg,
                                uint8_t                 pda_5bit,
                                uint8_t                 pra_5bit,
                                ra8_rmac_mdio_op_t      op,
                                uint16_t                prd_16bit,
                                bool                    mff)
{
  /* HUM Ch 33.4.1.1 "MPSM : PHY Station Management Register" p 1707 */
  const uint32_t pda     = ((uint32_t)pda_5bit & k_ra8_rmac_mask_mpsm_phy_reg)
                           << (uint32_t)k_ra8_rmac_shift_mpsm_pda;
  const uint32_t pra     = ((uint32_t)pra_5bit & k_ra8_rmac_mask_mpsm_phy_reg)
                           << (uint32_t)k_ra8_rmac_shift_mpsm_pra;
  const uint32_t pop     = ((uint32_t)op & k_ra8_rmac_mask_mpsm_op)
                           << (uint32_t)k_ra8_rmac_shift_mpsm_pop;
  const uint32_t prd     = ((uint32_t)prd_16bit & k_ra8_rmac_mask_mpsm_data)
                           << (uint32_t)k_ra8_rmac_shift_mpsm_prd;
  uint32_t       mff_bit = 0UL;
  if (mff) {
    mff_bit = (1UL << 2U);
  }
  /* Clear any stale MMIS1 completion bits from the previous
   * transaction BEFORE launching this one, so the post-wait can
   * never short-circuit on a leftover status bit. MMID1 is the
   * write-1-to-clear alias of MMIS1. */
  /* HUM Ch 33 "MMID1 / MMIS1" p 1707 */
  reg->MMID1 = k_ra8_rmac_mask_all;
  /* PSME = bit 0 -- starts the transaction. The caller has already
   * drained PSME to 0 via internal_mdio_drain, so this write is
   * honoured (HUM Note 2: a write while PSME=1 has no effect). */
  /* HUM Ch 33.4.1.1 "MPSM : PHY Station Management Register" p 1707 */
  reg->MPSM = pda | pra | pop | prd | mff_bit | (uint32_t)k_ra8_rmac_mpsm_psme;
  /* Ensure the issuing write reaches the peripheral before the post-wait
   * starts polling MPSM (host no-op via the ra8_hw_intrinsics seam). */
  ra8_hw_dsb();
}

/**
 * @brief Reset every per-MAC config register to its post-init baseline.
 *
 * @details
 * HUM Ch 33.4 "MIOC ... MEEEC" p 1707. Splits ``ra8_rmac_init`` so it
 * stays under the function-size threshold.
 *
 * @param[in,out] reg RMAC register window.
 * @param[in]     cfg Validated init config.
 *
 * @pre Module clock ungated.
 * @post All listed registers reflect ``cfg``.
 *
 * @note Internal helper, not thread-safe.
 *
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_program_mac_config(volatile r_rmac_regs_t* reg, const ra8_rmac_config_t* cfg)
{
  reg->MIOC             = 0U;
  reg->MTFFC            = 0U;
  reg->MRMAC0           = 0U;
  reg->MRMAC1           = 0U;
  reg->MRGC             = 0U;
  reg->MRAFC            = (uint32_t)cfg->rx_filter;
  reg->MLBC             = 0U;
  reg->MEEEC            = 0U;
  const uint32_t mdc_hz = (cfg->mdc_hz != 0U) ? cfg->mdc_hz : (uint32_t)k_ra8_rmac_mdc_default_hz;
  const uint32_t psmcs  = internal_calc_psmcs(cfg->eswclk_hz, mdc_hz);
  reg->MPIC = internal_make_mpic(cfg->phy_interface, cfg->link_speed, cfg->duplex, psmcs);
}

/**
 * @brief Reset/disable every IRQ status + enable register.
 *
 * @details
 * HUM Ch 33.4 "MEID/MMID0..2" p 1706 -- write 1 to clear, then load
 * the caller's enable masks into MEIE / MMIE0..2.
 *
 * @param[in,out] reg RMAC register window.
 * @param[in]     cfg Validated init config.
 *
 * @pre Module clock ungated.
 * @post Status bits cleared, enable masks programmed.
 *
 * @note Internal helper, not thread-safe.
 *
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_program_irq_block(volatile r_rmac_regs_t* reg, const ra8_rmac_config_t* cfg)
{
  reg->MEID  = k_ra8_rmac_mask_all;
  reg->MMID0 = k_ra8_rmac_mask_all;
  reg->MMID1 = k_ra8_rmac_mask_all;
  reg->MMID2 = k_ra8_rmac_mask_all;

  reg->MEIE  = cfg->err_irq_enable;
  reg->MMIE0 = cfg->mon0_irq_enable;
  reg->MMIE1 = cfg->mon1_irq_enable;
  reg->MMIE2 = cfg->mon2_irq_enable;
}

ra8_err_t ra8_rmac_init(ra8_rmac_port_t port, const ra8_rmac_config_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "rmac_init: cfg must not be nullptr");
  if (!internal_port_ok(port)) {
    ra8_log_error(s_tag, "rmac_init: port out of range");
    return k_ra8_err_invalid_arg;
  }
  if (!internal_pis_ok(cfg->phy_interface)) {
    ra8_log_error(s_tag, "rmac_init: phy_interface out of range");
    return k_ra8_err_invalid_arg;
  }
  if ((uint8_t)cfg->link_speed >= k_ra8_rmac_lsc_count) {
    ra8_log_error(s_tag, "rmac_init: link_speed out of range");
    return k_ra8_err_invalid_arg;
  }

  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
  const ra8_err_t mst_err = ra8_mstp_enable(k_ra8_mstp_eswm);
  /* GCOVR_EXCL_BR_START -- MSTP HW readback */
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "rmac_init: mstp enable");
  /* GCOVR_EXCL_BR_STOP */

  volatile r_rmac_regs_t* reg = ra8_rmac(port);
  internal_program_mac_config(reg, cfg);
  internal_program_irq_block(reg, cfg);

  s_slots[(uint8_t)port].cb            = nullptr;
  s_slots[(uint8_t)port].ctx           = nullptr;
  s_slots[(uint8_t)port].cached_filter = cfg->rx_filter;
  s_slots[(uint8_t)port].in_use        = 1U;
  ra8_log_info(s_tag, "rmac_init");
  return k_ra8_ok;
}

ra8_err_t ra8_rmac_deinit(ra8_rmac_port_t port)
{
  if (!internal_port_ok(port)) {
    ra8_log_error(s_tag, "rmac_deinit: port out of range");
    return k_ra8_err_invalid_arg;
  }

  volatile r_rmac_regs_t* reg = ra8_rmac(port);

  /* HUM Ch 33.4 "MRAFC : MAC Reception Address Filter Configuration Register" p 1717 */
  reg->MRAFC = 0U;
  /* HUM Ch 33.4 "MRGC : MAC Reception General Configuration Register" p 1715 */
  reg->MRGC = 0U;
  /* HUM Ch 33.4 "MTFFC : MAC Transmission Frame Format Configuration Register" p 1711 */
  reg->MTFFC = 0U;
  /* HUM Ch 33.4 "MLBC : MAC Loopback Configuration Register" p 1726 */
  reg->MLBC = 0U;
  /* HUM Ch 33.4 "MEEEC : MAC Energy Efficient Ethernet Configuration Register" p 1726 */
  reg->MEEEC = 0U;
  /* HUM Ch 33.4 "MEIE : MAC Error Interrupt Enable Register" p 1751 */
  reg->MEIE = 0U;
  /* HUM Ch 33.4 "MMIE0 : MAC Monitoring Interrupt Enable Register 0" p 1757 */
  reg->MMIE0 = 0U;
  /* HUM Ch 33.4 "MMIE1 : MAC Monitoring Interrupt Enable Register 1" p 1759 */
  reg->MMIE1 = 0U;
  /* HUM Ch 33.4 "MMIE2 : MAC Monitoring Interrupt Enable Register 2" p 1762 */
  reg->MMIE2 = 0U;

  s_slots[(uint8_t)port].cb     = nullptr;
  s_slots[(uint8_t)port].ctx    = nullptr;
  s_slots[(uint8_t)port].in_use = 0U;
  /* MSTP gate is shared with the rest of the Ethernet subsystem; do
   * NOT drop it here. ra8_eth_deinit owns the final reference. */
  return k_ra8_ok;
}

ra8_err_t ra8_rmac_enter_stop(ra8_rmac_port_t port)
{
  if (!internal_port_ok(port)) {
    ra8_log_error(s_tag, "rmac_enter_stop: port out of range");
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 33.4 "MRAFC : MAC Reception Address Filter Configuration Register" p 1717 */
  ra8_rmac(port)->MRAFC = 0U;
  return k_ra8_ok;
}

ra8_err_t ra8_rmac_exit_stop(ra8_rmac_port_t port)
{
  if (!internal_port_ok(port)) {
    ra8_log_error(s_tag, "rmac_exit_stop: port out of range");
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 33.4 "MRAFC : MAC Reception Address Filter Configuration Register" p 1717 */
  ra8_rmac(port)->MRAFC = (uint32_t)s_slots[(uint8_t)port].cached_filter;
  return k_ra8_ok;
}

ra8_err_t ra8_rmac_set_mac_address(ra8_rmac_port_t port,
                                   const uint8_t   mac[k_ra8_rmac_mac_byte_count])
{
  RA8_CHECK_NULL_PTR(mac, s_tag, "set_mac_address: mac must not be nullptr");
  if (!internal_port_ok(port)) {
    ra8_log_error(s_tag, "set_mac_address: port out of range");
    return k_ra8_err_invalid_arg;
  }

  /* MRMAC0.MAU holds the upper 16 bits (bytes 0..1, MSB at bit 15);
   * MRMAC1.MAL holds the lower 32 bits (bytes 2..5, mac[2] in [31:24]).
   * Source: HUM Ch 33.4 "MRMAC0/MRMAC1 : MAC Reception MAC Address
   * Configuration Registers" p 1707, cross-checked against FSP
   * R_RMAC0_Type.MRMAC0_b.MAU and .MRMAC1_b.MAL. */
  const uint32_t b0 = (uint32_t)mac[k_ra8_rmac_mac_byte_0] & k_ra8_rmac_mask_byte;
  const uint32_t b1 = (uint32_t)mac[k_ra8_rmac_mac_byte_1] & k_ra8_rmac_mask_byte;
  const uint32_t b2 = (uint32_t)mac[k_ra8_rmac_mac_byte_2] & k_ra8_rmac_mask_byte;
  const uint32_t b3 = (uint32_t)mac[k_ra8_rmac_mac_byte_3] & k_ra8_rmac_mask_byte;
  const uint32_t b4 = (uint32_t)mac[k_ra8_rmac_mac_byte_4] & k_ra8_rmac_mask_byte;
  const uint32_t b5 = (uint32_t)mac[k_ra8_rmac_mac_byte_5] & k_ra8_rmac_mask_byte;

  const uint32_t mrmac0 =
    ((b0 << (uint32_t)k_ra8_rmac_shift_high24) | (b1 << (uint32_t)k_ra8_rmac_shift_high16)) &
    k_ra8_rmac_mask_mrmac0_mau;
  const uint32_t mrmac1 =
    (b2 << (uint32_t)k_ra8_rmac_shift_byte3) | (b3 << (uint32_t)k_ra8_rmac_shift_byte2) |
    (b4 << (uint32_t)k_ra8_rmac_shift_byte1) | (b5 << (uint32_t)k_ra8_rmac_shift_byte0);

  volatile r_rmac_regs_t* reg = ra8_rmac(port);
  /* HUM Ch 33.4 "MRMAC0 : MAC Reception MAC Address Configuration Register 0" p 1716 */
  reg->MRMAC0 = mrmac0;
  /* HUM Ch 33.4 "MRMAC1 : MAC Reception MAC Address Configuration Register 1" p 1717 */
  reg->MRMAC1 = mrmac1;
  return k_ra8_ok;
}

ra8_err_t ra8_rmac_set_rx_filter(ra8_rmac_port_t port, ra8_rmac_mrafc_t filter)
{
  if (!internal_port_ok(port)) {
    ra8_log_error(s_tag, "set_rx_filter: port out of range");
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 33.4 "MRAFC : MAC Reception Address Filter Configuration Register" p 1717 */
  ra8_rmac(port)->MRAFC                = (uint32_t)filter;
  s_slots[(uint8_t)port].cached_filter = filter;
  return k_ra8_ok;
}

ra8_err_t ra8_rmac_set_ptp_filter(ra8_rmac_port_t port,
                                  uint8_t         index,
                                  uint8_t         byte_offset,
                                  uint8_t         value,
                                  bool            tef0,
                                  bool            tef1)
{
  if (!internal_port_ok(port)) {
    ra8_log_error(s_tag, "set_ptp_filter: port out of range");
    return k_ra8_err_invalid_arg;
  }
  if (index >= k_ra8_rmac_ptp_filter_count) {
    ra8_log_error(s_tag, "set_ptp_filter: index out of range");
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 33.4 "MPFCt : MAC PTP Filtering Register Configuration t" p 1724 */
  const uint32_t pfbn = (uint32_t)byte_offset & k_ra8_rmac_mask_byte;
  const uint32_t pfbv = ((uint32_t)value & k_ra8_rmac_mask_byte)
                        << (uint32_t)k_ra8_rmac_shift_mpfc_val;
  uint32_t       te   = 0UL;
  if (tef0) {
    te |= (1UL << (uint32_t)k_ra8_rmac_shift_mpfc_te0);
  }
  if (tef1) {
    te |= (1UL << (uint32_t)k_ra8_rmac_shift_mpfc_te1);
  }
  ra8_rmac(port)->MPFC[index] = pfbn | pfbv | te;
  return k_ra8_ok;
}

ra8_err_t
ra8_rmac_set_frame_size(ra8_rmac_port_t port, bool is_pframe, uint16_t min_size, uint16_t max_size)
{
  if (!internal_port_ok(port)) {
    ra8_log_error(s_tag, "set_frame_size: port out of range");
    return k_ra8_err_invalid_arg;
  }
  if (min_size > max_size) {
    ra8_log_error(s_tag, "set_frame_size: min > max");
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 33.4 "MRFSCE / MRFSCP : MAC Reception Frame Size Configuration" p 1707 */
  const uint32_t v =
    (((uint32_t)max_size & k_ra8_rmac_mask_mrfsc) << (uint32_t)k_ra8_rmac_shift_mrfsce_max) |
    (((uint32_t)min_size & k_ra8_rmac_mask_mrfsc) << (uint32_t)k_ra8_rmac_shift_mrfsce_min);
  if (is_pframe) {
    ra8_rmac(port)->MRFSCP = v;
  } else {
    ra8_rmac(port)->MRFSCE = v;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_rmac_set_vlan_framing(ra8_rmac_port_t port, bool disable_pad, bool use_mcrc)
{
  if (!internal_port_ok(port)) {
    ra8_log_error(s_tag, "set_vlan_framing: port out of range");
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 33.4 "MTFFC : MAC Transmission Frame Format Configuration" p 1711 */
  uint32_t v = 0UL;
  if (disable_pad) {
    v |= k_ra8_rmac_mtffc_dpad;
  }
  if (use_mcrc) {
    v |= k_ra8_rmac_mtffc_fcm;
  }
  ra8_rmac(port)->MTFFC = v;
  return k_ra8_ok;
}

ra8_err_t ra8_rmac_set_pause_frame(ra8_rmac_port_t       port,
                                   ra8_rmac_pause_mode_t mode,
                                   uint16_t              pause_time,
                                   uint8_t               retry_time,
                                   uint8_t               retry_level)
{
  if (!internal_port_ok(port)) {
    ra8_log_error(s_tag, "set_pause_frame: port out of range");
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 33.4 "MTPFC : MAC Transmission Pause or PFC Frame Configuration" p 1712 */
  const uint32_t pt    = ((uint32_t)pause_time & k_ra8_rmac_mask_mtpfc_pt)
                         << (uint32_t)k_ra8_rmac_shift_mtpfc_pt;
  const uint32_t pfrt  = ((uint32_t)retry_time & k_ra8_rmac_mask_mtpfc_pfrt)
                         << (uint32_t)k_ra8_rmac_shift_mtpfc_pfrt;
  const uint32_t pfrlv = ((uint32_t)retry_level & k_rmac_pfrlv_mask)
                         << (uint32_t)k_ra8_rmac_shift_mtpfc_pfrlv;
  /* HUM Ch 33.4 "MTPFC2 : MAC Transmission Pause or PFC Frame Cfg 2" p 1713 */
  uint32_t pfm = 0UL;
  if (mode == k_ra8_rmac_pause_mode_pfc) {
    pfm = (1UL << (uint32_t)k_ra8_rmac_shift_mtpfc2_pfm);
  }
  ra8_rmac(port)->MTPFC  = pt | pfrt | pfrlv;
  ra8_rmac(port)->MTPFC2 = pfm;
  return k_ra8_ok;
}

ra8_err_t ra8_rmac_set_pfc_group(ra8_rmac_port_t port, ra8_rmac_pfc_group_t group, uint8_t pcp_mask)
{
  if (!internal_port_ok(port)) {
    ra8_log_error(s_tag, "set_pfc_group: port out of range");
    return k_ra8_err_invalid_arg;
  }
  if ((uint8_t)group >= k_ra8_rmac_pfc_group_count) {
    ra8_log_error(s_tag, "set_pfc_group: group out of range");
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 33.4 "MTPFC3t : MAC Transmission PFC Frame Configuration 3" p 1715 */
  ra8_rmac(port)->MTPFC3[(uint8_t)group] = (uint32_t)pcp_mask & k_ra8_rmac_mask_pfc_priority;
  return k_ra8_ok;
}

ra8_err_t ra8_rmac_set_lpi(ra8_rmac_port_t port, bool enable)
{
  if (!internal_port_ok(port)) {
    ra8_log_error(s_tag, "set_lpi: port out of range");
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 33.4 "MEEEC : MAC Energy Efficient Ethernet Configuration" p 1726 */
  uint32_t meeec = 0UL;
  if (enable) {
    meeec = k_ra8_rmac_meeec_lpitr;
  }
  ra8_rmac(port)->MEEEC = meeec;
  return k_ra8_ok;
}

ra8_err_t ra8_rmac_set_magic_packet(ra8_rmac_port_t port, bool enable)
{
  if (!internal_port_ok(port)) {
    ra8_log_error(s_tag, "set_magic_packet: port out of range");
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 33.4 "MRGC : MAC Reception General Configuration" p 1715 */
  volatile r_rmac_regs_t* reg = ra8_rmac(port);
  uint32_t                v   = reg->MRGC;
  if (enable) {
    v |= k_ra8_rmac_mrgc_mpde;
  } else {
    v &= ~k_ra8_rmac_mrgc_mpde;
  }
  reg->MRGC = v;
  return k_ra8_ok;
}

ra8_err_t ra8_rmac_set_loopback(ra8_rmac_port_t port, bool enable)
{
  if (!internal_port_ok(port)) {
    ra8_log_error(s_tag, "set_loopback: port out of range");
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 33.4 "MLBC : MAC Loopback Configuration Register" p 1726 */
  uint32_t mlbc = 0UL;
  if (enable) {
    mlbc = k_ra8_rmac_mlbc_lbme;
  }
  ra8_rmac(port)->MLBC = mlbc;
  return k_ra8_ok;
}

ra8_err_t ra8_rmac_set_link(ra8_rmac_port_t   port,
                            ra8_rmac_pis_t    iface,
                            ra8_rmac_lsc_t    speed,
                            ra8_rmac_duplex_t duplex)
{
  if (!internal_port_ok(port)) {
    ra8_log_error(s_tag, "set_link: port out of range");
    return k_ra8_err_invalid_arg;
  }
  if (!internal_pis_ok(iface)) {
    ra8_log_error(s_tag, "set_link: iface out of range");
    return k_ra8_err_invalid_arg;
  }
  if ((uint8_t)speed >= k_ra8_rmac_lsc_count) {
    ra8_log_error(s_tag, "set_link: speed out of range");
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 33.4 "MPIC : PHY Interfaces Configuration Register" p 1709
   * Preserve the existing PSMCS (MDC divider) field -- ra8_rmac_init
   * programmed it from cfg->eswclk_hz / mdc_hz; a runtime ra8_rmac_set_link
   * call must NOT clobber the MDC clock the PHY is already using to ack
   * the very transaction that is updating PIS/LSC/PIPP. */
  volatile r_rmac_regs_t* reg = ra8_rmac(port);
  const uint32_t          existing_psmcs =
    (reg->MPIC >> (uint32_t)k_ra8_rmac_shift_mpic_psmcs) & (uint32_t)k_ra8_rmac_mask_mpic_psmcs;
  reg->MPIC = internal_make_mpic(iface, speed, duplex, existing_psmcs);
  return k_ra8_ok;
}

ra8_err_t ra8_rmac_mdio_c22_read(ra8_rmac_port_t port,
                                 uint8_t         phy_addr,
                                 uint8_t         reg_addr,
                                 uint16_t*       out_value)
{
  RA8_CHECK_NULL_PTR(out_value, s_tag, "mdio_c22_read: out_value must not be nullptr");
  if (!internal_port_ok(port)) {
    ra8_log_error(s_tag, "mdio_c22_read: port out of range");
    return k_ra8_err_invalid_arg;
  }

  volatile r_rmac_regs_t* reg = ra8_rmac(port);
  /* Pre-wait: a write to MPSM while PSME=1 has no effect (HUM Note 2). */
  const ra8_err_t drain_err = internal_mdio_drain(reg);
  if (drain_err != k_ra8_ok) {
    ra8_log_error(s_tag, "mdio_c22_read: pre-drain timeout");
    return drain_err;
  }
  internal_mpsm_issue(reg, phy_addr, reg_addr, k_ra8_rmac_mdio_op_c22_read, 0U, false);
  const ra8_err_t wait_err = internal_mdio_wait(reg, k_ra8_rmac_mmis1_pracs);
  if (wait_err != k_ra8_ok) {
    ra8_log_error(s_tag, "mdio_c22_read: timeout"); /* GCOVR_EXCL_BR_LINE -- MDIO status no stuck */
    return wait_err;
  }
  /* HUM Ch 33.4.1.1 "MPSM : PHY Station Management Register" p 1707 */
  *out_value =
    (uint16_t)((reg->MPSM >> (uint32_t)k_ra8_rmac_shift_mpsm_prd) & k_ra8_rmac_mask_mpsm_data);
  return k_ra8_ok;
}

ra8_err_t
ra8_rmac_mdio_c22_write(ra8_rmac_port_t port, uint8_t phy_addr, uint8_t reg_addr, uint16_t value)
{
  if (!internal_port_ok(port)) {
    ra8_log_error(s_tag, "mdio_c22_write: port out of range");
    return k_ra8_err_invalid_arg;
  }
  volatile r_rmac_regs_t* reg = ra8_rmac(port);
  /* Pre-wait: a write to MPSM while PSME=1 has no effect (HUM Note 2). */
  const ra8_err_t drain_err = internal_mdio_drain(reg);
  if (drain_err != k_ra8_ok) {
    ra8_log_error(s_tag, "mdio_c22_write: pre-drain timeout");
    return drain_err;
  }
  internal_mpsm_issue(reg, phy_addr, reg_addr, k_ra8_rmac_mdio_op_c22_write, value, false);
  return internal_mdio_wait(reg, k_ra8_rmac_mmis1_pwacs);
}

ra8_err_t ra8_rmac_mdio_c45_read(ra8_rmac_port_t port,
                                 uint8_t         phy_addr,
                                 uint8_t         dev_addr,
                                 uint16_t        reg_addr,
                                 uint16_t*       out_value)
{
  RA8_CHECK_NULL_PTR(out_value, s_tag, "mdio_c45_read: out_value must not be nullptr");
  if (!internal_port_ok(port)) {
    ra8_log_error(s_tag, "mdio_c45_read: port out of range");
    return k_ra8_err_invalid_arg;
  }

  volatile r_rmac_regs_t* reg = ra8_rmac(port);
  /* C45 step 1: address frame loads the 16-bit MMD register address. */
  ra8_err_t err = internal_mdio_drain(reg);
  if (err != k_ra8_ok) {
    ra8_log_error(s_tag, "mdio_c45_read: addr pre-drain timeout"); /* GCOVR_EXCL_BR_LINE -- stuck */
    return err;
  }
  internal_mpsm_issue(reg, phy_addr, dev_addr, k_ra8_rmac_mdio_op_c45_address, reg_addr, true);
  err = internal_mdio_wait(reg, k_ra8_rmac_mmis1_paacs);
  if (err != k_ra8_ok) {
    ra8_log_error(s_tag, "mdio_c45_read: address timeout"); /* GCOVR_EXCL_BR_LINE -- MDIO timeout */
    return err;
  }
  /* C45 step 2: read frame fetches the 16-bit data. */
  err = internal_mdio_drain(reg);
  if (err != k_ra8_ok) {
    ra8_log_error(s_tag, "mdio_c45_read: read pre-drain timeout");
    return err;
  }
  internal_mpsm_issue(reg, phy_addr, dev_addr, k_ra8_rmac_mdio_op_c45_read, 0U, true);
  err = internal_mdio_wait(reg, k_ra8_rmac_mmis1_pracs);
  if (err != k_ra8_ok) {
    ra8_log_error(s_tag, "mdio_c45_read: read timeout"); /* GCOVR_EXCL_BR_LINE -- no stuck-status */
    return err;
  }
  /* HUM Ch 33.4.1.1 "MPSM : PHY Station Management Register" p 1707 */
  *out_value =
    (uint16_t)((reg->MPSM >> (uint32_t)k_ra8_rmac_shift_mpsm_prd) & k_ra8_rmac_mask_mpsm_data);
  return k_ra8_ok;
}

ra8_err_t ra8_rmac_mdio_c45_write(ra8_rmac_port_t port,
                                  uint8_t         phy_addr,
                                  uint8_t         dev_addr,
                                  uint16_t        reg_addr,
                                  uint16_t        value)
{
  if (!internal_port_ok(port)) {
    ra8_log_error(s_tag, "mdio_c45_write: port out of range");
    return k_ra8_err_invalid_arg;
  }
  volatile r_rmac_regs_t* reg = ra8_rmac(port);
  /* Address frame followed by write frame -- both must complete. */
  ra8_err_t err = internal_mdio_drain(reg);
  if (err != k_ra8_ok) {
    ra8_log_error(s_tag, "mdio_c45_write: addr pre-drain timeout"); /* GCOVR_EXCL_BR_LINE -- MDIO */
    return err;
  }
  internal_mpsm_issue(reg, phy_addr, dev_addr, k_ra8_rmac_mdio_op_c45_address, reg_addr, true);
  err = internal_mdio_wait(reg, k_ra8_rmac_mmis1_paacs);
  if (err != k_ra8_ok) {
    ra8_log_error(s_tag, "mdio_c45_write: address timeout"); /* GCOVR_EXCL_BR_LINE -- timeout */
    return err;
  }
  err = internal_mdio_drain(reg);
  if (err != k_ra8_ok) {
    ra8_log_error(s_tag, "write pre-drain timeout"); /* GCOVR_EXCL_BR_LINE -- MDIO never stalls */
    return err;
  }
  internal_mpsm_issue(reg, phy_addr, dev_addr, k_ra8_rmac_mdio_op_c45_write, value, true);
  return internal_mdio_wait(reg, k_ra8_rmac_mmis1_pwacs);
}

ra8_err_t ra8_rmac_attach_handler(ra8_rmac_port_t port, ra8_rmac_event_fn_t cb, void* ctx)
{
  if (!internal_port_ok(port)) {
    ra8_log_error(s_tag, "attach_handler: port out of range");
    return k_ra8_err_invalid_arg;
  }
  s_slots[(uint8_t)port].cb  = cb;
  s_slots[(uint8_t)port].ctx = ctx;
  return k_ra8_ok;
}

RA8_ISR_SAFE
void ra8_rmac_dispatch(ra8_rmac_port_t port)
{
  if (!internal_port_ok(port)) {
    return;
  }
  volatile r_rmac_regs_t* reg = ra8_rmac(port);
  /* HUM Ch 33.4 "MEIS / MMIS0 / MMIS1 / MMIS2" p 1706 */
  const uint32_t            err = reg->MEIS;
  const uint32_t            m0  = reg->MMIS0;
  const uint32_t            m1  = reg->MMIS1;
  const uint32_t            m2  = reg->MMIS2;
  const ra8_rmac_event_fn_t fn  = s_slots[(uint8_t)port].cb;
  void* const               ctx = s_slots[(uint8_t)port].ctx;
  /* HUM Ch 33.4 "MEID / MMID0 / MMID1 / MMID2" p 1706 */
  reg->MEID  = err;
  reg->MMID0 = m0;
  reg->MMID1 = m1;
  reg->MMID2 = m2;
  reg->MEIS  = 0U;
  reg->MMIS0 = 0U;
  reg->MMIS1 = 0U;
  reg->MMIS2 = 0U;
  if (fn != nullptr) {
    fn(ctx, port, err, m0, m1, m2);
  }
}
