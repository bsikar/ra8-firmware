/**
 * @file ra_etha.c
 * @brief Per-port Ethernet Agent (ETHA) driver implementation -- HUM Ch 32
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Round-3 driver for the RA8D2 ETHA block (HUM Ch 32, p 1627-1702).
 * ETHA is the per-port "Ethernet Agent" that bridges the L3 switch
 * fabric (ESWM, ra_eth.c, HUM Ch 29) and the per-port MAC (RMAC,
 * ra_rmac.c, HUM Ch 33).
 *
 * Pipeline: ESWM (top, switch) -> ETHA (per-port agent, this file)
 *           -> RMAC (per-port MAC) -> off-chip PHY (bottom).
 *
 * The shared MSTP gate ``k_ra_mstp_eswm`` is reference-counted by
 * ra_mstp, so init/deinit interleave safely with the other ethernet
 * sub-drivers.
 *
 * Coverage in this round:
 *  - All three error-IRQ blocks (EAEIS0/E/D0, EAEIS1/E/D1, EAEIS2/E/D2)
 *    plumbed through one central dispatch path;
 *  - Per-class TX queues (8 traffic classes, EATPEC/EATDQAC/EATDQDC/EATDQM);
 *  - 802.3br TX preemption (EATPEC.TTQ + AFS);
 *  - IPV remap table (EAIRC, 8 entries x 3 bits);
 *  - 802.1Q VLAN insertion (EAVCC.VIM + EAVTC C-/S-tags) and stripping
 *    (EAVCC.VEM modes);
 *  - RX tag filter (EARTFC) acting as the multicast-group filter;
 *  - Cut-through queue (EACTQC + EACTDQDC);
 *  - Credit-based shaper (CBS, EACAEC, EACC, EACAIVC[8], EACAULC[8],
 *    EACOEM, EACGSM mirrors);
 *  - Time-aware shaper (TAS / 802.1Qbv) gate-control list with cycle
 *    time + start time (EATASC, EATASIGSC, EATASENC[8], EATASCSTC0/1,
 *    EATASCTC, EATASGL0/1);
 *  - Per-port full statistics (EAUSMFSECN, EATFECN, EAFSECN, EADQOECN,
 *    EADQSECN) with read + clear paths;
 *  - Security gate (EASCR);
 *  - Software reset / lifecycle.
 *
 * Every register access carries a HUM Ch 32 citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_etha.h"

#include <stdint.h>

#include "ra8d2_etha_regs.h"
#include "ra8d2_ether_regs.h"
#include "ra8d2_mstp_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

/**
 * @var s_tag
 * @brief Logger tag used by every ra_etha_* call.
 */
static const char* s_tag = "ETHA";

/**
 * @struct ra_etha_slot_t
 * @brief Per-port runtime state.
 */
typedef struct {
  ra_etha_event_fn_t cb;  /**< Attached callback (nullptr if none). */
  void*              ctx; /**< Opaque cookie passed back to cb.     */
} ra_etha_slot_t;

/**
 * @var s_slots
 * @brief Per-port handler table; index = ::ra_etha_port_t.
 */
static ra_etha_slot_t s_slots[k_ra_etha_port_count];

/**
 * @brief Range-check a port argument.
 *
 * @param[in] port Port to validate.
 * @return true if port is one of ::ra_etha_port_t.
 */
static inline bool internal_port_ok(ra_etha_port_t port)
{
  return (uint8_t)port < (uint8_t)k_ra_etha_port_count;
}

/**
 * @brief Range-check a traffic-class argument.
 *
 * @param[in] tc Traffic class to validate.
 * @return true if tc is one of ::ra_etha_tc_t.
 */
static inline bool internal_tc_ok(ra_etha_tc_t tc)
{
  return (uint8_t)tc < (uint8_t)k_ra_etha_tc_count;
}

/**
 * @brief Range-check an error-IRQ block index.
 *
 * @param[in] block Block index to validate.
 * @return true if in 0..2.
 */
static inline bool internal_irq_block_ok(ra_etha_irq_class_t block)
{
  return (uint8_t)block < (uint8_t)k_ra_etha_irq_class_count;
}

