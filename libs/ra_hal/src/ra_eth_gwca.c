/**
 * @file ra_eth_gwca.c
 * @brief Ethernet CPU Agent driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * driver for the RA8D2 GWCA block. Every register access
 * carries a HUM Ch 34 citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_eth_gwca.h"

#include <stdint.h>
#include <string.h>

#include "ra8d2_ether_regs.h"
#include "ra8d2_mstp_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "ETHGWC";

static ra_eth_gwca_event_fn_t s_gwca_fn;
static void*                  s_gwca_ctx;

/**
 * @enum ra_eth_gwca_init_layout_t
 * @brief MFWD FWPC10/11/12 offsets used to enable extended descriptors.
 *
 * @details Per FSP r_layer3_switch open path, every agent (GWCA, ETHA0,
 * ETHA1) needs FWPC1n.DDE = 1 BEFORE the GWCA mode transitions, or
 * GWARIRM.ARR never asserts during the AXI init handshake (the chip
 * silently rejects the request). Bench-confirmed on EK-RA8D2.
 */
typedef enum : uint32_t {
  k_ra_mfwd_off_fwpc10 = 0x104UL, /**< FWPC10 (GWCA agent).  */
  k_ra_mfwd_off_fwpc11 = 0x114UL, /**< FWPC11 (ETHA0 agent). */
  k_ra_mfwd_off_fwpc12 = 0x124UL, /**< FWPC12 (ETHA1 agent). */
  k_ra_mfwd_fwpc_dde   = 0x1UL,   /**< DDE bit position 0.   */
} ra_eth_gwca_init_layout_t;

/* Implementation of ra_eth_gwca_init (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_eth_gwca_init(void)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_eswm);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "gwca_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  volatile r_gwca_regs_t* reg = ra_gwca();
  /* HUM Ch 34 "Ethernet CPU Agent (GWCA)" p 1787 */
  reg->GWCA_CTRL = 0U;
  reg->GWCA_STS  = 0U;
  reg->GWCA_IE   = 0U;
  reg->GWCA_ICLR = 0U;

  /* Enable extended descriptor format on each agent before any GWCA
   * mode transition. Without this the AXI init handshake (GWARIRM.ARR)
   * never asserts. Mirrors FSP r_layer3_switch open path. */
  /* HUM Ch 30 "Ethernet Message Forwarding Engine (MFWD)" p 1321 */
  volatile uint32_t* const fwpc10 =
      (volatile uint32_t*)(k_ra_mfwd_base_addr + (uintptr_t)k_ra_mfwd_off_fwpc10);
  volatile uint32_t* const fwpc11 =
      (volatile uint32_t*)(k_ra_mfwd_base_addr + (uintptr_t)k_ra_mfwd_off_fwpc11);
  volatile uint32_t* const fwpc12 =
      (volatile uint32_t*)(k_ra_mfwd_base_addr + (uintptr_t)k_ra_mfwd_off_fwpc12);
  *fwpc10 = (*fwpc10 & ~(uint32_t)k_ra_mfwd_fwpc_dde) | (uint32_t)k_ra_mfwd_fwpc_dde;
  *fwpc11 = (*fwpc11 & ~(uint32_t)k_ra_mfwd_fwpc_dde) | (uint32_t)k_ra_mfwd_fwpc_dde;
  *fwpc12 = (*fwpc12 & ~(uint32_t)k_ra_mfwd_fwpc_dde) | (uint32_t)k_ra_mfwd_fwpc_dde;

  ra_log_info(s_tag, "gwca_init");
  return k_ra_ok;
}

/* Implementation of ra_eth_gwca_deinit (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_eth_gwca_deinit(void)
{
  volatile r_gwca_regs_t* reg = ra_gwca();
  /* HUM Ch 34 "Ethernet CPU Agent (GWCA)" p 1787 */
  reg->GWCA_CTRL = 0U;
  reg->GWCA_IE   = 0U;
  s_gwca_fn      = nullptr;
  s_gwca_ctx     = nullptr;
  return ra_mstp_disable(k_ra_mstp_eswm);
}

/* Implementation of ra_eth_gwca_get_status (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_eth_gwca_get_status(uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 34 "Ethernet CPU Agent (GWCA)" p 1787 */
  *out_mask = ra_gwca()->GWCA_STS;
  return k_ra_ok;
}

/* Implementation of ra_eth_gwca_clear_status (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_eth_gwca_clear_status(uint32_t mask)
{
  volatile r_gwca_regs_t* reg = ra_gwca();
  /* HUM Ch 34 "Ethernet CPU Agent (GWCA)" p 1787 */
  reg->GWCA_ICLR = mask;
  reg->GWCA_STS  = reg->GWCA_STS & ~mask;
  return k_ra_ok;
}

/* Implementation of ra_eth_gwca_attach_handler (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_eth_gwca_attach_handler(ra_eth_gwca_event_fn_t fn, void* ctx)
{
  s_gwca_fn  = fn;
  s_gwca_ctx = ctx;
  return k_ra_ok;
}

/* Implementation of ra_eth_gwca_dispatch (see header for full contract) -- see header for the documented contract. */
void ra_eth_gwca_dispatch(void)
{
  volatile r_gwca_regs_t* reg = ra_gwca();
  /* HUM Ch 34 "Ethernet CPU Agent (GWCA)" p 1787 */
  const uint32_t               mask = reg->GWCA_STS;
  const ra_eth_gwca_event_fn_t fn   = s_gwca_fn;
  void* const                  ctx  = s_gwca_ctx;
  reg->GWCA_ICLR                    = mask;
  reg->GWCA_STS                     = 0U;
  if (fn != nullptr) {
    fn(ctx, mask);
  }
}

/* Implementation of ra_eth_gwca_enter_stop (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_eth_gwca_enter_stop(void)
{
  /* HUM Ch 34 "Ethernet CPU Agent (GWCA)" p 1787 */
  ra_gwca()->GWCA_CTRL = 0U;
  return ra_mstp_disable(k_ra_mstp_eswm);
}

/* Implementation of ra_eth_gwca_exit_stop (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_eth_gwca_exit_stop(void)
{
  return ra_mstp_enable(k_ra_mstp_eswm);
}

/**
 * @brief Bounded poll budget for the GWMS.OPS / GWARIRM.ARR convergence.
 *
 * @details ~10 ms ceiling at 1 GHz CPU with ~5 cycles per iter. The
 * state machine transitions in FSP take a handful of CANFDCLK ticks
 * on real silicon. On host (RA_SIMULATOR_MODE) the loop runs against
 * the mmap'd peri region and the bits flip immediately, so the
 * budget is mostly hit-once.
 */
enum : uint32_t {
  k_ra_eth_gwca_mode_spin = 2000000UL,
  k_ra_eth_gwca_balr_spin = 2000000UL, /**< GWDCCi.BALR self-clear poll budget. */
};

/**
 * @var g_ra_eth_gwca_open_step
 * @brief Bench-side debug trail bumped by ::ra_eth_gwca_default_open.
 *
 * @details Read externally via J-Link `mem32` when the chip parks in
 * panic_halt to identify which sub-primitive failed:
 *   1 = internal_default_open_pre ok (init + rings + bring_up + CONFIG).
 *   2 = internal_default_open_queues ok (RX/TX cfgs written).
 *   3 = set_operation_mode(OPERATION) ok (final).
 * Plus the symmetric error codes ``0x10|N`` for failures at step N.
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_ra_eth_gwca_open_step;

/**
 * @var g_ra_eth_gwca_pre_step
 * @brief Bench-side debug trail bumped inside ::internal_default_open_pre.
 *
 * @details Lets a JTAG-attached operator identify which sub-primitive
 * inside the pre-phase failed when ``g_ra_eth_gwca_open_step == 0x11``:
 *   1 = ra_eth_gwca_init ok.
 *   2 = internal_default_open_rings ok.
 *   3 = ra_eth_gwca_bring_up ok.
 *   4 = set_operation_mode(CONFIG) ok.
 * Error codes ``0x10|N`` for failures at step N.
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_ra_eth_gwca_pre_step;

/**
 * @var g_ra_eth_gwca_bring_up_step
 * @brief Bench-side debug trail bumped inside ::ra_eth_gwca_bring_up.
 *
 * @details Pinpoints which of the six sub-steps failed:
 *   1 = first DISABLE ok.
 *   2 = DISABLE -> CONFIG ok.
 *   3 = AXI init ok (GWARIRM handshake).
 *   4 = install_linkfix ok.
 *   5 = second DISABLE ok.
 *   6 = DISABLE -> OPERATION ok.
 * Error codes ``0x10|N`` for failures at step N.
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_ra_eth_gwca_bring_up_step;

/**
 * @enum ra_eth_gwca_step_t
 * @brief Step values bumped through ::g_ra_eth_gwca_open_step,
 *        ::g_ra_eth_gwca_pre_step, and ::g_ra_eth_gwca_bring_up_step.
 *
 * @details Successful progression uses values 0..6. Failure codes are
 * ``0x10 | N`` so a JTAG-attached operator can tell at a glance whether
 * the chip parked on a happy-path step or an error path.
 */
