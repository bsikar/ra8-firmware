/**
 * @file ra_eth_gwca.c
 * @brief Ethernet CPU Agent driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * driver for the RA8D2 GWCA block. This translation unit holds the
 * lifecycle / status / dispatch surface plus the GWCA state-machine
 * bring-up (set_operation_mode / axi_init / install_linkfix /
 * bring_up); the per-queue descriptor primitives live in
 * ra_eth_gwca_queue.c and the one-call default-state API in
 * ra_eth_gwca_default.c. Every register access carries a HUM Ch 34
 * citation.
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
#include "ra_eth_gwca_internal.h"
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

ra_err_t ra_eth_gwca_get_status(uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 34 "Ethernet CPU Agent (GWCA)" p 1787 */
  *out_mask = ra_gwca()->GWCA_STS;
  return k_ra_ok;
}

ra_err_t ra_eth_gwca_clear_status(uint32_t mask)
{
  volatile r_gwca_regs_t* reg = ra_gwca();
  /* HUM Ch 34 "Ethernet CPU Agent (GWCA)" p 1787 */
  reg->GWCA_ICLR = mask;
  reg->GWCA_STS  = reg->GWCA_STS & ~mask;
  return k_ra_ok;
}

ra_err_t ra_eth_gwca_attach_handler(ra_eth_gwca_event_fn_t fn, void* ctx)
{
  s_gwca_fn  = fn;
  s_gwca_ctx = ctx;
  return k_ra_ok;
}

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

ra_err_t ra_eth_gwca_enter_stop(void)
{
  /* HUM Ch 34 "Ethernet CPU Agent (GWCA)" p 1787 */
  ra_gwca()->GWCA_CTRL = 0U;
  return ra_mstp_disable(k_ra_mstp_eswm);
}

ra_err_t ra_eth_gwca_exit_stop(void)
{
  return ra_mstp_enable(k_ra_mstp_eswm);
}

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
  volatile uint32_t* const gwmc =
    (volatile uint32_t*)(k_ra_gwca0_base_addr + (uintptr_t)k_ra_gwca_off_gwmc);

  const uint32_t opc = (uint32_t)mode & (uint32_t)k_ra_gwmc_opc_mask;
  const uint32_t cur = *gwmc & ~(uint32_t)k_ra_gwmc_opc_mask;
  *gwmc              = cur | opc;

#ifndef RA_SIMULATOR_MODE
  /* HUM Ch 34.3.2 "GWMS : Mode Status Register" p 1798 */
  volatile uint32_t* const gwms =
    (volatile uint32_t*)(k_ra_gwca0_base_addr + (uintptr_t)k_ra_gwca_off_gwms);
  for (uint32_t i = 0U; i < k_ra_eth_gwca_mode_spin; ++i) { /* GCOVR_EXCL_BR_LINE */
    if ((*gwms & (uint32_t)k_ra_gwmc_opc_mask) == opc) {    /* GCOVR_EXCL_BR_LINE */
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
  const uintptr_t addr = (uintptr_t)linkfix_table;
  enum : uintptr_t {
    k_ra_linkfix_upper_shift = 32U,
    k_ra_linkfix_upper_mask  = 0xFFULL,
    k_ra_linkfix_lower_mask  = 0xFFFFFFFFULL,
  };
  const uint32_t upper =
    (uint32_t)((uint64_t)addr >> k_ra_linkfix_upper_shift) & k_ra_linkfix_upper_mask;
  const uint32_t           lower = (uint32_t)addr & k_ra_linkfix_lower_mask;
  volatile uint32_t* const gwdcbac0 =
    (volatile uint32_t*)(k_ra_gwca0_base_addr + (uintptr_t)k_ra_gwca_off_gwdcbac0);
  volatile uint32_t* const gwdcbac1 =
    (volatile uint32_t*)(k_ra_gwca0_base_addr + (uintptr_t)k_ra_gwca_off_gwdcbac1);
  *gwdcbac0 = upper;
  *gwdcbac1 = lower;
  return k_ra_ok;
}

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