ra_err_t ra_etha_init(ra_etha_port_t port, const ra_etha_config_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "etha_init: cfg must not be nullptr");
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "etha_init: port out of range");
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_eswm);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "etha_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  volatile r_etha_regs_t* reg = ra_etha(port);

  /* HUM Ch 32.3.1.1 "EAMC : Mode Command Register" p 1631 */
  reg->EAMC = (uint32_t)cfg->initial_mode & (uint32_t)k_ra_etha_mask_opc;
  /* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */
  reg->EAEID0 = 0xFFFFFFFFUL;
  /* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */
  reg->EAEID1 = 0xFFFFFFFFUL;
  /* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */
  reg->EAEID2 = 0xFFFFFFFFUL;
  /* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */
  reg->EAEIE0 = cfg->eaeie0_mask;
  /* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */
  reg->EAEIE1 = cfg->eaeie1_mask;
  /* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */
  reg->EAEIE2 = cfg->eaeie2_mask;

  s_slots[(uint8_t)port].cb  = nullptr;
  s_slots[(uint8_t)port].ctx = nullptr;
  ra_log_info(s_tag, "etha_init");
  return k_ra_ok;
}

ra_err_t ra_etha_deinit(ra_etha_port_t port)
{
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "etha_deinit: port out of range");
    return k_ra_err_invalid_arg;
  }

  volatile r_etha_regs_t* reg = ra_etha(port);
  /* HUM Ch 32.3.1.1 "EAMC : Mode Command Register" p 1631 */
  reg->EAMC = (uint32_t)k_ra_etha_opc_reset;
  /* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */
  reg->EAEIE0 = 0U;
  /* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */
  reg->EAEIE1 = 0U;
  /* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */
  reg->EAEIE2 = 0U;

  s_slots[(uint8_t)port].cb  = nullptr;
  s_slots[(uint8_t)port].ctx = nullptr;
  /* MSTP gate is shared with the rest of the Ethernet subsystem; do
   * NOT drop it here. ra_eth_deinit owns the final reference. */
  return k_ra_ok;
}

ra_err_t ra_etha_get_status(ra_etha_port_t port, ra_etha_status_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "etha_get_status: out must not be nullptr");
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "etha_get_status: port out of range");
    return k_ra_err_invalid_arg;
  }

  volatile r_etha_regs_t* reg = ra_etha(port);
  /* HUM Ch 32.3.1.2 "EAMS : Mode Status Register" p 1631 */
  const uint32_t ops = reg->EAMS & (uint32_t)k_ra_etha_mask_ops;
  /* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */
  const uint32_t eaeis0 = reg->EAEIS0;
  /* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */
  const uint32_t eaeis1 = reg->EAEIS1;
  /* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */
  const uint32_t eaeis2 = reg->EAEIS2;
  /* HUM Ch 32.3 "EATASCTM : TAS Cycle Time Monitoring Register" p 1668 */
  const uint32_t tasctm = reg->EATASCTM;
  out->ops              = (ra_etha_ops_t)(uint8_t)ops;
  out->eaeis0           = eaeis0;
  out->eaeis1           = eaeis1;
  out->eaeis2           = eaeis2;
  out->tas_cycle        = tasctm;
  return k_ra_ok;
}

ra_err_t ra_etha_clear_status(ra_etha_port_t port, ra_etha_irq_class_t block, uint32_t mask)
{
  if (!internal_port_ok(port) || !internal_irq_block_ok(block)) {
    ra_log_error(s_tag, "etha_clear_status: port/block out of range");
    return k_ra_err_invalid_arg;
  }

  volatile r_etha_regs_t* reg = ra_etha(port);
  switch (block) {
    case k_ra_etha_irq_class_0:
      /* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */
      reg->EAEID0 = mask;
      /* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */
      reg->EAEIS0 = reg->EAEIS0 & ~mask;
      break;
    case k_ra_etha_irq_class_1:
      /* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */
      reg->EAEID1 = mask;
      /* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */
      reg->EAEIS1 = reg->EAEIS1 & ~mask;
      break;
    case k_ra_etha_irq_class_2:
    default:
      /* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */
      reg->EAEID2 = mask;
      /* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */
      reg->EAEIS2 = reg->EAEIS2 & ~mask;
      break;
  }
  return k_ra_ok;
}