typedef enum : uint32_t {
  k_ra_eth_gwca_step_ok_1   = 1U,
  k_ra_eth_gwca_step_ok_2   = 2U,
  k_ra_eth_gwca_step_ok_3   = 3U,
  k_ra_eth_gwca_step_ok_4   = 4U,
  k_ra_eth_gwca_step_ok_5   = 5U,
  k_ra_eth_gwca_step_ok_6   = 6U,
  k_ra_eth_gwca_step_fail_1 = 0x11U,
  k_ra_eth_gwca_step_fail_2 = 0x12U,
  k_ra_eth_gwca_step_fail_3 = 0x13U,
  k_ra_eth_gwca_step_fail_4 = 0x14U,
  k_ra_eth_gwca_step_fail_5 = 0x15U,
  k_ra_eth_gwca_step_fail_6 = 0x16U,
} ra_eth_gwca_step_t;

/* GWMC.OPC field at bits [1:0], GWMS.OPS field at bits [1:0]. The
 * full register layout matches FSP R_GWCA0_Type from the cloned
 * Renesas FSP source. */

/**
 * @brief Transition the GWCA / ESWM state machine to a new OPC mode.
 *
 * @details See header for the canonical contract -- writes GWMC.OPC
 * and polls GWMS.OPS until convergence. Mirrors FSP
 * r_layer3_switch_update_gwca_operation_mode.
 *
 * @param[in] mode Target operation mode from ::ra_gwmc_opc_t.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              GWMS.OPS now reflects @p mode.
 * @retval k_ra_err_invalid_arg @p mode is out of range.
 * @retval k_ra_err_hw_timeout  GWMS.OPS never converged.
 *
 * @pre Module brought up via ::ra_eth_gwca_init.
 * @pre Caller is single-threaded with respect to GWCA edits.
 * @post On success GWMC.OPC and GWMS.OPS both equal @p mode.
 * @post On timeout GWMC.OPC may have been written even if OPS
 *       never converged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_eth_gwca_set_operation_mode(ra_gwmc_opc_t mode)
{
  if ((uint32_t)mode > (uint32_t)k_ra_gwmc_opc_mask) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 34.3.1 "GWMC : Mode Configuration Register" p 1797 */
  volatile uint32_t* const gwmc = (volatile uint32_t*)(k_ra_gwca0_base_addr + (uintptr_t)k_ra_gwca_off_gwmc);
  /* HUM Ch 34.3.2 "GWMS : Mode Status Register" p 1798 */
  volatile uint32_t* const gwms = (volatile uint32_t*)(k_ra_gwca0_base_addr + (uintptr_t)k_ra_gwca_off_gwms);

  const uint32_t opc = (uint32_t)mode & (uint32_t)k_ra_gwmc_opc_mask;
  const uint32_t cur = *gwmc & ~(uint32_t)k_ra_gwmc_opc_mask;
  *gwmc              = cur | opc;

#ifndef RA_SIMULATOR_MODE
  for (uint32_t i = 0U; i < k_ra_eth_gwca_mode_spin; ++i) { /* GCOVR_EXCL_BR_LINE */
    if ((*gwms & (uint32_t)k_ra_gwmc_opc_mask) == opc) {   /* GCOVR_EXCL_BR_LINE */
      return k_ra_ok;
    }
  }
  ra_log_error(s_tag, "set_operation_mode: GWMS.OPS never converged");
  return k_ra_err_hw_timeout;
#else
  return k_ra_ok;
#endif
}

/**
 * @brief Request the GWCA AXI bridge to initialize via GWARIRM.ARIOG.
 *
 * @details See header for the canonical contract -- asserts
 * GWARIRM.ARIOG and polls GWARIRM.ARR until 1. Mirrors the AXI
 * init step inside FSP r_layer3_switch_initialize_gwca.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              ARR asserted within the budget.
 * @retval k_ra_err_hw_timeout  ARR never asserted.
 *
 * @pre ::ra_eth_gwca_set_operation_mode(k_ra_gwmc_opc_config) returned ok.
 * @pre Caller is single-threaded with respect to GWCA edits.
 * @post On success ARR=1 indicates the AXI manager is ready.
 * @post On failure ARR may still read 0; caller retries.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_eth_gwca_axi_init(void)
{
  /* HUM Ch 34.3.x "GWARIRM" -- the AXI Manager (vendor name uses
   * the older M-word) Initialization Request register. ARIOG bit is
   * the request, ARR bit is the response. Sequence per FSP
   * r_layer3_switch_initialize_gwca: set ARIOG=1, poll ARR until 1. */
  enum : uint32_t {
    k_ra_gwarirm_ariog = 1UL << 0,
    k_ra_gwarirm_arr   = 1UL << 1,
  };
  volatile uint32_t* const gwarirm =
    (volatile uint32_t*)(k_ra_gwca0_base_addr + (uintptr_t)k_ra_gwca_off_gwarirm);
  *gwarirm = k_ra_gwarirm_ariog;

#ifndef RA_SIMULATOR_MODE
  for (uint32_t i = 0U; i < k_ra_eth_gwca_mode_spin; ++i) { /* GCOVR_EXCL_BR_LINE */
    if ((*gwarirm & k_ra_gwarirm_arr) != 0U) {              /* GCOVR_EXCL_BR_LINE */
      return k_ra_ok;
    }
  }
  ra_log_error(s_tag, "axi_init: GWARIRM.ARR never asserted");
  return k_ra_err_hw_timeout;
#else
  return k_ra_ok;
#endif
}

/**
 * @brief Install a fresh LINKFIX table at GWDCBAC0/1.
 *
 * @details See header for the canonical contract -- zeros every
 * LINKFIX entry, sets each entry's descriptor type to LEMPTY, then
 * writes the table address into GWDCBAC0 (upper byte) + GWDCBAC1
 * (lower 32 bits).
 *
 * @param[in,out] linkfix_table Caller-owned table; written to LEMPTY.
 * @param[in]     entry_count   Number of queues to cover (max 32).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              GWDCBAC0/1 programmed.
 * @retval k_ra_err_invalid_arg ``linkfix_table`` is null or count > 32.
 *
 * @pre Caller has invoked ::ra_eth_gwca_set_operation_mode
 *      with k_ra_gwmc_opc_config.
 * @pre Caller has invoked ::ra_eth_gwca_axi_init.
 * @post Every LINKFIX entry has dt = k_ra_gwdcc_dt_lempty.
 * @post GWDCBAC0 + GWDCBAC1 point at ``linkfix_table``.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_eth_gwca_install_linkfix(ra_gwca_basic_descriptor_t* linkfix_table,
                                     uint32_t                    entry_count)
{
  RA_CHECK_NULL_PTR(linkfix_table, s_tag, "install_linkfix: table must not be null");
  enum : uint32_t { k_ra_gwca_linkfix_max_entries = 32U };
  if (entry_count == 0U || entry_count > k_ra_gwca_linkfix_max_entries) {
    ra_log_error(s_tag, "install_linkfix: entry_count out of range");
    return k_ra_err_invalid_arg;
  }

  /* Initialize every entry to LEMPTY (queue disabled). The chip
   * walks the LINKFIX table at queue-activation time, so any entry
   * not explicitly populated with a real chain head must look
   * disabled instead of pointing at random memory. */
  (void)memset(linkfix_table, 0, (size_t)entry_count * sizeof(ra_gwca_basic_descriptor_t));
  for (uint32_t i = 0U; i < entry_count; ++i) {
    linkfix_table[i].dt = (uint8_t)k_ra_gwdcc_dt_lempty;
  }

  /* HUM Ch 34.3.7.2 / 34.3.7.3 GWDCBAC0/1: split the table's
   * 40-bit address into PTR[39:32] (GWDCBAC0.DCBAU) and PTR[31:0]
   * (GWDCBAC1.DCBAL). On this 32-bit MCU the upper byte is always
   * zero, but the bit-field exists for the device family that uses
   * 40-bit addresses. */
  const uintptr_t          addr     = (uintptr_t)linkfix_table;
  enum : uintptr_t {
    k_ra_linkfix_upper_shift = 32U,
    k_ra_linkfix_upper_mask  = 0xFFULL,
    k_ra_linkfix_lower_mask  = 0xFFFFFFFFULL,
  };
  const uint32_t           upper    = (uint32_t)((uint64_t)addr >> k_ra_linkfix_upper_shift) & k_ra_linkfix_upper_mask;
  const uint32_t           lower    = (uint32_t)addr & k_ra_linkfix_lower_mask;
  volatile uint32_t* const gwdcbac0 =
    (volatile uint32_t*)(k_ra_gwca0_base_addr + (uintptr_t)k_ra_gwca_off_gwdcbac0);
  volatile uint32_t* const gwdcbac1 =
    (volatile uint32_t*)(k_ra_gwca0_base_addr + (uintptr_t)k_ra_gwca_off_gwdcbac1);
  *gwdcbac0 = upper;
  *gwdcbac1 = lower;
  return k_ra_ok;
}

