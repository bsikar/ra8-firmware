/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_etha.h
 * @brief Per-port Ethernet Agent (ETHA) driver -- HUM Ch 32 (p 1627-1702)
 * @ingroup grp_hal_net
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * driver for the RA8D2 Ethernet Agent (ETHA) block. The
 * Ethernet pipeline on the RA8D2 is a small stack:
 *
 * @verbatim
 * +-----------------------+ top +-------------------------+
 * | ESWM (Ch 29) | ------> | Switch fabric |
 * | ra8_eth.c | | |
 * +-----------------------+ +-------------------------+
 * | per-port descriptor / forwarding bus
 * v
 * +-----------------------+ ETHA +-------------------------+
 * | ETHA (Ch 32, here) | <-----> | per-port queue / FIFO |
 * | ra8_etha.c (one per m) | | preemption, IPV remap |
 * +-----------------------+ +-------------------------+
 * | per-port MAC bus
 * v
 * +-----------------------+ RMAC +-------------------------+
 * | RMAC (Ch 33) | <-----> | MAC + PHY MDIO |
 * | ra8_rmac.c | | |
 * +-----------------------+ +-------------------------+
 * | MII / GMII pins
 * v bottom
 * Off-chip PHY (LAN8740, KSZ9031,...)
 * @endverbatim
 *
 * Round-3 brings the driver to **full HUM Ch 32 coverage**:
 *
 * - 8 traffic-class TX queues (EATDQAC, EATDQDC, EATDQM, EATDQMLM)
 * - TX preemption (802.3br) via EATPEC
 * - Per-class max frame size programming (EATMFSCq), jumbo-frame OK
 * - IPV remap table (EAIRC) -- 8 PCP-to-internal-priority entries
 * - VLAN tag insertion (EAVCC.VIM + EAVTC C-/S-tags)
 * - VLAN tag stripping (EAVCC.VEM modes)
 * - RX tag filter (EARTFC) -- multicast group filter
 * - Cut-through queue (EACTQC + EACTDQDC) for low-latency forwarding
 * - Credit-based shaper (CBS) admin / oper registers (EACAEC, EACAIVCq,
 * EACAULCq, EACOEM, EACOIVMq, EACOULMq, EACGSM)
 * - Time-aware shaper (TAS / 802.1Qbv): per-queue gate lists, TAS RAM
 * reset and entry read-back (EATASC, EATASIGSC, EATASENCi, EATASGL0/1,
 * EATASGLR, EATASGR, EATASGRR, EATASRIRM)
 * - Per-port full statistics (EAUSMFSECN, EATFECN, EAFSECN, EADQOECN,
 * EADQSECN) and MIB counter snapshot
 * - All three error-IRQ blocks (EAEIS0/E/D0, EAEIS1/E/D1, EAEIS2/E/D2)
 * -- TX-done, RX-frame, error and link-change events
 * - DMA-descriptor-ring management hooks for TX (queue-depth + monitor)
 *
 * There is no per-port security-gate register: HUM Ch 32 publishes none,
 * and the ``EASCR`` the driver used to write was an FSP header artefact
 * (#539).
 *
 * Per-port state is kept in a small fixed-size table; both port
 * instances (m = 0, 1) share this driver via the::ra8_etha_port_t
 * argument. The shared MSTP gate ``k_ra8_mstp_eswm`` is reference-
 * counted by ra8_mstp, so init/deinit interleave safely with the
 * other ethernet sub-drivers (ra8_eth, ra8_eth_mfwd, ra8_eth_coma,
 * ra8_eth_gwca, ra8_eth_gptp, ra8_rmac).
 *
 * This umbrella header is intentionally thin: the declarations are
 * grouped by concern into self-contained sub-headers and pulled in
 * below so existing consumers that ``#include "ra8_etha.h"`` are
 * unaffected.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_etha_ops.h"
#include "ra8_etha_regs.h"
#include "ra8_etha_shaper.h"
#include "ra8_etha_types.h"
#include "ra8_rmac.h"

#ifdef __cplusplus
}
#endif