ra_err_t ra_etha_enable_irq(ra_etha_port_t port, ra_etha_irq_class_t block, uint32_t mask)
{
  if (!internal_port_ok(port) || !internal_irq_block_ok(block)) {
    ra_log_error(s_tag, "etha_enable_irq: bad arg");
    return k_ra_err_invalid_arg;
  }
  volatile r_etha_regs_t* reg = ra_etha(port);
  switch (block) {
    case k_ra_etha_irq_class_0:
      /* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */
      reg->EAEIE0 = reg->EAEIE0 | mask;
      break;
    case k_ra_etha_irq_class_1:
      /* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */
      reg->EAEIE1 = reg->EAEIE1 | mask;
      break;
    case k_ra_etha_irq_class_2:
    default:
      /* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */
      reg->EAEIE2 = reg->EAEIE2 | mask;
      break;
  }
  return k_ra_ok;
}

ra_err_t ra_etha_disable_irq(ra_etha_port_t port, ra_etha_irq_class_t block, uint32_t mask)
{
  if (!internal_port_ok(port) || !internal_irq_block_ok(block)) {
    ra_log_error(s_tag, "etha_disable_irq: bad arg");
    return k_ra_err_invalid_arg;
  }
  volatile r_etha_regs_t* reg = ra_etha(port);
  switch (block) {
    case k_ra_etha_irq_class_0:
      /* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */
      reg->EAEIE0 = reg->EAEIE0 & ~mask;
      break;
    case k_ra_etha_irq_class_1:
      /* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */
      reg->EAEIE1 = reg->EAEIE1 & ~mask;
      break;
    case k_ra_etha_irq_class_2:
    default:
      /* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */
      reg->EAEIE2 = reg->EAEIE2 & ~mask;
      break;
  }
  return k_ra_ok;
}

ra_err_t ra_etha_attach_handler(ra_etha_port_t port, ra_etha_event_fn_t cb, void* ctx)
{
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "etha_attach_handler: port out of range");
    return k_ra_err_invalid_arg;
  }
  s_slots[(uint8_t)port].cb  = cb;
  s_slots[(uint8_t)port].ctx = ctx;
  return k_ra_ok;
}

void ra_etha_dispatch(ra_etha_port_t port)
{
  if (!internal_port_ok(port)) {
    return;
  }

  volatile r_etha_regs_t* reg = ra_etha(port);
  /* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */
  const uint32_t s0 = reg->EAEIS0;
  /* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */
  const uint32_t s1 = reg->EAEIS1;
  /* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */
  const uint32_t s2 = reg->EAEIS2;

  const ra_etha_event_fn_t fn  = s_slots[(uint8_t)port].cb;
  void* const              ctx = s_slots[(uint8_t)port].ctx;

  /* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */
  reg->EAEID0 = s0;
  /* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */
  reg->EAEID1 = s1;
  /* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */
  reg->EAEID2 = s2;
  /* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */
  reg->EAEIS0 = 0U;
  /* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */
  reg->EAEIS1 = 0U;
  /* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */
  reg->EAEIS2 = 0U;

  if (fn != nullptr) {
    fn(ctx, port, s0, s1, s2);
  }
}

ra_err_t ra_etha_enter_stop(ra_etha_port_t port)
{
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "etha_enter_stop: port out of range");
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 32.3.1.1 "EAMC : Mode Command Register" p 1631 */
  ra_etha(port)->EAMC = (uint32_t)k_ra_etha_opc_disable;
  return k_ra_ok;
}

ra_err_t ra_etha_exit_stop(ra_etha_port_t port)
{
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "etha_exit_stop: port out of range");
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 32.3.1.1 "EAMC : Mode Command Register" p 1631 */
  ra_etha(port)->EAMC = (uint32_t)k_ra_etha_opc_operation;
  return k_ra_ok;
}

ra_err_t ra_etha_reset(ra_etha_port_t port)
{
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "etha_reset: port out of range");
    return k_ra_err_invalid_arg;
  }
  volatile r_etha_regs_t* reg = ra_etha(port);
  /* HUM Ch 32.3.1.1 "EAMC : Mode Command Register" p 1631 */
  reg->EAMC = (uint32_t)k_ra_etha_opc_reset;
  /* HUM Ch 32.3.1.1 "EAMC : Mode Command Register" p 1631 */
  reg->EAMC = (uint32_t)k_ra_etha_opc_config;
  return k_ra_ok;
}