/**
 * @brief Bring the GWCA up to OPERATION with a fresh LINKFIX table.
 *
 * @details See header for the canonical contract. Sequence:
 *   1. set_operation_mode(DISABLE)
 *   2. set_operation_mode(CONFIG)
 *   3. axi_init()
 *   4. install_linkfix(table, n)
 *   5. set_operation_mode(DISABLE)
 *   6. set_operation_mode(OPERATION)
 *
 * @param[in,out] linkfix_table Caller-owned LINKFIX table.
 * @param[in]     entry_count   Queue count (1..32).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              GWCA in OPERATION mode; LINKFIX live.
 * @retval k_ra_err_invalid_arg Table is null or count out of range.
 * @retval k_ra_err_hw_timeout  A state-machine transition never converged.
 *
 * @pre Module brought up via ::ra_eth_gwca_init.
 * @pre Caller is single-threaded with respect to GWCA edits.
 * @post On success GWMS.OPS = OPERATION.
 * @post On failure GWMS.OPS = DISABLE (best-effort park state).
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
/**
 * @brief DISABLE -> CONFIG -> axi_init -> install_linkfix bring-up phase.
 *
 * @details Helper extracted from ::ra_eth_gwca_bring_up so the top
 * level stays under the 40-statement size budget. Walks the first
 * four bring-up sub-steps; on any failure bumps
 * ``g_ra_eth_gwca_bring_up_step`` to ``0x1N`` and pulls the chip
 * back to DISABLE before propagating the error.
 *
 * @param[in,out] linkfix_table LINKFIX table backing storage.
 * @param[in]     entry_count   Number of entries in linkfix_table.
 *
 * @return ra_err_t Error code from the first failing sub-call.
 * @retval k_ra_ok             Chip in CONFIG, AXI ready, LINKFIX installed.
 * @retval k_ra_err_hw_timeout A mode-transition or AXI handshake timed out.
 *
 * @pre Caller holds the GWCA serialization invariant.
 * @pre ``linkfix_table`` is non-null when entry_count > 0.
 * @post On success GWMC.OPC == CONFIG, GWARIRM.ARR == 1.
 * @post On failure GWMC.OPC == DISABLE.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t internal_bring_up_to_config(ra_gwca_basic_descriptor_t* linkfix_table,
                                            uint32_t                    entry_count)
{
  ra_err_t err = ra_eth_gwca_set_operation_mode(k_ra_gwmc_opc_disable);
  if (err != k_ra_ok) {
    g_ra_eth_gwca_bring_up_step = (uint32_t)k_ra_eth_gwca_step_fail_1;
    return err;
  }
  g_ra_eth_gwca_bring_up_step = (uint32_t)k_ra_eth_gwca_step_ok_1;
  err                         = ra_eth_gwca_set_operation_mode(k_ra_gwmc_opc_config);
  if (err != k_ra_ok) {
    g_ra_eth_gwca_bring_up_step = (uint32_t)k_ra_eth_gwca_step_fail_2;
    (void)ra_eth_gwca_set_operation_mode(k_ra_gwmc_opc_disable);
    return err;
  }
  g_ra_eth_gwca_bring_up_step = (uint32_t)k_ra_eth_gwca_step_ok_2;
  err                         = ra_eth_gwca_axi_init();
  if (err != k_ra_ok) {
    g_ra_eth_gwca_bring_up_step = (uint32_t)k_ra_eth_gwca_step_fail_3;
    (void)ra_eth_gwca_set_operation_mode(k_ra_gwmc_opc_disable);
    return err;
  }
  g_ra_eth_gwca_bring_up_step = (uint32_t)k_ra_eth_gwca_step_ok_3;
  err                         = ra_eth_gwca_install_linkfix(linkfix_table, entry_count);
  if (err != k_ra_ok) {
    g_ra_eth_gwca_bring_up_step = (uint32_t)k_ra_eth_gwca_step_fail_4;
    (void)ra_eth_gwca_set_operation_mode(k_ra_gwmc_opc_disable);
    return err;
  }
  g_ra_eth_gwca_bring_up_step = (uint32_t)k_ra_eth_gwca_step_ok_4;
  return k_ra_ok;
}

/**
 * @brief Six-step GWCA bring-up: RESET -> DISABLE -> CONFIG -> AXI ->
 *        install_linkfix -> DISABLE -> OPERATION.
 *
 * @details See header for the canonical contract. Calls
 * ::internal_bring_up_to_config for the first four sub-steps then
 * walks DISABLE -> OPERATION. ``g_ra_eth_gwca_bring_up_step`` is
 * bumped along the way so a JTAG-attached operator can identify
 * the failing sub-step via mem32.
 *
 * @param[in,out] linkfix_table Pointer to caller-owned LINKFIX storage.
 * @param[in]     entry_count   Number of entries (1..k_ra_gwca_linkfix_max).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              Chip in OPERATION mode.
 * @retval k_ra_err_invalid_arg linkfix_table null or entry_count invalid.
 * @retval k_ra_err_hw_timeout  A mode transition or AXI handshake timed out.
 *
 * @pre Caller is single-threaded with respect to the GWCA.
 * @pre ra_eth_gwca_init has succeeded.
 * @post On success GWMC.OPC == OPERATION and GWARIRM.ARR == 1.
 * @post On failure GWMC.OPC == DISABLE.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_eth_gwca_bring_up(ra_gwca_basic_descriptor_t* linkfix_table, uint32_t entry_count)
{
  g_ra_eth_gwca_bring_up_step = 0U;
  const ra_err_t phase1_err   = internal_bring_up_to_config(linkfix_table, entry_count);
  if (phase1_err != k_ra_ok) {
    return phase1_err;
  }
  const ra_err_t dis_err = ra_eth_gwca_set_operation_mode(k_ra_gwmc_opc_disable);
  if (dis_err != k_ra_ok) {
    g_ra_eth_gwca_bring_up_step = (uint32_t)k_ra_eth_gwca_step_fail_5;
    return dis_err;
  }
  g_ra_eth_gwca_bring_up_step = (uint32_t)k_ra_eth_gwca_step_ok_5;
  const ra_err_t op_err       = ra_eth_gwca_set_operation_mode(k_ra_gwmc_opc_operation);
  if (op_err != k_ra_ok) {
    g_ra_eth_gwca_bring_up_step = (uint32_t)k_ra_eth_gwca_step_fail_6;
    return op_err;
  }
  g_ra_eth_gwca_bring_up_step = (uint32_t)k_ra_eth_gwca_step_ok_6;
  return k_ra_ok;
}

/**
 * @brief Wire a per-queue config into GWDCC[i] + LINKFIX[i].
 *
 * @details See header for the canonical contract. Composes the
 * GWDCC value from cfg's SM / DQT / DCP / SL bits, writes it, then
 * promotes the LINKFIX entry from LEMPTY to LINKFIX with PTR
 * pointing at the queue's chain head.
 *
 * @param[in,out] linkfix_table Same table passed to install_linkfix.
 * @param[in]     queue_index   Queue 0..31.
 * @param[in]     cfg           Per-queue config (port, priority, dir, head).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              GWDCC[i] + LINKFIX[i] wired.
 * @retval k_ra_err_invalid_arg Null pointer or queue out of range.
 *
 * @pre ::ra_eth_gwca_install_linkfix returned ok.
 * @pre Caller is in GWMC.OPC = CONFIG.
 * @post GWDCC[queue_index] reflects cfg's settings.
 * @post linkfix_table[queue_index] has dt = LINKFIX with PTR = chain_head.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
/**
 * @brief Compose the GWDCC[i] 32-bit value from a queue config.
 *
 * @details Pure value composition: DQT + DCP[18:16] + SL.
 *
 * GWDCC.SM[1:0] (Synchronization Mode) is left at 00b -- "Normal mode
 * (full descriptor write back)" per HUM Ch 34.3 "GWDCCi" p 9060. SM
 * is NOT a source-MAC field: 01b is "No-write-back mode (no
 * descriptor write back)", which would leave every RX descriptor
 * stuck at FEMPTY because the GWCA never updates it. Bench-confirmed
 * on EK-RA8D2. EDE/ETS stay 0 (basic descriptors), BALR stays 0
 * (default AXI burst), OSID stays 0 (default stream). No MMIO
 * touched.
 *
 * @param[in] cfg Per-queue config.
 * @return Packed GWDCC value with DQT, SL, DCP bits set per cfg.
 * @retval value Packed register word.
 * @pre cfg is non-null with priority <= 7.
 * @pre Caller validated the bounds before invoking.
 * @post Returned value reflects the cfg fields; SM stays 00b.
 * @post No global state is modified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static uint32_t internal_compose_gwdcc(const ra_eth_gwca_queue_cfg_t* cfg)
{
  /* GWDCC value: DQT + DCP[18:16] + SL. SM[1:0] stays 00b (Normal
   * mode -- full descriptor write-back). EDE/ETS stay 0 (basic
   * descriptors), BALR stays 0 (default AXI burst), OSID 0. */
  uint32_t value = 0U;
  if (cfg->is_tx) {
    value |= (uint32_t)k_ra_gwdcc_dqt;
  }
  if (cfg->stop_on_last) {
    value |= (uint32_t)k_ra_gwdcc_sl;
  }
  value |= (((uint32_t)cfg->priority << k_ra_gwdcc_dcp_shift) & k_ra_gwdcc_dcp_mask);
  return value;
}