ra_err_t ra_etha_set_mode(ra_etha_port_t port, ra_etha_opc_t mode)
{
  if (!internal_port_ok(port) || (uint32_t)mode > (uint32_t)k_ra_etha_mask_opc) {
    ra_log_error(s_tag, "etha_set_mode: bad arg");
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 32.3.1.1 "EAMC : Mode Command Register" p 1631 */
  ra_etha(port)->EAMC = (uint32_t)mode & (uint32_t)k_ra_etha_mask_opc;
  return k_ra_ok;
}

ra_err_t ra_etha_set_queue_arb(ra_etha_port_t port, ra_etha_tc_t tc, uint8_t arb)
{
  if (!internal_port_ok(port) || !internal_tc_ok(tc) ||
      (uint32_t)arb > (uint32_t)k_ra_etha_mask_tdqa) {
    ra_log_error(s_tag, "etha_set_queue_arb: bad arg");
    return k_ra_err_invalid_arg;
  }

  volatile r_etha_regs_t* reg = ra_etha(port);
  /* HUM Ch 32.3 "EATDQAC : TX Descriptor Queue Arbitration Cfg" p 1633 */
  const uint32_t shift = (uint32_t)((uint8_t)tc * 4U);
  /* HUM Ch 32.3 "EATDQAC : TX Descriptor Queue Arbitration Cfg" p 1633 */
  const uint32_t mask = (uint32_t)k_ra_etha_mask_tdqa << shift;
  /* HUM Ch 32.3 "EATDQAC : TX Descriptor Queue Arbitration Cfg" p 1633 */
  reg->EATDQAC =
    (reg->EATDQAC & ~mask) | (((uint32_t)arb & (uint32_t)k_ra_etha_mask_tdqa) << shift);
  return k_ra_ok;
}

ra_err_t ra_etha_set_queue_depth(ra_etha_port_t port, ra_etha_tc_t tc, uint16_t depth)
{
  if (!internal_port_ok(port) || !internal_tc_ok(tc) ||
      (uint32_t)depth > (uint32_t)k_ra_etha_mask_dqd) {
    ra_log_error(s_tag, "etha_set_queue_depth: bad arg");
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 32.3 "EATDQDCq : Per-class TX Queue Depth Cfg" p 1641 */
  ra_etha(port)->EATDQDC[(uint8_t)tc] = (uint32_t)depth & (uint32_t)k_ra_etha_mask_dqd;
  return k_ra_ok;
}

ra_err_t
ra_etha_get_queue_level(ra_etha_port_t port, ra_etha_tc_t tc, uint16_t* cur_level, uint16_t* peak)
{
  RA_CHECK_NULL_PTR(cur_level, s_tag, "etha_get_queue_level: cur_level null");
  RA_CHECK_NULL_PTR(peak, s_tag, "etha_get_queue_level: peak null");
  if (!internal_port_ok(port) || !internal_tc_ok(tc)) {
    ra_log_error(s_tag, "etha_get_queue_level: bad arg");
    return k_ra_err_invalid_arg;
  }

  volatile r_etha_regs_t* reg = ra_etha(port);
  /* HUM Ch 32.3 "EATDQMq : Per-class TX Queue Monitor" p 1647 */
  *cur_level = (uint16_t)(reg->EATDQM[(uint8_t)tc] & (uint32_t)k_ra_etha_mask_dnq);
  /* HUM Ch 32.3 "EATDQMLMq : Per-class TX Queue Max Level Monitor" p 1650 */
  *peak = (uint16_t)(reg->EATDQMLM[(uint8_t)tc] & (uint32_t)k_ra_etha_mask_dnq);
  return k_ra_ok;
}

ra_err_t
ra_etha_set_preemption(ra_etha_port_t port, uint8_t preempt, uint8_t cut_thru, ra_etha_afs_t afs)
{
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "etha_set_preemption: port out of range");
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 32.3 "EATPEC : TX Preemption Configuration" p 1635 */
  uint32_t v = (uint32_t)preempt & 0xFFU;
  if (cut_thru != 0U) {
    v |= (1UL << 8); /* TTQ8 cut-through preemptable */
  }
  v |= ((uint32_t)afs & 0x3UL) << (uint32_t)k_ra_etha_eatpec_afs_pos;
  /* HUM Ch 32.3 "EATPEC : TX Preemption Configuration" p 1635 */
  ra_etha(port)->EATPEC = v;
  return k_ra_ok;
}

ra_err_t ra_etha_set_max_frame_size(ra_etha_port_t port, ra_etha_tc_t tc, uint16_t max_bytes)
{
  if (!internal_port_ok(port) || !internal_tc_ok(tc)) {
    ra_log_error(s_tag, "etha_set_max_frame_size: bad arg");
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 32.3 "EATMFSCq : Per-class TX Max Frame Size Cfg" p 1639 */
  ra_etha(port)->EATMFSC[(uint8_t)tc] = (uint32_t)max_bytes & (uint32_t)k_ra_etha_mask_mfs;
  return k_ra_ok;
}

ra_err_t ra_etha_set_ipv_remap(ra_etha_port_t port, const uint8_t* map)
{
  RA_CHECK_NULL_PTR(map, s_tag, "etha_set_ipv_remap: map null");
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "etha_set_ipv_remap: port out of range");
    return k_ra_err_invalid_arg;
  }
  /* Validate every entry first so we never write a partial value. */
  for (uint8_t i = 0U; i < (uint8_t)k_ra_etha_tc_count; ++i) {
    if ((uint32_t)map[i] > (uint32_t)k_ra_etha_mask_ipv) {
      ra_log_error(s_tag, "etha_set_ipv_remap: entry > 7");
      return k_ra_err_invalid_arg;
    }
  }
  /* HUM Ch 32.3 "EAIRC : IPV Remap Configuration" p 1632 */
  uint32_t packed = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_ra_etha_tc_count; ++i) {
    /* HUM Ch 32.3 "EAIRC : IPV Remap Configuration" p 1632 */
    const uint32_t shift = (uint32_t)i * 4U;
    packed |= ((uint32_t)map[i] & (uint32_t)k_ra_etha_mask_ipv) << shift;
  }
  /* HUM Ch 32.3 "EAIRC : IPV Remap Configuration" p 1632 */
  ra_etha(port)->EAIRC = packed;
  return k_ra_ok;
}

ra_err_t ra_etha_set_vlan_mode(ra_etha_port_t port, ra_etha_vim_t vim, ra_etha_vem_t vem)
{
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "etha_set_vlan_mode: port out of range");
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 32.3 "EAVCC : VLAN Control Configuration" p 1654 */
  const uint32_t v =
    ((uint32_t)vim & 0x1U) | (((uint32_t)vem & 0x7U) << (uint32_t)k_ra_etha_eavcc_vem_pos);
  /* HUM Ch 32.3 "EAVCC : VLAN Control Configuration" p 1654 */
  ra_etha(port)->EAVCC = v;
  return k_ra_ok;
}

ra_err_t ra_etha_set_vlan_tag(ra_etha_port_t            port,
                              const ra_etha_vlan_tag_t* c_tag,
                              const ra_etha_vlan_tag_t* s_tag_in)
{
  RA_CHECK_NULL_PTR(c_tag, s_tag, "etha_set_vlan_tag: c_tag null");
  RA_CHECK_NULL_PTR(s_tag_in, s_tag, "etha_set_vlan_tag: s_tag null");
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "etha_set_vlan_tag: port out of range");
    return k_ra_err_invalid_arg;
  }
  if ((uint32_t)c_tag->vid > (uint32_t)k_ra_etha_mask_vlan_vid ||
      (uint32_t)s_tag_in->vid > (uint32_t)k_ra_etha_mask_vlan_vid ||
      (uint32_t)c_tag->pcp > (uint32_t)k_ra_etha_mask_vlan_pcp ||
      (uint32_t)s_tag_in->pcp > (uint32_t)k_ra_etha_mask_vlan_pcp ||
      (uint32_t)c_tag->dei > (uint32_t)k_ra_etha_mask_vlan_dei ||
      (uint32_t)s_tag_in->dei > (uint32_t)k_ra_etha_mask_vlan_dei) {
    ra_log_error(s_tag, "etha_set_vlan_tag: tag fields out of range");
    return k_ra_err_invalid_arg;
  }

  const uint32_t v = (((uint32_t)c_tag->vid & (uint32_t)k_ra_etha_mask_vlan_vid)
                      << (uint32_t)k_ra_etha_eavtc_ctv_pos) |
                     (((uint32_t)c_tag->pcp & (uint32_t)k_ra_etha_mask_vlan_pcp)
                      << (uint32_t)k_ra_etha_eavtc_ctp_pos) |
                     (((uint32_t)c_tag->dei & (uint32_t)k_ra_etha_mask_vlan_dei)
                      << (uint32_t)k_ra_etha_eavtc_ctd_pos) |
                     (((uint32_t)s_tag_in->vid & (uint32_t)k_ra_etha_mask_vlan_vid)
                      << (uint32_t)k_ra_etha_eavtc_stv_pos) |
                     (((uint32_t)s_tag_in->pcp & (uint32_t)k_ra_etha_mask_vlan_pcp)
                      << (uint32_t)k_ra_etha_eavtc_stp_pos) |
                     (((uint32_t)s_tag_in->dei & (uint32_t)k_ra_etha_mask_vlan_dei)
                      << (uint32_t)k_ra_etha_eavtc_std_pos);
  /* HUM Ch 32.3 "EAVTC : VLAN TAG Configuration" p 1655 */
  ra_etha(port)->EAVTC = v;
  return k_ra_ok;
}