/**
 * @brief Promote a LINKFIX entry from LEMPTY to LINKFIX with chain-head PTR.
 *
 * @details Splits the 40-bit chain-head address into ptr_h (high 8
 * bits) + ptr_l (low 32 bits) and writes them with dt = LINKFIX.
 * No MMIO is touched -- caller-owned table memory only.
 *
 * @param[in,out] entry      LINKFIX entry to rewrite.
 * @param[in]     chain_head Address to encode into the 40-bit PTR field.
 * @pre entry is non-null and previously initialised by install_linkfix.
 * @pre chain_head is the head of the queue's descriptor array.
 * @post entry->dt == LINKFIX; ptr_h/ptr_l carry the 40-bit address.
 * @post No other LINKFIX entries are modified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void internal_set_linkfix_entry(ra_gwca_basic_descriptor_t*       entry,
                                       const ra_gwca_basic_descriptor_t* chain_head)
{
  enum : uintptr_t {
    k_ra_linkfix_ptr_upper_shift = 32U,
    k_ra_linkfix_ptr_upper_mask  = 0xFFULL,
    k_ra_linkfix_ptr_lower_mask  = 0xFFFFFFFFULL,
  };
  const uintptr_t head_addr = (uintptr_t)chain_head;
  entry->dt                 = (uint8_t)k_ra_gwdcc_dt_linkfix;
  entry->ptr_h              = (uint8_t)((uint64_t)head_addr >> k_ra_linkfix_ptr_upper_shift)
                 & k_ra_linkfix_ptr_upper_mask;
  entry->ptr_l = (uint32_t)head_addr & k_ra_linkfix_ptr_lower_mask;
}

/**
 * @brief Wire a per-queue config into GWDCC[i] + LINKFIX[i].
 *
 * @details See header for the canonical contract. Composes the
 * GWDCC value from cfg's DQT / DCP / SL bits via
 * internal_compose_gwdcc, writes it to GWDCC[queue_index], then
 * promotes the LINKFIX entry to LINKFIX via
 * internal_set_linkfix_entry.
 *
 * @param[in,out] linkfix_table Same table passed to install_linkfix.
 * @param[in]     queue_index    Queue 0..31.
 * @param[in]     cfg            Per-queue config (priority, dir, head).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              GWDCC[i] + LINKFIX[i] wired.
 * @retval k_ra_err_invalid_arg Null pointer or queue out of range.
 * @retval k_ra_err_null_ptr    linkfix_table, cfg, or cfg->chain_head is null.
 *
 * @pre ::ra_eth_gwca_install_linkfix returned ok.
 * @pre Caller is in GWMC.OPC = CONFIG.
 * @post GWDCC[queue_index] reflects cfg's settings.
 * @post linkfix_table[queue_index] has dt = LINKFIX with PTR = chain_head.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_eth_gwca_configure_queue(ra_gwca_basic_descriptor_t*    linkfix_table,
                                     uint32_t                       queue_index,
                                     const ra_eth_gwca_queue_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(linkfix_table, s_tag, "configure_queue: table null");
  RA_CHECK_NULL_PTR(cfg, s_tag, "configure_queue: cfg null");
  RA_CHECK_NULL_PTR(cfg->chain_head, s_tag, "configure_queue: chain_head null");
  enum : uint8_t {
    k_ra_gwdcc_dcp_max = 7U, /**< DCP field width 3 bits -> max value 7. */
  };
  if (cfg->priority > k_ra_gwdcc_dcp_max) {
    return k_ra_err_invalid_arg;
  }
  volatile uint32_t* const gwdcc = ra_gwca_gwdcc(queue_index);
  if (gwdcc == nullptr) {
    return k_ra_err_invalid_arg;
  }
  *gwdcc = internal_compose_gwdcc(cfg);
  internal_set_linkfix_entry(&linkfix_table[queue_index], cfg->chain_head);
  return k_ra_ok;
}

/**
 * @brief Reload a descriptor queue: pulse GWDCC[i].BALR, wait for clear.
 *
 * @details HUM Ch 34.3 "GWDCCi" p 1811 defines BALR (Base Address
 * Load Request) as the request that resets the AXI address RAM
 * current_address field for queue i to the chain base
 * ({GWDCBAC} + i x 8). Until BALR runs, the GWCA never scans the
 * descriptor chain -- every RX descriptor stays FEMPTY and no frame
 * is delivered. BALR self-clears once the reload completes. FSP
 * r_layer3_switch.c::R_LAYER3_SWITCH_StartDescriptorQueue performs
 * this same pulse, with the GWCA in OPERATION mode.
 *
 * @param[in] queue_index GWCA descriptor-queue number 0..63.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok             BALR pulsed and self-cleared.
 * @retval k_ra_err_invalid_arg queue_index has no GWDCC register.
 * @retval k_ra_err_hw_timeout  BALR never self-cleared.
 *
 * @pre Caller is in GWMC.OPC = OPERATION.
 * @pre ::ra_eth_gwca_configure_queue ran for queue_index.
 * @post The AXI address RAM current_address for queue_index points at
 *       the chain base.
 * @post GWDCC[queue_index].BALR reads 0.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_eth_gwca_reload_queue(uint32_t queue_index)
{
  volatile uint32_t* const gwdcc = ra_gwca_gwdcc(queue_index);
  if (gwdcc == nullptr) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 34.3 "GWDCCi" p 1811: BALR self-clears once the GWCA has
   * reset the AXI address RAM current_address pointer. */
  *gwdcc |= (uint32_t)k_ra_gwdcc_balr;
#ifndef RA_SIMULATOR_MODE
  for (uint32_t i = 0U; i < k_ra_eth_gwca_balr_spin; ++i) { /* GCOVR_EXCL_BR_LINE */
    if ((*gwdcc & (uint32_t)k_ra_gwdcc_balr) == 0U) {       /* GCOVR_EXCL_BR_LINE */
      return k_ra_ok;
    }
  }
  ra_log_error(s_tag, "reload_queue: GWDCC BALR never cleared");
  return k_ra_err_hw_timeout;
#else
  return k_ra_ok;
#endif
}

/**
 * @brief Initialise a descriptor chain as a ring of FEMPTY slots.
 *
 * @details See header for the canonical contract. Walks chain[],
 * setting every entry except the last to FEMPTY with ds = slot_bytes,
 * and the last entry to LINK with PTR pointing back at chain[0].
 *
 * @param[in,out] chain      Caller-owned descriptor array.
 * @param[in]     ring_depth Number of entries (>= 2).
 * @param[in]     slot_bytes Per-slot buffer size in bytes.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              Ring initialised.
 * @retval k_ra_err_null_ptr    chain is null.
 * @retval k_ra_err_invalid_arg ring_depth < 2 or slot_bytes out of range.
 *
 * @pre Caller is in GWMC.OPC = CONFIG.
 * @pre chain is 8-byte aligned.
 * @post chain[0..ring_depth-2] have dt = FEMPTY, ds = slot_bytes.
 * @post chain[ring_depth-1] has dt = LINK with PTR = &chain[0].
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_eth_gwca_init_ring(ra_gwca_basic_descriptor_t* chain,
                               uint32_t                    ring_depth,
                               uint32_t                    slot_bytes)
{
  RA_CHECK_NULL_PTR(chain, s_tag, "init_ring: chain null");
  enum : uint32_t {
    k_ra_gwca_ring_min_depth  = 2U,    /**< Need at least one FEMPTY + one LINK. */
    k_ra_gwca_ring_max_bytes  = 2048U, /**< HUM DS field is 12 bits (max 2048). */
  };
  if (ring_depth < k_ra_gwca_ring_min_depth || slot_bytes > k_ra_gwca_ring_max_bytes) {
    return k_ra_err_invalid_arg;
  }

  /* FEMPTY data slots: ds carries the buffer size, dt = FEMPTY. */
  enum : uint32_t {
    k_ra_ds_byte_mask  = 0xFFU,  /**< ds_l carries 8 bits.           */
    k_ra_ds_high_shift = 8U,     /**< ds_h packs the upper 4 bits.   */
    k_ra_ds_high_mask  = 0xFU,   /**< ds_h field width 4 bits.       */
  };
  for (uint32_t i = 0U; i < (ring_depth - 1U); ++i) {
    (void)memset(&chain[i], 0, sizeof(ra_gwca_basic_descriptor_t));
    chain[i].dt   = (uint8_t)k_ra_gwdcc_dt_fempty;
    chain[i].ds_l = (uint8_t)(slot_bytes & k_ra_ds_byte_mask);
    chain[i].ds_h = (uint8_t)((slot_bytes >> k_ra_ds_high_shift) & k_ra_ds_high_mask);
    /* ptr_l left at 0 -- caller fills in the per-slot buffer address. */
  }

  /* Last entry: LINK back to chain[0] so the chip wraps. */
  internal_set_linkfix_entry(&chain[ring_depth - 1U], &chain[0]);
  /* Override dt: internal_set_linkfix_entry writes LINKFIX, but for
   * mid-chain wrap we want LINK (interchangeable per HUM Ch
   * 34.5.1.3.2; LINK is the standard chain-continuation type). */
  chain[ring_depth - 1U].dt = (uint8_t)k_ra_gwdcc_dt_link;
  return k_ra_ok;
}

/**
 * @brief Set a descriptor's data-buffer pointer.
 *
 * @details Wraps ::internal_set_linkfix_entry-style address split
 * for external callers wiring buffer pointers into FEMPTY slots.
 *
 * @param[in,out] desc   Descriptor to update.
 * @param[in]     buffer Buffer address to encode.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok           PTR field updated.
 * @retval k_ra_err_null_ptr desc is null.
 *
 * @pre Caller is in GWMC.OPC = CONFIG.
 * @pre desc was previously zeroed (e.g. by init_ring).
 * @post desc->ptr_h / ptr_l encode @p buffer.
 * @post desc->dt / ds / etc unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_eth_gwca_set_descriptor_buffer(ra_gwca_basic_descriptor_t* desc, void* buffer)
{
  RA_CHECK_NULL_PTR(desc, s_tag, "set_descriptor_buffer: desc null");
  enum : uintptr_t {
    k_ra_buf_ptr_upper_shift = 32U,
    k_ra_buf_ptr_upper_mask  = 0xFFULL,
    k_ra_buf_ptr_lower_mask  = 0xFFFFFFFFULL,
  };
  const uintptr_t addr = (uintptr_t)buffer;
  desc->ptr_h          = (uint8_t)((uint64_t)addr >> k_ra_buf_ptr_upper_shift)
                & k_ra_buf_ptr_upper_mask;
  desc->ptr_l = (uint32_t)addr & k_ra_buf_ptr_lower_mask;
  return k_ra_ok;
}

/**
 * @brief Walk a ring and attach per-slot buffers from a static pool.
 *
 * @details See header for the canonical contract. Iterates the
 * FEMPTY slots (chain[0..ring_depth-2]) and sets each PTR to
 * pool + i * slot_bytes via ::ra_eth_gwca_set_descriptor_buffer.
 *
 * @param[in,out] chain      Ring from init_ring.
 * @param[in]     ring_depth Same depth as init_ring.
 * @param[in]     slot_bytes Per-slot buffer size.
 * @param[in,out] pool       Contiguous buffer pool.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              Every FEMPTY slot has its buffer wired.
 * @retval k_ra_err_null_ptr    chain or pool is null.
 * @retval k_ra_err_invalid_arg ring_depth < 2 or slot_bytes == 0.
 *
 * @pre Caller is in GWMC.OPC = CONFIG.
 * @pre Pool spans at least (ring_depth - 1) * slot_bytes bytes.
 * @post Each chain[i].ptr_h/ptr_l (i in [0, ring_depth-1)) encodes
 *       pool + i * slot_bytes.
 * @post Chain[ring_depth-1] (the LINK terminator) is unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_eth_gwca_attach_buffers(ra_gwca_basic_descriptor_t* chain,
                                    uint32_t                    ring_depth,
                                    uint32_t                    slot_bytes,
                                    uint8_t*                    pool)
{
  RA_CHECK_NULL_PTR(chain, s_tag, "attach_buffers: chain null");
  RA_CHECK_NULL_PTR(pool, s_tag, "attach_buffers: pool null");
  if (ring_depth < 2U || slot_bytes == 0U) {
    return k_ra_err_invalid_arg;
  }
  for (uint32_t i = 0U; i < (ring_depth - 1U); ++i) {
    const size_t   slot_offset = (size_t)i * (size_t)slot_bytes;
    const ra_err_t err = ra_eth_gwca_set_descriptor_buffer(&chain[i], &pool[slot_offset]);
    if (err != k_ra_ok) {
      return err;
    }
  }
  return k_ra_ok;
}

/**
 * @brief Kick a TX queue via GWTRC.
 *
 * @details See header. Sets the bit for queue_index in GWTRC0
 * (queues 0..31) or GWTRC1 (queues 32..63). Read-modify-write so
 * other already-pending TX requests on the same 32-queue word are
 * preserved.
 *
 * @param[in] queue_index TX queue 0..63.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              GWTRCi bit set.
 * @retval k_ra_err_invalid_arg queue_index >= 64.
 *
 * @pre Queue's chain has at least one FSINGLE descriptor ready.
 * @pre GWCA is in GWMC.OPC = OPERATION.
 * @post Matching GWTRC bit is 1.
 * @post Other already-pending TX requests on the same word are preserved.
 *
 * @note Not thread-safe across writes to the same GWTRC word.
 * @since 0.1.0
 */
ra_err_t ra_eth_gwca_kick_tx(uint32_t queue_index)
{
  enum : uint32_t {
    k_ra_gwca_max_tx_queues = 64U,
    k_ra_gwca_queues_per_reg = 32U,
  };
  if (queue_index >= k_ra_gwca_max_tx_queues) {
    return k_ra_err_invalid_arg;
  }
  const uintptr_t offset = (queue_index < k_ra_gwca_queues_per_reg)
                             ? (uintptr_t)k_ra_gwca_off_gwtrc0
                             : (uintptr_t)k_ra_gwca_off_gwtrc1;
  volatile uint32_t* const gwtrc = (volatile uint32_t*)(k_ra_gwca0_base_addr + offset);
  const uint32_t bit = 1UL << (queue_index % k_ra_gwca_queues_per_reg);
  *gwtrc             = *gwtrc | bit;
  return k_ra_ok;
}

/**
 * @brief Find the next slot in a ring matching a target descriptor type.
 *
 * @details See header for the canonical contract. Walks
 * chain[start_idx .. ring_depth-2] looking for an entry where
 * dt == match_dt, wrapping around to chain[0..start_idx-1] if
 * needed. Skips chain[ring_depth-1] (the LINK terminator).
 *
 * @param[in]  chain      Ring from init_ring.
 * @param[in]  ring_depth Same depth as init_ring.
 * @param[in]  match_dt   Descriptor type to find (FEMPTY / FSINGLE).
 * @param[in]  start_idx  Slot to start scanning from.
 * @param[out] out_index  First matching slot.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              ``*out_index`` set.
 * @retval k_ra_err_no_data     No slot matched ``match_dt``.
 * @retval k_ra_err_invalid_arg null pointer / ring_depth < 2 /
 *                              start_idx out of range.
 *
 * @pre Caller is in GWMC.OPC = OPERATION.
 * @pre chain has been initialised via init_ring + attach_buffers.
 * @post On success ``*out_index`` < ring_depth - 1.
 * @post On failure ``*out_index`` is unchanged.
 *
 * @note Not thread-safe with concurrent chip-side updates.
 * @since 0.1.0
 */