ra_err_t ra_etha_set_rx_tag_filter(ra_etha_port_t port, uint32_t mask)
{
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "etha_set_rx_tag_filter: port out of range");
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 32.3 "EARTFC : RX TAG Filtering Configuration" p 1656 */
  ra_etha(port)->EARTFC = mask & 0x000001FFUL;
  return k_ra_ok;
}

ra_err_t ra_etha_configure_cut_through(ra_etha_port_t port, uint16_t qd, uint8_t dqd)
{
  if (!internal_port_ok(port) || (uint32_t)dqd > (uint32_t)k_ra_etha_mask_ctdqd) {
    ra_log_error(s_tag, "etha_configure_cut_through: bad arg");
    return k_ra_err_invalid_arg;
  }
  volatile r_etha_regs_t* reg = ra_etha(port);
  /* HUM Ch 32.3 "EACTQC : Cut-Through Queue Configuration" p 1651 */
  reg->EACTQC = (uint32_t)qd & (uint32_t)k_ra_etha_mask_ctqd;
  /* HUM Ch 32.3 "EACTDQDC : Cut-Through Descriptor Queue Depth" p 1652 */
  reg->EACTDQDC = (uint32_t)dqd & (uint32_t)k_ra_etha_mask_ctdqd;
  return k_ra_ok;
}

ra_err_t ra_etha_configure_cbs(ra_etha_port_t             port,
                               ra_etha_tc_t               tc,
                               uint8_t                    enable,
                               const ra_etha_cbs_param_t* param)
{
  if (!internal_port_ok(port) || !internal_tc_ok(tc)) {
    ra_log_error(s_tag, "etha_configure_cbs: bad arg");
    return k_ra_err_invalid_arg;
  }
  if (enable != 0U) {
    RA_CHECK_NULL_PTR(param, s_tag, "etha_configure_cbs: param null");
    if (param->increment > (uint32_t)k_ra_etha_mask_civ ||
        param->upper_lim > (uint32_t)k_ra_etha_mask_cul) {
      ra_log_error(s_tag, "etha_configure_cbs: param range");
      return k_ra_err_invalid_arg;
    }
  }

  volatile r_etha_regs_t* reg = ra_etha(port);
  if (enable != 0U) {
    /* HUM Ch 32.3 "EACAIVCq : CBS Admin Increment Value" p 1660 */
    reg->EACAIVC[(uint8_t)tc] = param->increment & (uint32_t)k_ra_etha_mask_civ;
    /* HUM Ch 32.3 "EACAULCq : CBS Admin Upper Limit" p 1661 */
    reg->EACAULC[(uint8_t)tc] = param->upper_lim & (uint32_t)k_ra_etha_mask_cul;
  }
  /* HUM Ch 32.3 "EACAEC : CBS Admin Enable Configuration" p 1658 */
  const uint32_t bit = 1UL << (uint8_t)tc;
  if (enable != 0U) {
    reg->EACAEC = reg->EACAEC | bit;
    /* HUM Ch 32.3 "EACC : CBS Configuration" p 1659 */
    reg->EACC = reg->EACC | bit;
  } else {
    reg->EACAEC = reg->EACAEC & ~bit;
    /* HUM Ch 32.3 "EACC : CBS Configuration" p 1659 */
    reg->EACC = reg->EACC & ~bit;
  }
  return k_ra_ok;
}