ra_err_t ra_eth_gwca_find_slot(const ra_gwca_basic_descriptor_t* chain,
                               uint32_t                          ring_depth,
                               ra_gwdcc_dt_t                     match_dt,
                               uint32_t                          start_idx,
                               uint32_t*                         out_index)
{
  RA_CHECK_NULL_PTR(chain, s_tag, "find_slot: chain null");
  RA_CHECK_NULL_PTR(out_index, s_tag, "find_slot: out_index null");
  if (ring_depth < 2U) {
    return k_ra_err_invalid_arg;
  }
  const uint32_t data_slot_count = ring_depth - 1U;
  if (start_idx >= data_slot_count) {
    return k_ra_err_invalid_arg;
  }
  /* Walk from start_idx forward, wrapping once if needed. */
  for (uint32_t i = 0U; i < data_slot_count; ++i) {
    const uint32_t slot = (start_idx + i) % data_slot_count;
    if (chain[slot].dt == (uint8_t)match_dt) {
      *out_index = slot;
      return k_ra_ok;
    }
  }
  return k_ra_err_no_data;
}

/**
 * @brief Decode a descriptor's 40-bit PTR back to a host pointer.
 *
 * @details Reverses the ptr_h / ptr_l split applied by
 * ::ra_eth_gwca_set_descriptor_buffer. On a 32-bit MCU like the
 * RA8D2 the high byte is always zero, but the function handles the
 * 40-bit format generically.
 *
 * @param[in] desc Descriptor whose PTR to decode.
 * @return Host-visible buffer pointer or nullptr if desc is null.
 * @retval pointer Valid buffer address.
 * @pre desc was previously initialised via attach_buffers or
 *      set_descriptor_buffer.
 * @pre desc is non-null.
 * @post Returned pointer matches the address the descriptor encodes.
 * @post No state is modified.
 * @note Pure helper; no thread-safety concerns.
 * @since 0.1.0
 */
static uint8_t* internal_decode_ptr(const ra_gwca_basic_descriptor_t* desc)
{
  if (desc == nullptr) {
    return nullptr;
  }
  /* PTR is 40 bits across ptr_h (high 8) + ptr_l (low 32). On a
   * 32-bit target uintptr_t is 32 bits, so shifting by 32 is UB --
   * promote to uint64_t for the recompose, then back to uintptr_t.
   * On 32-bit chips ptr_h is always zero so the upper byte is
   * harmlessly truncated when we cast back. */
  enum : uint64_t {
    k_ra_ptr_upper_shift = 32ULL,
    k_ra_ptr_low_mask    = 0xFFFFFFFFULL,
  };
  const uint64_t addr64 =
    ((uint64_t)desc->ptr_h << k_ra_ptr_upper_shift) | ((uint64_t)desc->ptr_l & k_ra_ptr_low_mask);
  return (uint8_t*)(uintptr_t)addr64;
}

/**
 * @brief Reconstruct the 12-bit DS (descriptor size) field from a basic descriptor.
 *
 * @details Reverses the ds_l / ds_h split applied when the chain
 * is populated. Used by the RX path to know how many bytes the
 * chip wrote into a buffer before we copy them out.
 *
 * @param[in] desc Descriptor whose ds field to read.
 * @return 12-bit size value.
 * @retval value Reconstructed DS as a uint32_t.
 * @pre desc is non-null and previously initialised.
 * @pre Caller has already validated desc against null.
 * @post Returned value is in [0, 4095].
 * @post No state is modified.
 * @note Pure helper.
 * @since 0.1.0
 */
static uint32_t internal_decode_ds(const ra_gwca_basic_descriptor_t* desc)
{
  enum : uint32_t {
    k_ra_ds_low_mask  = 0xFFU,
    k_ra_ds_high_shift = 8U,
    k_ra_ds_high_mask = 0xFU,
  };
  return ((uint32_t)desc->ds_l & k_ra_ds_low_mask) |
         (((uint32_t)desc->ds_h & k_ra_ds_high_mask) << k_ra_ds_high_shift);
}

/**
 * @brief Enqueue one frame on a TX queue's descriptor ring.
 *
 * @details See header for the canonical contract. Finds the next
 * FEMPTY slot, memcpy's the frame into the slot's pre-attached
 * buffer, sets ds to frame_len, flips dt to FSINGLE, and advances
 * the caller's tail_idx cursor.
 *
 * @param[in,out] chain      TX descriptor ring.
 * @param[in]     ring_depth Ring depth.
 * @param[in,out] tail_idx   Round-robin write cursor.
 * @param[in]     frame      Source bytes.
 * @param[in]     frame_len  Frame length.
 * @param[in]     slot_bytes Per-slot capacity.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              Frame queued; FSINGLE marked.
 * @retval k_ra_err_no_data     All slots already FSINGLE (queue full).
 * @retval k_ra_err_invalid_arg Null pointer or frame_len > slot_bytes.
 * @retval k_ra_err_null_ptr    chain / tail_idx / frame is null.
 *
 * @pre Caller is in GWMC.OPC = OPERATION.
 * @pre chain initialised via init_ring + attach_buffers.
 * @post On success the chosen slot is FSINGLE with the frame copied.
 * @post On success ``*tail_idx`` advanced past the chosen slot.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_eth_gwca_tx_frame(ra_gwca_basic_descriptor_t* chain,
                              uint32_t                    ring_depth,
                              uint32_t*                   tail_idx,
                              const uint8_t*              frame,
                              uint32_t                    frame_len,
                              uint32_t                    slot_bytes)
{
  RA_CHECK_NULL_PTR(chain, s_tag, "tx_frame: chain null");
  RA_CHECK_NULL_PTR(tail_idx, s_tag, "tx_frame: tail_idx null");
  RA_CHECK_NULL_PTR(frame, s_tag, "tx_frame: frame null");
  if (frame_len == 0U || frame_len > slot_bytes) {
    return k_ra_err_invalid_arg;
  }
  uint32_t       slot = 0U;
  const ra_err_t err  = ra_eth_gwca_find_slot(chain, ring_depth, k_ra_gwdcc_dt_fempty,
                                              *tail_idx % (ring_depth - 1U), &slot);
  if (err != k_ra_ok) {
    return err;
  }
  uint8_t* const buf = internal_decode_ptr(&chain[slot]);
  if (buf == nullptr) {
    return k_ra_err_invalid_arg;
  }
  (void)memcpy(buf, frame, (size_t)frame_len);
  enum : uint32_t {
    k_ra_ds_low_byte_mask = 0xFFU,
    k_ra_ds_high_shift     = 8U,
    k_ra_ds_high_nibble    = 0xFU,
  };
  chain[slot].ds_l = (uint8_t)(frame_len & k_ra_ds_low_byte_mask);
  chain[slot].ds_h = (uint8_t)((frame_len >> k_ra_ds_high_shift) & k_ra_ds_high_nibble);
  chain[slot].dt   = (uint8_t)k_ra_gwdcc_dt_fsingle;
  *tail_idx        = (slot + 1U) % (ring_depth - 1U);
  return k_ra_ok;
}

/**
 * @brief Copy out a filled RX slot and reset it to FEMPTY.
 *
 * @details Pure helper invoked by ::ra_eth_gwca_rx_frame after the
 * caller has located an FSINGLE slot. Splits the post-validation
 * memcpy + state reset path out so the top-level function fits
 * under the 60-line cap.
 *
 * When the GWCA writes back an FSINGLE descriptor it overwrites the
 * DS field with the *received* frame length. Re-arming the slot
 * therefore has to restore DS to the buffer capacity (@p slot_bytes)
 * -- otherwise the next frame larger than the last one no longer
 * fits in "one descriptor's area" and the GWCA fragments it into
 * FSTART/FEND, which ::ra_eth_gwca_rx_frame would then never drain.
 *
 * @param[in,out] desc         The FSINGLE slot to drain.
 * @param[out]    out_frame    Destination buffer.
 * @param[in]     out_capacity Size of ``out_frame``.
 * @param[in]     slot_bytes   Buffer capacity to restore into DS.
 * @param[out]    out_len      Bytes actually written.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              Slot copied out + reset to FEMPTY.
 * @retval k_ra_err_invalid_arg buf null or frame > capacity.
 *
 * @pre desc is non-null and currently FSINGLE.
 * @pre out_frame / out_len non-null and out_capacity > 0.
 * @post Slot dt = FEMPTY and DS = slot_bytes on success.
 * @post out_len contains the frame size on success.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t internal_drain_rx_slot(ra_gwca_basic_descriptor_t* desc,
                                       uint8_t*                    out_frame,
                                       uint32_t                    out_capacity,
                                       uint32_t                    slot_bytes,
                                       uint32_t*                   out_len)
{
  const uint8_t* const buf      = internal_decode_ptr(desc);
  const uint32_t       frame_ds = internal_decode_ds(desc);
  if (buf == nullptr || frame_ds > out_capacity) {
    return k_ra_err_invalid_arg;
  }
  (void)memcpy(out_frame, buf, (size_t)frame_ds);
  *out_len = frame_ds;
  enum : uint32_t {
    k_ra_ds_byte_mask  = 0xFFU, /**< ds_l carries 8 bits.         */
    k_ra_ds_high_shift = 8U,    /**< ds_h packs the upper 4 bits. */
    k_ra_ds_high_mask  = 0xFU,  /**< ds_h field width 4 bits.     */
  };
  desc->ds_l = (uint8_t)(slot_bytes & k_ra_ds_byte_mask);
  desc->ds_h = (uint8_t)((slot_bytes >> k_ra_ds_high_shift) & k_ra_ds_high_mask);
  desc->dt   = (uint8_t)k_ra_gwdcc_dt_fempty;
  return k_ra_ok;
}