ra_err_t ra_etha_get_cbs_state(ra_etha_port_t       port,
                               ra_etha_tc_t         tc,
                               uint8_t*             enabled,
                               uint8_t*             gate_open,
                               ra_etha_cbs_param_t* oper_param)
{
  RA_CHECK_NULL_PTR(enabled, s_tag, "etha_get_cbs_state: enabled null");
  RA_CHECK_NULL_PTR(gate_open, s_tag, "etha_get_cbs_state: gate_open null");
  RA_CHECK_NULL_PTR(oper_param, s_tag, "etha_get_cbs_state: oper_param null");
  if (!internal_port_ok(port) || !internal_tc_ok(tc)) {
    ra_log_error(s_tag, "etha_get_cbs_state: bad arg");
    return k_ra_err_invalid_arg;
  }

  volatile r_etha_regs_t* reg = ra_etha(port);
  const uint32_t          bit = 1UL << (uint8_t)tc;
  /* HUM Ch 32.3 "EACOEM : CBS Oper Enable Monitoring" p 1663 */
  *enabled = (uint8_t)((reg->EACOEM & bit) != 0U ? 1U : 0U);
  /* HUM Ch 32.3 "EACGSM : CBS Gate State Monitoring" p 1666 */
  *gate_open = (uint8_t)((reg->EACGSM & bit) != 0U ? 1U : 0U);
  /* HUM Ch 32.3 "EACOIVMq : CBS Oper Increment Monitor" p 1664 */
  oper_param->increment = reg->EACOIVM[(uint8_t)tc] & (uint32_t)k_ra_etha_mask_civ;
  /* HUM Ch 32.3 "EACOULMq : CBS Oper Upper Limit Monitor" p 1665 */
  oper_param->upper_lim = reg->EACOULM[(uint8_t)tc] & (uint32_t)k_ra_etha_mask_cul;
  return k_ra_ok;
}

ra_err_t ra_etha_set_tas_schedule(ra_etha_port_t            port,
                                  const ra_etha_tas_gate_t* gate_list,
                                  uint16_t                  entry_count,
                                  uint32_t                  cycle_units,
                                  uint64_t                  start_time)
{
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "etha_set_tas_schedule: port out of range");
    return k_ra_err_invalid_arg;
  }
  if (entry_count > 256U) {
    ra_log_error(s_tag, "etha_set_tas_schedule: entry_count > 256");
    return k_ra_err_invalid_arg;
  }
  if (entry_count > 0U) {
    RA_CHECK_NULL_PTR(gate_list, s_tag, "etha_set_tas_schedule: gate_list null");
  }

  volatile r_etha_regs_t* reg = ra_etha(port);
  /* HUM Ch 32.3 "EATASCSTC0 : TAS Cycle Start Time Cfg lo" p 1672 */
  reg->EATASCSTC0 = (uint32_t)(start_time & 0xFFFFFFFFUL);
  /* HUM Ch 32.3 "EATASCSTC1 : TAS Cycle Start Time Cfg hi" p 1673 */
  reg->EATASCSTC1 = (uint32_t)((start_time >> 32U) & 0xFFFFFFFFUL);
  /* HUM Ch 32.3 "EATASCTC : TAS Cycle Time Configuration" p 1675 */
  reg->EATASCTC = cycle_units;

  /* HUM Ch 32.3 "EATASIGSC : TAS Initial Gate State Cfg" p 1668 */
  if (entry_count > 0U) {
    reg->EATASIGSC = (uint32_t)gate_list[0].gate_state;
  }

  /* HUM Ch 32.3 "EATASGL0 : TAS Gate Learn 0" p 1677 */
  /* HUM Ch 32.3 "EATASGL1 : TAS Gate Learn 1" p 1678 */
  for (uint16_t i = 0U; i < entry_count; ++i) {
    /* HUM Ch 32.3 "EATASGL0 : TAS Gate Learn 0" p 1677 */
    reg->EATASGL0 = (uint32_t)gate_list[i].gate_state & 0xFFU;
    /* HUM Ch 32.3 "EATASGL1 : TAS Gate Learn 1" p 1678 */
    const uint32_t gl1 = (gate_list[i].time_units & (uint32_t)k_ra_etha_mask_tas_gtl) |
                         (gate_list[i].cut_through != 0U ? (1UL << 28) : 0U);
    /* HUM Ch 32.3 "EATASGL1 : TAS Gate Learn 1" p 1678 */
    reg->EATASGL1 = gl1;
  }

  /* Commit the new schedule. */
  /* HUM Ch 32.3 "EATASC : TAS Configuration" p 1667 */
  reg->EATASC = reg->EATASC | (1UL << (uint32_t)k_ra_etha_eatasc_tascc_pos);
  return k_ra_ok;
}