/**
 * @brief Dequeue one frame from an RX queue's descriptor ring.
 *
 * @details See header for the canonical contract. Locates the next
 * FSINGLE slot, delegates the buffer copy + slot reset to
 * ::internal_drain_rx_slot, and advances the head cursor.
 *
 * @param[in,out] chain        RX descriptor ring.
 * @param[in]     ring_depth   Ring depth.
 * @param[in,out] head_idx     Round-robin read cursor.
 * @param[out]    out_frame    Destination buffer for the frame.
 * @param[in]     out_capacity Size of out_frame.
 * @param[out]    out_len      Frame length written.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              Frame copied; slot reset to FEMPTY.
 * @retval k_ra_err_no_data     No FSINGLE slot yet.
 * @retval k_ra_err_invalid_arg out_capacity == 0 or frame > capacity.
 * @retval k_ra_err_null_ptr    chain / head_idx / out_frame / out_len null.
 *
 * @pre Caller is in GWMC.OPC = OPERATION.
 * @pre RX queue's MFWD forwarding cfg has been programmed.
 * @post On success the chosen slot is FEMPTY.
 * @post On success ``*head_idx`` advanced past the chosen slot.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
/**
 * @brief Set up the RX + TX descriptor rings for the default-state API.
 *
 * @details Helper called by ra_eth_gwca_default_open. Walks
 * init_ring + attach_buffers for both the RX and TX chains so the
 * top-level function stays under the 60-line / 40-statement budget.
 *
 * @param[in,out] state Pre-populated state block.
 *
 * @return ra_err_t Error code propagated from init_ring/attach_buffers.
 * @retval k_ra_ok              Both rings primed.
 * @retval k_ra_err_invalid_arg Depth/slot/pool inconsistent.
 * @retval k_ra_err_null_ptr    Required pointer field is null.
 *
 * @pre state->rx_chain / tx_chain are 16-byte aligned arrays of
 *      ra_gwca_basic_descriptor_t.
 * @pre state->rx_pool / tx_pool point to rx_depth * rx_slot_bytes
 *      (resp. tx_*) of payload backing.
 * @post On success every RX/TX descriptor has dt = FEMPTY and PTR
 *       pointing into the matching pool.
 * @post On success the trailing LINK descriptor of each chain wraps
 *       to slot 0.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t internal_default_open_rings(ra_eth_gwca_default_state_t* state)
{
  ra_err_t err = ra_eth_gwca_init_ring(state->rx_chain, state->rx_depth, state->rx_slot_bytes);
  RA_RETURN_ON_ERROR(err, s_tag, "default_open: rx init_ring"); /* GCOVR_EXCL_BR_LINE */
  err = ra_eth_gwca_attach_buffers(state->rx_chain, state->rx_depth, state->rx_slot_bytes,
                                   state->rx_pool);
  RA_RETURN_ON_ERROR(err, s_tag, "default_open: rx attach"); /* GCOVR_EXCL_BR_LINE */
  err = ra_eth_gwca_init_ring(state->tx_chain, state->tx_depth, state->tx_slot_bytes);
  RA_RETURN_ON_ERROR(err, s_tag, "default_open: tx init_ring"); /* GCOVR_EXCL_BR_LINE */
  return ra_eth_gwca_attach_buffers(state->tx_chain, state->tx_depth, state->tx_slot_bytes,
                                    state->tx_pool);
}

/**
 * @brief Program the RX + TX per-queue cfgs for the default-state API.
 *
 * @details Helper called by ra_eth_gwca_default_open after the rings
 * are primed and GWMC.OPC is in CONFIG. Builds the two
 * ra_eth_gwca_queue_cfg_t structs and calls configure_queue twice.
 *
 * @param[in,out] state Pre-populated state block.
 *
 * @return ra_err_t Error code propagated from configure_queue.
 * @retval k_ra_ok              Both queues programmed.
 * @retval k_ra_err_invalid_arg queue_index out of range or chain_head null.
 * @retval k_ra_err_null_ptr    state field is null.
 *
 * @pre GWMC.OPC == CONFIG.
 * @pre rx_queue_index != tx_queue_index, both < linkfix_count.
 * @post On success both GWDCC[i] cfgs are live.
 * @post On success matching LINKFIX entries point at chain_head.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t internal_default_open_queues(ra_eth_gwca_default_state_t* state)
{
  const ra_eth_gwca_queue_cfg_t rx_cfg = {.priority     = 0U,
                                          .is_tx        = false,
                                          .stop_on_last = false,
                                          .chain_head   = state->rx_chain};
  ra_err_t err = ra_eth_gwca_configure_queue(state->linkfix_table, state->rx_queue_index, &rx_cfg);
  RA_RETURN_ON_ERROR(err, s_tag, "default_open: rx config"); /* GCOVR_EXCL_BR_LINE */
  const ra_eth_gwca_queue_cfg_t tx_cfg = {.priority     = 0U,
                                          .is_tx        = true,
                                          .stop_on_last = false,
                                          .chain_head   = state->tx_chain};
  return ra_eth_gwca_configure_queue(state->linkfix_table, state->tx_queue_index, &tx_cfg);
}