ra_err_t ra_etha_enable_tas(ra_etha_port_t port, uint8_t enable)
{
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "etha_enable_tas: port out of range");
    return k_ra_err_invalid_arg;
  }
  volatile r_etha_regs_t* reg = ra_etha(port);
  /* HUM Ch 32.3 "EATASC : TAS Configuration" p 1667 */
  if (enable != 0U) {
    reg->EATASC = reg->EATASC | (1UL << (uint32_t)k_ra_etha_eatasc_tase_pos);
  } else {
    reg->EATASC = reg->EATASC & ~(1UL << (uint32_t)k_ra_etha_eatasc_tase_pos);
  }
  return k_ra_ok;
}

ra_err_t ra_etha_read_stats(ra_etha_port_t port, ra_etha_stats_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "etha_read_stats: out null");
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "etha_read_stats: port out of range");
    return k_ra_err_invalid_arg;
  }

  volatile r_etha_regs_t* reg = ra_etha(port);
  /* HUM Ch 32.3 "EAUSMFSECN : Switch Min Frame Size Err Counter" p 1680 */
  out->switch_min_frame_err = (uint16_t)(reg->EAUSMFSECN & (uint32_t)k_ra_etha_mask_mfs);
  /* HUM Ch 32.3 "EATFECN : TAG Filtering Error Counter" p 1681 */
  out->tag_filter_err = (uint16_t)(reg->EATFECN & (uint32_t)k_ra_etha_mask_mfs);
  /* HUM Ch 32.3 "EAFSECN : Frame Size Error Counter" p 1682 */
  out->frame_size_err = (uint16_t)(reg->EAFSECN & (uint32_t)k_ra_etha_mask_mfs);
  /* HUM Ch 32.3 "EADQOECN : Descriptor Queue Overflow Error Counter" p 1683 */
  out->queue_overflow_err = (uint16_t)(reg->EADQOECN & (uint32_t)k_ra_etha_mask_mfs);
  /* HUM Ch 32.3 "EADQSECN : Descriptor Queue Security Error Counter" p 1684 */
  out->queue_security_err = (uint16_t)(reg->EADQSECN & (uint32_t)k_ra_etha_mask_mfs);
  return k_ra_ok;
}

ra_err_t ra_etha_clear_stats(ra_etha_port_t port)
{
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "etha_clear_stats: port out of range");
    return k_ra_err_invalid_arg;
  }
  volatile r_etha_regs_t* reg = ra_etha(port);
  /* HUM Ch 32.3 "EAUSMFSECN : Switch Min Frame Size Err Counter" p 1680 */
  reg->EAUSMFSECN = 0U;
  /* HUM Ch 32.3 "EATFECN : TAG Filtering Error Counter" p 1681 */
  reg->EATFECN = 0U;
  /* HUM Ch 32.3 "EAFSECN : Frame Size Error Counter" p 1682 */
  reg->EAFSECN = 0U;
  /* HUM Ch 32.3 "EADQOECN : Descriptor Queue Overflow Error Counter" p 1683 */
  reg->EADQOECN = 0U;
  /* HUM Ch 32.3 "EADQSECN : Descriptor Queue Security Error Counter" p 1684 */
  reg->EADQSECN = 0U;
  return k_ra_ok;
}

ra_err_t ra_etha_set_security(ra_etha_port_t port, uint32_t mask)
{
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "etha_set_security: port out of range");
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 32.3 "EASCR : Security Configuration" p 1697 */
  ra_etha(port)->EASCR = mask;
  return k_ra_ok;
}