/**
 * @brief Walk the bring-up sub-sequence (init/rings/bring_up/-> CONFIG).
 *
 * @details Helper for ra_eth_gwca_default_open. Splits the front
 * half of the bring-up so the top-level wrapper stays under the
 * 40-statement budget.
 *
 * @param[in,out] state Pre-populated state block.
 *
 * @return ra_err_t Error code propagated from sub-calls.
 * @retval k_ra_ok              Hardware in CONFIG mode with rings primed.
 * @retval k_ra_err_invalid_arg state field invalid.
 * @retval k_ra_err_hw_timeout  GWMC.OPC transition timed out.
 *
 * @pre state pointer non-null.
 * @pre Power gates and clocks already on (CGC + MSTP for ESWM/GWCA).
 * @post On success GWMC.OPC == CONFIG.
 * @post On success rings have descriptors with PTRs in their pools.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t internal_default_open_pre(ra_eth_gwca_default_state_t* state)
{
  g_ra_eth_gwca_pre_step = 0U;
  ra_err_t err           = ra_eth_gwca_init();
  if (err != k_ra_ok) {
    g_ra_eth_gwca_pre_step = (uint32_t)k_ra_eth_gwca_step_fail_1;
    return err;
  }
  g_ra_eth_gwca_pre_step = (uint32_t)k_ra_eth_gwca_step_ok_1;
  err                    = internal_default_open_rings(state);
  if (err != k_ra_ok) {
    g_ra_eth_gwca_pre_step = (uint32_t)k_ra_eth_gwca_step_fail_2;
    return err;
  }
  g_ra_eth_gwca_pre_step = (uint32_t)k_ra_eth_gwca_step_ok_2;
  err                    = ra_eth_gwca_bring_up(state->linkfix_table, state->linkfix_count);
  if (err != k_ra_ok) {
    g_ra_eth_gwca_pre_step = (uint32_t)k_ra_eth_gwca_step_fail_3;
    return err;
  }
  g_ra_eth_gwca_pre_step = (uint32_t)k_ra_eth_gwca_step_ok_3;
  const ra_err_t cfg_err = ra_eth_gwca_set_operation_mode(k_ra_gwmc_opc_config);
  if (cfg_err != k_ra_ok) {
    g_ra_eth_gwca_pre_step = (uint32_t)k_ra_eth_gwca_step_fail_4;
    return cfg_err;
  }
  g_ra_eth_gwca_pre_step = (uint32_t)k_ra_eth_gwca_step_ok_4;
  return k_ra_ok;
}

/**
 * @brief One-call GWCA bring-up for the default-state API.
 *
 * @details See header. Brings up RX + TX chains + LINKFIX, walks
 * the GWCA state machine to OPERATION.
 *
 * @param[in,out] state Pre-populated state block.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              GWCA live; queues walkable.
 * @retval k_ra_err_invalid_arg state pointer or fields invalid.
 * @retval k_ra_err_hw_timeout  Mode transition never converged.
 *
 * @pre state's chain / pool / table pointers are 16-byte aligned.
 * @pre rx_queue_index != tx_queue_index, both < linkfix_count.
 * @post On success GWMC.OPC = OPERATION; both queues live.
 * @post state's rx_head / tx_tail cursors reset to 0.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_eth_gwca_default_open(ra_eth_gwca_default_state_t* state)
{
  RA_CHECK_NULL_PTR(state, s_tag, "default_open: state null");
  g_ra_eth_gwca_open_step = 0U;
  ra_err_t err = internal_default_open_pre(state);
  if (err != k_ra_ok) {
    g_ra_eth_gwca_open_step = (uint32_t)k_ra_eth_gwca_step_fail_1;
    return err;
  }
  g_ra_eth_gwca_open_step = (uint32_t)k_ra_eth_gwca_step_ok_1;
  err                     = internal_default_open_queues(state);
  if (err != k_ra_ok) {
    g_ra_eth_gwca_open_step = (uint32_t)k_ra_eth_gwca_step_fail_2;
    return err;
  }
  g_ra_eth_gwca_open_step = (uint32_t)k_ra_eth_gwca_step_ok_2;
  state->rx_head          = 0U;
  state->tx_tail          = 0U;
  const ra_err_t op_err   = ra_eth_gwca_set_operation_mode(k_ra_gwmc_opc_operation);
  if (op_err != k_ra_ok) {
    g_ra_eth_gwca_open_step = (uint32_t)k_ra_eth_gwca_step_fail_3;
    return op_err;
  }
  g_ra_eth_gwca_open_step = (uint32_t)k_ra_eth_gwca_step_ok_3;

  /* Arm both queues now the GWCA is in OPERATION: BALR loads the
   * chain base into the AXI address RAM so the GWCA starts scanning
   * the descriptor chains (HUM Ch 34.3 "GWDCCi"). */
  const ra_err_t rx_reload = ra_eth_gwca_reload_queue(state->rx_queue_index);
  if (rx_reload != k_ra_ok) {
    g_ra_eth_gwca_open_step = (uint32_t)k_ra_eth_gwca_step_fail_3;
    return rx_reload;
  }
  const ra_err_t tx_reload = ra_eth_gwca_reload_queue(state->tx_queue_index);
  if (tx_reload != k_ra_ok) {
    g_ra_eth_gwca_open_step = (uint32_t)k_ra_eth_gwca_step_fail_3;
    return tx_reload;
  }
  return k_ra_ok;
}

/**
 * @brief One-call TX: enqueue + kick.
 *
 * @details See header. Wraps tx_frame + kick_tx.
 *
 * @param[in,out] state Initialized by default_open.
 * @param[in]     frame Frame bytes.
 * @param[in]     len   Frame length.
 *
 * @return ra_err_t Error code propagated from tx_frame + kick_tx.
 * @retval k_ra_ok              Frame queued + TX request fired.
 * @retval k_ra_err_no_data     Queue full.
 * @retval k_ra_err_invalid_arg len > tx_slot_bytes.
 * @retval k_ra_err_null_ptr    state or frame null.
 *
 * @pre default_open returned ok.
 * @pre state remains in its post-default_open layout.
 * @post On success state->tx_tail advanced.
 * @post On success the chip has been signaled.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_eth_gwca_default_send(ra_eth_gwca_default_state_t* state, const uint8_t* frame,
                                  uint32_t len)
{
  RA_CHECK_NULL_PTR(state, s_tag, "default_send: state null");
  const ra_err_t err = ra_eth_gwca_tx_frame(state->tx_chain, state->tx_depth, &state->tx_tail,
                                            frame, len, state->tx_slot_bytes);
  if (err != k_ra_ok) {
    return err;
  }
  return ra_eth_gwca_kick_tx(state->tx_queue_index);
}

/**
 * @brief One-call RX: dequeue next frame.
 *
 * @details See header. Wraps rx_frame using state->rx_chain/head.
 *
 * @param[in,out] state        Initialized by default_open.
 * @param[out]    out_frame    Destination buffer.
 * @param[in]     out_capacity Size of out_frame.
 * @param[out]    out_len      Frame length written.
 *
 * @return ra_err_t Error code propagated from rx_frame.
 * @retval k_ra_ok              Frame copied; slot reset to FEMPTY.
 * @retval k_ra_err_no_data     No inbound frame waiting.
 * @retval k_ra_err_invalid_arg capacity 0 or frame too large.
 * @retval k_ra_err_null_ptr    state, out_frame, or out_len null.
 *
 * @pre default_open returned ok.
 * @pre state remains in its post-default_open layout.
 * @post On success state->rx_head advanced.
 * @post On success *out_len reflects the received frame size.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_eth_gwca_default_recv(ra_eth_gwca_default_state_t* state, uint8_t* out_frame,
                                  uint32_t out_capacity, uint32_t* out_len)
{
  RA_CHECK_NULL_PTR(state, s_tag, "default_recv: state null");
  return ra_eth_gwca_rx_frame(state->rx_chain, state->rx_depth, &state->rx_head, out_frame,
                              out_capacity, state->rx_slot_bytes, out_len);
}

/**
 * @brief Dequeue one frame from an RX queue's descriptor ring.
 *
 * @details See header for the canonical contract. Locates the next
 * FSINGLE slot, delegates the buffer copy + slot reset to
 * ::internal_drain_rx_slot, and advances the head cursor.
 *
 * @param[in,out] chain        RX descriptor ring.
 * @param[in]     ring_depth   Ring depth.
 * @param[in,out] head_idx     Round-robin read cursor.
 * @param[out]    out_frame    Destination buffer for the frame.
 * @param[in]     out_capacity Size of out_frame.
 * @param[in]     slot_bytes   Per-slot buffer capacity restored into DS.
 * @param[out]    out_len      Frame length written.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              Frame copied; slot reset to FEMPTY.
 * @retval k_ra_err_no_data     No FSINGLE slot yet.
 * @retval k_ra_err_invalid_arg out_capacity == 0 or frame > capacity.
 * @retval k_ra_err_null_ptr    chain / head_idx / out_frame / out_len null.
 *
 * @pre Caller is in GWMC.OPC = OPERATION.
 * @pre RX queue's MFWD forwarding cfg has been programmed.
 * @post On success the chosen slot is FEMPTY with DS = slot_bytes.
 * @post On success ``*head_idx`` advanced past the chosen slot.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_eth_gwca_rx_frame(ra_gwca_basic_descriptor_t* chain,
                              uint32_t                    ring_depth,
                              uint32_t*                   head_idx,
                              uint8_t*                    out_frame,
                              uint32_t                    out_capacity,
                              uint32_t                    slot_bytes,
                              uint32_t*                   out_len)
{
  RA_CHECK_NULL_PTR(chain, s_tag, "rx_frame: chain null");
  RA_CHECK_NULL_PTR(head_idx, s_tag, "rx_frame: head_idx null");
  RA_CHECK_NULL_PTR(out_frame, s_tag, "rx_frame: out_frame null");
  RA_CHECK_NULL_PTR(out_len, s_tag, "rx_frame: out_len null");
  if (out_capacity == 0U) {
    return k_ra_err_invalid_arg;
  }
  uint32_t       slot = 0U;
  const ra_err_t err  = ra_eth_gwca_find_slot(chain, ring_depth, k_ra_gwdcc_dt_fsingle,
                                              *head_idx % (ring_depth - 1U), &slot);
  if (err != k_ra_ok) {
    return err;
  }
  const ra_err_t drain_err =
    internal_drain_rx_slot(&chain[slot], out_frame, out_capacity, slot_bytes, out_len);
  if (drain_err != k_ra_ok) {
    return drain_err;
  }
  *head_idx = (slot + 1U) % (ring_depth - 1U);
  return k_ra_ok;
}
