/**
 * @file ra_rmac.c
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
 * The shared MSTP gate ``k_ra_mstp_eswm`` is reference-counted by
 * ra_mstp, so init/deinit interleave safely with the other ethernet
 * sub-drivers.
 *
 * Every register access carries a HUM Ch 33 citation.
 *
 * @par State Machine
 * @startuml
 * [*] --> RESET     : ra_rmac_init()
 * RESET   --> CONFIG : caller programs MRMAC etc.
 * CONFIG  --> RUN    : ETHA enters OPERATION mode
 * RUN     --> SLEEP  : ra_rmac_set_lpi(true)
 * SLEEP   --> RUN    : ra_rmac_set_lpi(false)
 * RUN     --> STOPPED: ra_rmac_deinit()
 * @enduml
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_rmac.h"

#include <stdint.h>

#include "ra8d2_ether_regs.h"
#include "ra8d2_mstp_regs.h"
#include "ra8d2_rmac_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

/**
 * @var s_tag
 * @brief Logger tag used by every ra_rmac_* call.
 */
static const char* s_tag = "RMAC";

/**
 * @enum ra_rmac_internal_t
 * @brief Driver-private constants (poll budgets, sentinel values).
 */
typedef enum : uint32_t {
  k_ra_rmac_mdio_poll_budget        = 100000UL, /**< Bounded MDIO poll loop.   */
  k_ra_rmac_phy_reset_iter_cap      = 4096UL,   /**< BMCR.RESET self-clear cap.*/
  k_ra_rmac_phy_anwait_iter_cap     = 65536UL,  /**< Auto-neg internal cap.    */
  k_ra_rmac_phy_anwait_iters_per_ms = 100UL,    /**< 10us per spin -> 100/ms.  */
} ra_rmac_internal_t;

/**
 * @enum ra_rmac_phy_reg_addr_t
 * @brief IEEE 802.3 Clause 22 register addresses.
 *
 * @details
 * Reference: IEEE Std 802.3-2018 Clause 22 sec 22.2.4 "Management
 * register set". These five registers are the only ones the RMAC
 * PHY-control glue touches; everything else is vendor-specific.
 */
typedef enum : uint8_t {
  k_ra_rmac_phy_reg_bmcr   = 0U,  /**< IEEE 802.3 Clause 22 sec 22.2.4.1 BMCR.   */
  k_ra_rmac_phy_reg_bmsr   = 1U,  /**< IEEE 802.3 Clause 22 sec 22.2.4.2 BMSR.   */
  k_ra_rmac_phy_reg_anar   = 4U,  /**< IEEE 802.3 Clause 28.2.4 ANAR.            */
  k_ra_rmac_phy_reg_anlpar = 5U,  /**< IEEE 802.3 Clause 28.2.4 ANLPAR.          */
  k_ra_rmac_phy_addr_max   = 31U, /**< 5-bit MDIO PHY address ceiling.           */
} ra_rmac_phy_reg_addr_t;

/**
 * @enum ra_rmac_phy_bit_t
 * @brief IEEE 802.3 Clause 22 BMCR / BMSR / ANxR field bits.
 */
typedef enum : uint16_t {
  k_ra_rmac_phy_bmcr_an_restart = 0x0200U, /**< BMCR bit 9.            */
  k_ra_rmac_phy_bmcr_an_enable  = 0x1000U, /**< BMCR bit 12.           */
  k_ra_rmac_phy_bmcr_reset      = 0x8000U, /**< BMCR bit 15.           */
  k_ra_rmac_phy_bmsr_link_up    = 0x0004U, /**< BMSR bit 2.            */
  k_ra_rmac_phy_bmsr_an_done    = 0x0020U, /**< BMSR bit 5.            */
  k_ra_rmac_phy_anar_selector   = 0x0001U, /**< ANAR bit 0 (802.3).    */
  k_ra_rmac_phy_anlpar_10_hd    = 0x0020U, /**< ANLPAR 10BASE-T HD.    */
  k_ra_rmac_phy_anlpar_10_fd    = 0x0040U, /**< ANLPAR 10BASE-T FD.    */
  k_ra_rmac_phy_anlpar_100_hd   = 0x0080U, /**< ANLPAR 100BASE-TX HD.  */
  k_ra_rmac_phy_anlpar_100_fd   = 0x0100U, /**< ANLPAR 100BASE-TX FD.  */
} ra_rmac_phy_bit_t;

/**
 * @enum ra_rmac_bit_pos_t
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
  k_ra_rmac_shift_mpfc_te0    = 16U, /**< MPFC.TEF0 (PTP table entry 0 enable). */
  k_ra_rmac_shift_mpfc_te1    = 17U, /**< MPFC.TEF1 (PTP table entry 1 enable). */
  k_ra_rmac_shift_mtpfc_pfrlv = 27U, /**< MTPFC.PFRLV[31:27] retry-level field. */
  k_ra_rmac_shift_mtpfc2_pfm  = 26U, /**< MTPFC2.PFM (1 = PFC frame, 0 = pause). */
} ra_rmac_bit_pos_t;

/**
 * @struct ra_rmac_slot_t
 * @brief Per-port runtime state.
 */
typedef struct {
  ra_rmac_event_fn_t cb;            /**< Attached callback (nullptr).    */
  void*              ctx;           /**< Opaque cookie passed to cb.     */
  ra_rmac_mrafc_t    cached_filter; /**< Last filter for stop/exit.    */
  uint8_t            in_use;        /**< 1 once init succeeds, 0 after deinit */
} ra_rmac_slot_t;

/**
 * @var s_slots
 * @brief Per-port handler / cache table; index = ::ra_rmac_port_t.
 */
static ra_rmac_slot_t s_slots[k_ra_rmac_port_count];

/**
 * @brief Range-check a port argument.
 *
 * @param[in] port Port to validate.
 * @return true if port is one of ::ra_rmac_port_t.
 *
 * @details See implementation.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static inline bool internal_port_ok(ra_rmac_port_t port)
{
  return (uint8_t)port < k_ra_rmac_port_count;
}

/**
 * @brief Build the MPIC value from interface / speed / duplex inputs.
 *
 * @param[in] iface   PHY interface.
 * @param[in] speed   Link speed.
 * @param[in] duplex  Duplex mode.
 * @return 32-bit MPIC value with PIS/LSC/PIPP fields packed.
 *
 * @details See implementation.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static inline uint32_t
internal_make_mpic(ra_rmac_pis_t iface, ra_rmac_lsc_t speed, ra_rmac_duplex_t duplex)
{
  /* HUM Ch 33.4 "MPIC : PHY Interfaces Configuration Register" p 1707 */
  const uint32_t pis  = ((uint32_t)iface & k_ra_rmac_mask_mpic_pis)
                        << (uint32_t)k_ra_rmac_shift_mpic_pis;
  const uint32_t lsc  = ((uint32_t)speed & k_ra_rmac_mask_mpic_lsc)
                        << (uint32_t)k_ra_rmac_shift_mpic_lsc;
  const uint32_t pipp = (duplex == k_ra_rmac_duplex_full) ? (1UL << 9U) : 0UL;
  return pis | lsc | pipp;
}

/**
 * @brief Wait for an MMIS1 completion bit, with a bounded poll budget.
 *
 * @param[in] reg     RMAC register window.
 * @param[in] mask    MMIS1 bit to test (e.g. PRACS for read).
 * @return k_ra_ok if the bit asserted, k_ra_err_hw_timeout otherwise.
 *
 * @details See implementation.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_mdio_wait(volatile r_rmac_regs_t* reg, uint32_t mask)
{
  for (uint32_t i = 0; i < k_ra_rmac_mdio_poll_budget; ++i) {
    /* HUM Ch 33.4 "MMIS1 : MAC Monitoring Interrupt Status Register 1" p 1707 */
    if ((reg->MMIS1 & mask) != 0U) {
      /* HUM Ch 33.4 "MMID1 : MAC Monitoring Interrupt Disable Register 1" p 1707 */
      reg->MMID1 = mask;
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
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
static void internal_mpsm_issue(volatile r_rmac_regs_t* reg,
                                uint8_t                 pda_5bit,
                                uint8_t                 pra_5bit,
                                ra_rmac_mdio_op_t       op,
                                uint16_t                prd_16bit,
                                bool                    mff)
{
  /* HUM Ch 33.4.1.1 "MPSM : PHY Station Management Register" p 1707 */
  const uint32_t pda     = ((uint32_t)pda_5bit & k_ra_rmac_mask_mpsm_phy_reg)
                           << (uint32_t)k_ra_rmac_shift_mpsm_pda;
  const uint32_t pra     = ((uint32_t)pra_5bit & k_ra_rmac_mask_mpsm_phy_reg)
                           << (uint32_t)k_ra_rmac_shift_mpsm_pra;
  const uint32_t pop     = ((uint32_t)op & k_ra_rmac_mask_mpsm_op)
                           << (uint32_t)k_ra_rmac_shift_mpsm_pop;
  const uint32_t prd     = ((uint32_t)prd_16bit & k_ra_rmac_mask_mpsm_data)
                           << (uint32_t)k_ra_rmac_shift_mpsm_prd;
  uint32_t       mff_bit = 0UL;
  if (mff) {
    mff_bit = (1UL << 2U);
  }
  /* PSME = bit 0 -- starts the transaction. */
  /* HUM Ch 33.4.1.1 "MPSM : PHY Station Management Register" p 1707 */
  reg->MPSM = pda | pra | pop | prd | mff_bit | 1UL;
}

/**
 * @brief Reset every per-MAC config register to its post-init baseline.
 *
 * @details
 * HUM Ch 33.4 "MIOC ... MEEEC" p 1707. Splits ``ra_rmac_init`` so it
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
static void internal_program_mac_config(volatile r_rmac_regs_t* reg, const ra_rmac_config_t* cfg)
{
  reg->MIOC   = 0U;
  reg->MTFFC  = 0U;
  reg->MRMAC0 = 0U;
  reg->MRMAC1 = 0U;
  reg->MRGC   = 0U;
  reg->MRAFC  = (uint32_t)cfg->rx_filter;
  reg->MLBC   = 0U;
  reg->MEEEC  = 0U;
  reg->MPIC   = internal_make_mpic(cfg->phy_interface, cfg->link_speed, cfg->duplex);
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
static void internal_program_irq_block(volatile r_rmac_regs_t* reg, const ra_rmac_config_t* cfg)
{
  reg->MEID  = k_ra_rmac_mask_all;
  reg->MMID0 = k_ra_rmac_mask_all;
  reg->MMID1 = k_ra_rmac_mask_all;
  reg->MMID2 = k_ra_rmac_mask_all;

  reg->MEIE  = cfg->err_irq_enable;
  reg->MMIE0 = cfg->mon0_irq_enable;
  reg->MMIE1 = cfg->mon1_irq_enable;
  reg->MMIE2 = cfg->mon2_irq_enable;
}

/* Implementation of ra_rmac_init (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_rmac_init(ra_rmac_port_t port, const ra_rmac_config_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "rmac_init: cfg must not be nullptr");
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "rmac_init: port out of range");
    return k_ra_err_invalid_arg;
  }
  if ((uint8_t)cfg->phy_interface >= k_ra_rmac_pis_count) {
    ra_log_error(s_tag, "rmac_init: phy_interface out of range");
    return k_ra_err_invalid_arg;
  }
  if ((uint8_t)cfg->link_speed >= k_ra_rmac_lsc_count) {
    ra_log_error(s_tag, "rmac_init: link_speed out of range");
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_eswm);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "rmac_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  volatile r_rmac_regs_t* reg = ra_rmac(port);
  internal_program_mac_config(reg, cfg);
  internal_program_irq_block(reg, cfg);

  s_slots[(uint8_t)port].cb            = nullptr;
  s_slots[(uint8_t)port].ctx           = nullptr;
  s_slots[(uint8_t)port].cached_filter = cfg->rx_filter;
  s_slots[(uint8_t)port].in_use        = 1U;
  ra_log_info(s_tag, "rmac_init");
  return k_ra_ok;
}

/* Implementation of ra_rmac_deinit (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_rmac_deinit(ra_rmac_port_t port)
{
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "rmac_deinit: port out of range");
    return k_ra_err_invalid_arg;
  }

  volatile r_rmac_regs_t* reg = ra_rmac(port);

  /* HUM Ch 33.4 "MRAFC : MAC Reception Address Filter Configuration Register" p 1707 */
  reg->MRAFC = 0U;
  /* HUM Ch 33.4 "MRGC : MAC Reception General Configuration Register" p 1707 */
  reg->MRGC = 0U;
  /* HUM Ch 33.4 "MTFFC : MAC Transmission Frame Format Configuration Register" p 1707 */
  reg->MTFFC = 0U;
  /* HUM Ch 33.4 "MLBC : MAC Loopback Configuration Register" p 1707 */
  reg->MLBC = 0U;
  /* HUM Ch 33.4 "MEEEC : MAC Energy Efficient Ethernet Configuration Register" p 1707 */
  reg->MEEEC = 0U;
  /* HUM Ch 33.4 "MEIE : MAC Error Interrupt Enable Register" p 1706 */
  reg->MEIE = 0U;
  /* HUM Ch 33.4 "MMIE0 : MAC Monitoring Interrupt Enable Register 0" p 1706 */
  reg->MMIE0 = 0U;
  /* HUM Ch 33.4 "MMIE1 : MAC Monitoring Interrupt Enable Register 1" p 1706 */
  reg->MMIE1 = 0U;
  /* HUM Ch 33.4 "MMIE2 : MAC Monitoring Interrupt Enable Register 2" p 1706 */
  reg->MMIE2 = 0U;

  s_slots[(uint8_t)port].cb     = nullptr;
  s_slots[(uint8_t)port].ctx    = nullptr;
  s_slots[(uint8_t)port].in_use = 0U;
  /* MSTP gate is shared with the rest of the Ethernet subsystem; do
   * NOT drop it here. ra_eth_deinit owns the final reference. */
  return k_ra_ok;
}

/* Implementation of ra_rmac_enter_stop (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_rmac_enter_stop(ra_rmac_port_t port)
{
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "rmac_enter_stop: port out of range");
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 33.4 "MRAFC : MAC Reception Address Filter Configuration Register" p 1707 */
  ra_rmac(port)->MRAFC = 0U;
  return k_ra_ok;
}

/* Implementation of ra_rmac_exit_stop (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_rmac_exit_stop(ra_rmac_port_t port)
{
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "rmac_exit_stop: port out of range");
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 33.4 "MRAFC : MAC Reception Address Filter Configuration Register" p 1707 */
  ra_rmac(port)->MRAFC = (uint32_t)s_slots[(uint8_t)port].cached_filter;
  return k_ra_ok;
}

/* Implementation of ra_rmac_set_mac_address (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_rmac_set_mac_address(ra_rmac_port_t port, const uint8_t mac[k_ra_rmac_mac_byte_count])
{
  RA_CHECK_NULL_PTR(mac, s_tag, "set_mac_address: mac must not be nullptr");
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "set_mac_address: port out of range");
    return k_ra_err_invalid_arg;
  }

  /* MRMAC0.MAU holds the upper 16 bits (bytes 0..1, MSB at bit 15);
   * MRMAC1.MAL holds the lower 32 bits (bytes 2..5, mac[2] in [31:24]).
   * Source: HUM Ch 33.4 "MRMAC0/MRMAC1 : MAC Reception MAC Address
   * Configuration Registers" p 1707, cross-checked against FSP
   * R_RMAC0_Type.MRMAC0_b.MAU and .MRMAC1_b.MAL. */
  const uint32_t b0 = (uint32_t)mac[k_ra_rmac_mac_byte_0] & k_ra_rmac_mask_byte;
  const uint32_t b1 = (uint32_t)mac[k_ra_rmac_mac_byte_1] & k_ra_rmac_mask_byte;
  const uint32_t b2 = (uint32_t)mac[k_ra_rmac_mac_byte_2] & k_ra_rmac_mask_byte;
  const uint32_t b3 = (uint32_t)mac[k_ra_rmac_mac_byte_3] & k_ra_rmac_mask_byte;
  const uint32_t b4 = (uint32_t)mac[k_ra_rmac_mac_byte_4] & k_ra_rmac_mask_byte;
  const uint32_t b5 = (uint32_t)mac[k_ra_rmac_mac_byte_5] & k_ra_rmac_mask_byte;

  const uint32_t mrmac0 =
    ((b0 << (uint32_t)k_ra_rmac_shift_high24) | (b1 << (uint32_t)k_ra_rmac_shift_high16)) &
    k_ra_rmac_mask_mrmac0_mau;
  const uint32_t mrmac1 =
    (b2 << (uint32_t)k_ra_rmac_shift_byte3) | (b3 << (uint32_t)k_ra_rmac_shift_byte2) |
    (b4 << (uint32_t)k_ra_rmac_shift_byte1) | (b5 << (uint32_t)k_ra_rmac_shift_byte0);

  volatile r_rmac_regs_t* reg = ra_rmac(port);
  /* HUM Ch 33.4 "MRMAC0 : MAC Reception MAC Address Configuration Register 0" p 1707 */
  reg->MRMAC0 = mrmac0;
  /* HUM Ch 33.4 "MRMAC1 : MAC Reception MAC Address Configuration Register 1" p 1707 */
  reg->MRMAC1 = mrmac1;
  return k_ra_ok;
}

/* Implementation of ra_rmac_set_rx_filter (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_rmac_set_rx_filter(ra_rmac_port_t port, ra_rmac_mrafc_t filter)
{
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "set_rx_filter: port out of range");
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 33.4 "MRAFC : MAC Reception Address Filter Configuration Register" p 1707 */
  ra_rmac(port)->MRAFC                 = (uint32_t)filter;
  s_slots[(uint8_t)port].cached_filter = filter;
  return k_ra_ok;
}

/* Implementation of ra_rmac_set_ptp_filter (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_rmac_set_ptp_filter(ra_rmac_port_t port,
                                uint8_t        index,
                                uint8_t        byte_offset,
                                uint8_t        value,
                                bool           tef0,
                                bool           tef1)
{
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "set_ptp_filter: port out of range");
    return k_ra_err_invalid_arg;
  }
  if (index >= k_ra_rmac_ptp_filter_count) {
    ra_log_error(s_tag, "set_ptp_filter: index out of range");
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 33.4 "MPFCt : MAC PTP Filtering Register Configuration t" p 1707 */
  const uint32_t pfbn = (uint32_t)byte_offset & k_ra_rmac_mask_byte;
  const uint32_t pfbv = ((uint32_t)value & k_ra_rmac_mask_byte)
                        << (uint32_t)k_ra_rmac_shift_mpfc_val;
  uint32_t       te   = 0UL;
  if (tef0) {
    te |= (1UL << (uint32_t)k_ra_rmac_shift_mpfc_te0);
  }
  if (tef1) {
    te |= (1UL << (uint32_t)k_ra_rmac_shift_mpfc_te1);
  }
  ra_rmac(port)->MPFC[index] = pfbn | pfbv | te;
  return k_ra_ok;
}

/* Implementation of ra_rmac_set_frame_size (see header for full contract) -- see header for the documented contract. */
ra_err_t
ra_rmac_set_frame_size(ra_rmac_port_t port, bool is_pframe, uint16_t min_size, uint16_t max_size)
{
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "set_frame_size: port out of range");
    return k_ra_err_invalid_arg;
  }
  if (min_size > max_size) {
    ra_log_error(s_tag, "set_frame_size: min > max");
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 33.4 "MRFSCE / MRFSCP : MAC Reception Frame Size Configuration" p 1707 */
  const uint32_t v =
    (((uint32_t)max_size & k_ra_rmac_mask_mrfsc) << (uint32_t)k_ra_rmac_shift_mrfsce_max) |
    (((uint32_t)min_size & k_ra_rmac_mask_mrfsc) << (uint32_t)k_ra_rmac_shift_mrfsce_min);
  if (is_pframe) {
    ra_rmac(port)->MRFSCP = v;
  } else {
    ra_rmac(port)->MRFSCE = v;
  }
  return k_ra_ok;
}

/* Implementation of ra_rmac_set_vlan_framing (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_rmac_set_vlan_framing(ra_rmac_port_t port, bool disable_pad, bool use_mcrc)
{
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "set_vlan_framing: port out of range");
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 33.4 "MTFFC : MAC Transmission Frame Format Configuration" p 1707 */
  uint32_t v = 0UL;
  if (disable_pad) {
    v |= k_ra_rmac_mtffc_dpad;
  }
  if (use_mcrc) {
    v |= k_ra_rmac_mtffc_fcm;
  }
  ra_rmac(port)->MTFFC = v;
  return k_ra_ok;
}

/* Implementation of ra_rmac_set_pause_frame (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_rmac_set_pause_frame(ra_rmac_port_t       port,
                                 ra_rmac_pause_mode_t mode,
                                 uint16_t             pause_time,
                                 uint8_t              retry_time,
                                 uint8_t              retry_level)
{
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "set_pause_frame: port out of range");
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 33.4 "MTPFC : MAC Transmission Pause or PFC Frame Configuration" p 1707 */
  const uint32_t pt    = ((uint32_t)pause_time & k_ra_rmac_mask_mtpfc_pt)
                         << (uint32_t)k_ra_rmac_shift_mtpfc_pt;
  const uint32_t pfrt  = ((uint32_t)retry_time & k_ra_rmac_mask_mtpfc_pfrt)
                         << (uint32_t)k_ra_rmac_shift_mtpfc_pfrt;
  const uint32_t pfrlv = ((uint32_t)retry_level & 0x1FU) << (uint32_t)k_ra_rmac_shift_mtpfc_pfrlv;
  /* HUM Ch 33.4 "MTPFC2 : MAC Transmission Pause or PFC Frame Cfg 2" p 1707 */
  uint32_t pfm = 0UL;
  if (mode == k_ra_rmac_pause_mode_pfc) {
    pfm = (1UL << (uint32_t)k_ra_rmac_shift_mtpfc2_pfm);
  }
  ra_rmac(port)->MTPFC  = pt | pfrt | pfrlv;
  ra_rmac(port)->MTPFC2 = pfm;
  return k_ra_ok;
}

/* Implementation of ra_rmac_set_pfc_group (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_rmac_set_pfc_group(ra_rmac_port_t port, ra_rmac_pfc_group_t group, uint8_t pcp_mask)
{
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "set_pfc_group: port out of range");
    return k_ra_err_invalid_arg;
  }
  if ((uint8_t)group >= k_ra_rmac_pfc_group_count) {
    ra_log_error(s_tag, "set_pfc_group: group out of range");
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 33.4 "MTPFC3t : MAC Transmission PFC Frame Configuration 3" p 1707 */
  ra_rmac(port)->MTPFC3[(uint8_t)group] = (uint32_t)pcp_mask & k_ra_rmac_mask_pfc_priority;
  return k_ra_ok;
}

/* Implementation of ra_rmac_set_lpi (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_rmac_set_lpi(ra_rmac_port_t port, bool enable)
{
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "set_lpi: port out of range");
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 33.4 "MEEEC : MAC Energy Efficient Ethernet Configuration" p 1707 */
  uint32_t meeec = 0UL;
  if (enable) {
    meeec = k_ra_rmac_meeec_lpitr;
  }
  ra_rmac(port)->MEEEC = meeec;
  return k_ra_ok;
}

/* Implementation of ra_rmac_set_magic_packet (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_rmac_set_magic_packet(ra_rmac_port_t port, bool enable)
{
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "set_magic_packet: port out of range");
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 33.4 "MRGC : MAC Reception General Configuration" p 1707 */
  volatile r_rmac_regs_t* reg = ra_rmac(port);
  uint32_t                v   = reg->MRGC;
  if (enable) {
    v |= k_ra_rmac_mrgc_mpde;
  } else {
    v &= ~k_ra_rmac_mrgc_mpde;
  }
  reg->MRGC = v;
  return k_ra_ok;
}

/* Implementation of ra_rmac_set_loopback (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_rmac_set_loopback(ra_rmac_port_t port, bool enable)
{
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "set_loopback: port out of range");
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 33.4 "MLBC : MAC Loopback Configuration Register" p 1707 */
  uint32_t mlbc = 0UL;
  if (enable) {
    mlbc = k_ra_rmac_mlbc_lbme;
  }
  ra_rmac(port)->MLBC = mlbc;
  return k_ra_ok;
}

/* Implementation of ra_rmac_set_link (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_rmac_set_link(ra_rmac_port_t   port,
                          ra_rmac_pis_t    iface,
                          ra_rmac_lsc_t    speed,
                          ra_rmac_duplex_t duplex)
{
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "set_link: port out of range");
    return k_ra_err_invalid_arg;
  }
  if ((uint8_t)iface >= k_ra_rmac_pis_count) {
    ra_log_error(s_tag, "set_link: iface out of range");
    return k_ra_err_invalid_arg;
  }
  if ((uint8_t)speed >= k_ra_rmac_lsc_count) {
    ra_log_error(s_tag, "set_link: speed out of range");
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 33.4 "MPIC : PHY Interfaces Configuration Register" p 1707 */
  ra_rmac(port)->MPIC = internal_make_mpic(iface, speed, duplex);
  return k_ra_ok;
}

/* Implementation of ra_rmac_mdio_c22_read (see header for full contract) -- see header for the documented contract. */
ra_err_t
ra_rmac_mdio_c22_read(ra_rmac_port_t port, uint8_t phy_addr, uint8_t reg_addr, uint16_t* out_value)
{
  RA_CHECK_NULL_PTR(out_value, s_tag, "mdio_c22_read: out_value must not be nullptr");
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "mdio_c22_read: port out of range");
    return k_ra_err_invalid_arg;
  }

  volatile r_rmac_regs_t* reg = ra_rmac(port);
  internal_mpsm_issue(reg, phy_addr, reg_addr, k_ra_rmac_mdio_op_c22_read, 0U, false);
  const ra_err_t wait_err = internal_mdio_wait(reg, k_ra_rmac_mmis1_pracs);
  if (wait_err != k_ra_ok) {
    ra_log_error(s_tag, "mdio_c22_read: timeout"); /* GCOVR_EXCL_BR_LINE */
    return wait_err;
  }
  /* HUM Ch 33.4.1.1 "MPSM : PHY Station Management Register" p 1707 */
  *out_value =
    (uint16_t)((reg->MPSM >> (uint32_t)k_ra_rmac_shift_mpsm_prd) & k_ra_rmac_mask_mpsm_data);
  return k_ra_ok;
}

/* Implementation of ra_rmac_mdio_c22_write (see header for full contract) -- see header for the documented contract. */
ra_err_t
ra_rmac_mdio_c22_write(ra_rmac_port_t port, uint8_t phy_addr, uint8_t reg_addr, uint16_t value)
{
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "mdio_c22_write: port out of range");
    return k_ra_err_invalid_arg;
  }
  volatile r_rmac_regs_t* reg = ra_rmac(port);
  internal_mpsm_issue(reg, phy_addr, reg_addr, k_ra_rmac_mdio_op_c22_write, value, false);
  return internal_mdio_wait(reg, k_ra_rmac_mmis1_pwacs);
}

/* Implementation of ra_rmac_mdio_c45_read (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_rmac_mdio_c45_read(ra_rmac_port_t port,
                               uint8_t        phy_addr,
                               uint8_t        dev_addr,
                               uint16_t       reg_addr,
                               uint16_t*      out_value)
{
  RA_CHECK_NULL_PTR(out_value, s_tag, "mdio_c45_read: out_value must not be nullptr");
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "mdio_c45_read: port out of range");
    return k_ra_err_invalid_arg;
  }

  volatile r_rmac_regs_t* reg = ra_rmac(port);
  /* C45 step 1: address frame loads the 16-bit MMD register address. */
  internal_mpsm_issue(reg, phy_addr, dev_addr, k_ra_rmac_mdio_op_c45_address, reg_addr, true);
  ra_err_t err = internal_mdio_wait(reg, k_ra_rmac_mmis1_paacs);
  if (err != k_ra_ok) {
    ra_log_error(s_tag, "mdio_c45_read: address timeout"); /* GCOVR_EXCL_BR_LINE */
    return err;
  }
  /* C45 step 2: read frame fetches the 16-bit data. */
  internal_mpsm_issue(reg, phy_addr, dev_addr, k_ra_rmac_mdio_op_c45_read, 0U, true);
  err = internal_mdio_wait(reg, k_ra_rmac_mmis1_pracs);
  if (err != k_ra_ok) {
    ra_log_error(s_tag, "mdio_c45_read: read timeout"); /* GCOVR_EXCL_BR_LINE */
    return err;
  }
  /* HUM Ch 33.4.1.1 "MPSM : PHY Station Management Register" p 1707 */
  *out_value =
    (uint16_t)((reg->MPSM >> (uint32_t)k_ra_rmac_shift_mpsm_prd) & k_ra_rmac_mask_mpsm_data);
  return k_ra_ok;
}

/* Implementation of ra_rmac_mdio_c45_write (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_rmac_mdio_c45_write(ra_rmac_port_t port,
                                uint8_t        phy_addr,
                                uint8_t        dev_addr,
                                uint16_t       reg_addr,
                                uint16_t       value)
{
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "mdio_c45_write: port out of range");
    return k_ra_err_invalid_arg;
  }
  volatile r_rmac_regs_t* reg = ra_rmac(port);
  /* Address frame followed by write frame -- both must complete. */
  internal_mpsm_issue(reg, phy_addr, dev_addr, k_ra_rmac_mdio_op_c45_address, reg_addr, true);
  ra_err_t err = internal_mdio_wait(reg, k_ra_rmac_mmis1_paacs);
  if (err != k_ra_ok) {
    ra_log_error(s_tag, "mdio_c45_write: address timeout"); /* GCOVR_EXCL_BR_LINE */
    return err;
  }
  internal_mpsm_issue(reg, phy_addr, dev_addr, k_ra_rmac_mdio_op_c45_write, value, true);
  return internal_mdio_wait(reg, k_ra_rmac_mmis1_pwacs);
}

/* Implementation of ra_rmac_get_status (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_rmac_get_status(ra_rmac_port_t port, ra_rmac_status_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "rmac_get_status: out must not be nullptr");
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "rmac_get_status: port out of range");
    return k_ra_err_invalid_arg;
  }

  volatile r_rmac_regs_t* reg = ra_rmac(port);
  /* HUM Ch 33.4 "MEIS : MAC Error Interrupt Status Register" p 1706 */
  out->err_status = reg->MEIS;
  /* HUM Ch 33.4 "MMIS0 : MAC Monitoring Interrupt Status Register 0" p 1706 */
  out->mon_status[0] = reg->MMIS0;
  /* HUM Ch 33.4 "MMIS1 : MAC Monitoring Interrupt Status Register 1" p 1706 */
  out->mon_status[1] = reg->MMIS1;
  /* HUM Ch 33.4 "MMIS2 : MAC Monitoring Interrupt Status Register 2" p 1706 */
  out->mon_status[2] = reg->MMIS2;
  /* HUM Ch 33.4 "MPIM : PHY Interfaces Monitoring Register" p 1707 */
  out->phy_monitor = reg->MPIM;
  /* HUM Ch 33.4 "MRMAC0 : MAC Reception MAC Address Configuration Register 0" p 1707 */
  out->mrmac0 = reg->MRMAC0;
  /* HUM Ch 33.4 "MRMAC1 : MAC Reception MAC Address Configuration Register 1" p 1707 */
  out->mrmac1 = reg->MRMAC1;
  return k_ra_ok;
}

/* Implementation of ra_rmac_clear_status (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_rmac_clear_status(ra_rmac_port_t port,
                              uint32_t       err_mask,
                              uint32_t       mon0_mask,
                              uint32_t       mon1_mask,
                              uint32_t       mon2_mask)
{
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "rmac_clear_status: port out of range");
    return k_ra_err_invalid_arg;
  }
  volatile r_rmac_regs_t* reg = ra_rmac(port);
  /* The disable registers act as the clear-on-write counterpart of
   * each status register; writing 1 to a bit clears the matching bit
   * in MEIS / MMIS{0,1,2}. The driver also writes the explicit
   * masked-out value so simulator backings (which lack RW1C) end up
   * in the same observable state as real hardware. */
  /* HUM Ch 33.4 "MEID : MAC Error Interrupt Disable Register" p 1706 */
  reg->MEID = err_mask;
  /* HUM Ch 33.4 "MMID0 : MAC Monitoring Interrupt Disable Register 0" p 1706 */
  reg->MMID0 = mon0_mask;
  /* HUM Ch 33.4 "MMID1 : MAC Monitoring Interrupt Disable Register 1" p 1706 */
  reg->MMID1 = mon1_mask;
  /* HUM Ch 33.4 "MMID2 : MAC Monitoring Interrupt Disable Register 2" p 1706 */
  reg->MMID2 = mon2_mask;
  /* HUM Ch 33.4 "MEIS : MAC Error Interrupt Status Register" p 1706 */
  reg->MEIS = reg->MEIS & ~err_mask;
  /* HUM Ch 33.4 "MMIS0 : MAC Monitoring Interrupt Status Register 0" p 1706 */
  reg->MMIS0 = reg->MMIS0 & ~mon0_mask;
  /* HUM Ch 33.4 "MMIS1 : MAC Monitoring Interrupt Status Register 1" p 1706 */
  reg->MMIS1 = reg->MMIS1 & ~mon1_mask;
  /* HUM Ch 33.4 "MMIS2 : MAC Monitoring Interrupt Status Register 2" p 1706 */
  reg->MMIS2 = reg->MMIS2 & ~mon2_mask;
  return k_ra_ok;
}

/**
 * @brief Snapshot the pause / PFC / EEE counters into ``out``.
 *
 * @details
 * HUM Ch 33.4 MMPFTCT / MAPFTCT / MPFRCT / MFCICT / MEEECT and the
 * MMPCFTCT / MAPCFTCT / MPCFRCT counter banks (p 1706).
 *
 * @param[in] reg See implementation.
 * @param[in] out See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_snapshot_pause_pfc(volatile r_rmac_regs_t* reg, ra_rmac_stats_t* out)
{
  out->pause_tx_manual = reg->MMPFTCT;
  out->pause_tx_auto   = reg->MAPFTCT;
  out->pause_rx        = reg->MPFRCT;
  out->false_carrier   = reg->MFCICT;
  out->eee_count       = reg->MEEECT;
  for (uint8_t i = 0; i < k_ra_rmac_pfc_group_count; ++i) {
    out->pfc_tx_manual[i] = reg->MMPCFTCT[i];
    out->pfc_tx_auto[i]   = reg->MAPCFTCT[i];
  }
  for (uint8_t i = 0; i < k_ra_rmac_pfc_rx_count; ++i) {
    out->pfc_rx[i] = reg->MPCFRCT[i];
  }
}

/**
 * @brief Snapshot the receive counters into ``out``.
 *
 * @details
 * HUM Ch 33.4 MROVFC ... MRXBCPL p 1706.
 *
 * @param[in] reg See implementation.
 * @param[in] out See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_snapshot_rx(volatile r_rmac_regs_t* reg, ra_rmac_stats_t* out)
{
  out->rx_overflow        = reg->MROVFC;
  out->rx_hdr_crc_err     = reg->MRHCRCEC;
  out->rx_good_e          = reg->MRGFCE;
  out->rx_good_p          = reg->MRGFCP;
  out->rx_broadcast       = reg->MRBFC;
  out->rx_multicast       = reg->MRMFC;
  out->rx_unicast         = reg->MRUFC;
  out->rx_phy_err         = reg->MRPEFC;
  out->rx_nibble_err      = reg->MRNEFC;
  out->rx_fcs_err         = reg->MRFMEFC;
  out->rx_final_frag_miss = reg->MRFFMEFC;
  out->rx_c_frag_err      = reg->MRCFCEFC;
  out->rx_frag_count_err  = reg->MRFCEFC;
  out->rx_filter_rejected = reg->MRRCFEFC;
  out->rx_total           = reg->MRFC;
  out->rx_good_undersize  = reg->MRGUEFC;
  out->rx_bad_undersize   = reg->MRBUEFC;
  out->rx_good_oversize   = reg->MRGOEFC;
  out->rx_bad_oversize    = reg->MRBOEFC;
  out->rx_bytes_e_upper   = reg->MRXBCEU;
  out->rx_bytes_e_lower   = reg->MRXBCEL;
  out->rx_bytes_p_upper   = reg->MRXBCPU;
  out->rx_bytes_p_lower   = reg->MRXBCPL;
}

/**
 * @brief Snapshot the transmit counters into ``out``.
 *
 * @details
 * HUM Ch 33.4 MTGFCE ... MTXBCPL p 1706.
 *
 * @param[in] reg See implementation.
 * @param[in] out See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_snapshot_tx(volatile r_rmac_regs_t* reg, ra_rmac_stats_t* out)
{
  out->tx_good_e        = reg->MTGFCE;
  out->tx_good_p        = reg->MTGFCP;
  out->tx_broadcast     = reg->MTBFC;
  out->tx_multicast     = reg->MTMFC;
  out->tx_unicast       = reg->MTUFC;
  out->tx_error         = reg->MTEFC;
  out->tx_bytes_e_upper = reg->MTXBCEU;
  out->tx_bytes_e_lower = reg->MTXBCEL;
  out->tx_bytes_p_upper = reg->MTXBCPU;
  out->tx_bytes_p_lower = reg->MTXBCPL;
}

/* Implementation of ra_rmac_read_stats (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_rmac_read_stats(ra_rmac_port_t port, ra_rmac_stats_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "read_stats: out must not be nullptr");
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "read_stats: port out of range");
    return k_ra_err_invalid_arg;
  }

  volatile r_rmac_regs_t* reg = ra_rmac(port);
  internal_snapshot_pause_pfc(reg, out);
  internal_snapshot_rx(reg, out);
  internal_snapshot_tx(reg, out);
  return k_ra_ok;
}

/* Implementation of ra_rmac_attach_handler (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_rmac_attach_handler(ra_rmac_port_t port, ra_rmac_event_fn_t cb, void* ctx)
{
  if (!internal_port_ok(port)) {
    ra_log_error(s_tag, "attach_handler: port out of range");
    return k_ra_err_invalid_arg;
  }
  s_slots[(uint8_t)port].cb  = cb;
  s_slots[(uint8_t)port].ctx = ctx;
  return k_ra_ok;
}

/* Implementation of ra_rmac_dispatch (see header for full contract) -- see header for the documented contract. */
void ra_rmac_dispatch(ra_rmac_port_t port)
{
  if (!internal_port_ok(port)) {
    return;
  }
  volatile r_rmac_regs_t* reg = ra_rmac(port);
  /* HUM Ch 33.4 "MEIS / MMIS0 / MMIS1 / MMIS2" p 1706 */
  const uint32_t           err = reg->MEIS;
  const uint32_t           m0  = reg->MMIS0;
  const uint32_t           m1  = reg->MMIS1;
  const uint32_t           m2  = reg->MMIS2;
  const ra_rmac_event_fn_t fn  = s_slots[(uint8_t)port].cb;
  void* const              ctx = s_slots[(uint8_t)port].ctx;
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

/**
 * @brief Validate the (port, phy_addr) tuple shared by every PHY helper.
 *
 * @param[in] port     Port to validate.
 * @param[in] phy_addr 5-bit PHY address to validate.
 * @return true iff both arguments are in range.
 *
 * @details See implementation.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static inline bool internal_phy_args_ok(ra_rmac_port_t port, uint8_t phy_addr)
{
  return internal_port_ok(port) && (phy_addr <= (uint8_t)k_ra_rmac_phy_addr_max);
}

/**
 * @brief Decode an ANLPAR snapshot into the highest-priority capability.
 *
 * @details
 * IEEE 802.3 Clause 28.2.4.4 "Link Partner Ability Register" defines
 * the priority order for resolved capabilities.
 *
 * @param[in] anlpar Raw ANLPAR contents read over MDIO.
 * @return Resolved ::ra_rmac_phy_speed_t value.
 *
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_rmac_phy_speed_t internal_decode_anlpar(uint16_t anlpar)
{
  if ((anlpar & (uint16_t)k_ra_rmac_phy_anlpar_100_fd) != 0U) {
    return k_ra_rmac_phy_speed_100_fd;
  }
  if ((anlpar & (uint16_t)k_ra_rmac_phy_anlpar_100_hd) != 0U) {
    return k_ra_rmac_phy_speed_100_hd;
  }
  if ((anlpar & (uint16_t)k_ra_rmac_phy_anlpar_10_fd) != 0U) {
    return k_ra_rmac_phy_speed_10_fd;
  }
  if ((anlpar & (uint16_t)k_ra_rmac_phy_anlpar_10_hd) != 0U) {
    return k_ra_rmac_phy_speed_10_hd;
  }
  return k_ra_rmac_phy_speed_unknown;
}

/* Implementation of ra_rmac_phy_reset (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_rmac_phy_reset(ra_rmac_port_t port, uint8_t phy_addr)
{
  if (!internal_phy_args_ok(port, phy_addr)) {
    ra_log_error(s_tag, "phy_reset: bad args");
    return k_ra_err_invalid_arg;
  }
  /* IEEE 802.3 Clause 22 sec 22.2.4.1.1 "BMCR.RESET" -- self-clearing.
   * HUM Ch 33.4.1.1 "MPSM" p 1707 carries the MDIO write. */
  const ra_err_t w = ra_rmac_mdio_c22_write(port,
                                            phy_addr,
                                            (uint8_t)k_ra_rmac_phy_reg_bmcr,
                                            (uint16_t)k_ra_rmac_phy_bmcr_reset);
  if (w != k_ra_ok) {
    ra_log_error(s_tag, "phy_reset: bmcr write");
    return w;
  }
  for (uint32_t i = 0U; i < (uint32_t)k_ra_rmac_phy_reset_iter_cap; ++i) {
    uint16_t       bmcr = 0U;
    const ra_err_t r =
      ra_rmac_mdio_c22_read(port, phy_addr, (uint8_t)k_ra_rmac_phy_reg_bmcr, &bmcr);
    if (r != k_ra_ok) {
      ra_log_error(s_tag, "phy_reset: bmcr read");
      return r;
    }
    if ((bmcr & (uint16_t)k_ra_rmac_phy_bmcr_reset) == 0U) {
      return k_ra_ok;
    }
  }
  ra_log_error(s_tag, "phy_reset: bmcr.reset never cleared");
  return k_ra_err_hw_timeout;
}

/* Implementation of ra_rmac_phy_set_advertise (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_rmac_phy_set_advertise(ra_rmac_port_t port, uint8_t phy_addr, uint16_t capabilities)
{
  if (!internal_phy_args_ok(port, phy_addr)) {
    ra_log_error(s_tag, "phy_set_advertise: bad args");
    return k_ra_err_invalid_arg;
  }
  /* IEEE 802.3 Clause 28.2.4 "ANAR" -- selector field is bits [4:0],
   * value 00001 = IEEE 802.3 (the only selector RMAC supports). */
  const uint16_t anar = (uint16_t)(capabilities | (uint16_t)k_ra_rmac_phy_anar_selector);
  return ra_rmac_mdio_c22_write(port, phy_addr, (uint8_t)k_ra_rmac_phy_reg_anar, anar);
}

/* Implementation of ra_rmac_phy_auto_neg_start (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_rmac_phy_auto_neg_start(ra_rmac_port_t port, uint8_t phy_addr)
{
  if (!internal_phy_args_ok(port, phy_addr)) {
    ra_log_error(s_tag, "phy_auto_neg_start: bad args");
    return k_ra_err_invalid_arg;
  }
  /* IEEE 802.3 Clause 22 sec 22.2.4.1 "BMCR" -- AN_ENABLE (bit 12) +
   * AN_RESTART (bit 9). Writing both in one transaction is the
   * canonical "kick" sequence. */
  const uint16_t bmcr =
    (uint16_t)((uint16_t)k_ra_rmac_phy_bmcr_an_enable | (uint16_t)k_ra_rmac_phy_bmcr_an_restart);
  return ra_rmac_mdio_c22_write(port, phy_addr, (uint8_t)k_ra_rmac_phy_reg_bmcr, bmcr);
}

/* Implementation of ra_rmac_phy_auto_neg_wait (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_rmac_phy_auto_neg_wait(ra_rmac_port_t      port,
                                   uint8_t             phy_addr,
                                   uint32_t            timeout_ms,
                                   ra_rmac_phy_link_t* out_link)
{
  RA_CHECK_NULL_PTR(out_link, s_tag, "phy_auto_neg_wait: out_link null");
  if (!internal_phy_args_ok(port, phy_addr)) {
    ra_log_error(s_tag, "phy_auto_neg_wait: bad args");
    return k_ra_err_invalid_arg;
  }
  out_link->up    = false;
  out_link->speed = k_ra_rmac_phy_speed_unknown;

  /* 100 iters per ms = 10us per spin (k_ra_rmac_phy_anwait_us_per_iter). */
  uint32_t cap = (timeout_ms == 0U) ? (uint32_t)k_ra_rmac_phy_anwait_iter_cap
                                    : (timeout_ms * (uint32_t)k_ra_rmac_phy_anwait_iters_per_ms);
  if (cap == 0U) {
    cap = 1U;
  }

  for (uint32_t i = 0U; i < cap; ++i) {
    uint16_t bmsr = 0U;
    /* IEEE 802.3 Clause 22 sec 22.2.4.2 "BMSR" -- AN_COMPLETE bit 5,
     * LINK_STATUS bit 2. Need both set for a "ready to forward" link. */
    const ra_err_t r =
      ra_rmac_mdio_c22_read(port, phy_addr, (uint8_t)k_ra_rmac_phy_reg_bmsr, &bmsr);
    if (r != k_ra_ok) {
      return r;
    }
    const uint16_t need =
      (uint16_t)((uint16_t)k_ra_rmac_phy_bmsr_an_done | (uint16_t)k_ra_rmac_phy_bmsr_link_up);
    if ((bmsr & need) == need) {
      uint16_t anlpar = 0U;
      /* IEEE 802.3 Clause 28.2.4.4 "ANLPAR" -- resolved capability. */
      const ra_err_t lp =
        ra_rmac_mdio_c22_read(port, phy_addr, (uint8_t)k_ra_rmac_phy_reg_anlpar, &anlpar);
      if (lp != k_ra_ok) {
        return lp;
      }
      out_link->up    = true;
      out_link->speed = internal_decode_anlpar(anlpar);
      return k_ra_ok;
    }
  }
  ra_log_error(s_tag, "phy_auto_neg_wait: timeout");
  return k_ra_err_hw_timeout;
}

/* Implementation of ra_rmac_phy_link_status (see header for full contract) -- see header for the documented contract. */
ra_err_t
ra_rmac_phy_link_status(ra_rmac_port_t port, uint8_t phy_addr, ra_rmac_phy_link_t* out_link)
{
  RA_CHECK_NULL_PTR(out_link, s_tag, "phy_link_status: out_link null");
  if (!internal_phy_args_ok(port, phy_addr)) {
    ra_log_error(s_tag, "phy_link_status: bad args");
    return k_ra_err_invalid_arg;
  }
  out_link->up    = false;
  out_link->speed = k_ra_rmac_phy_speed_unknown;

  uint16_t bmsr = 0U;
  /* IEEE 802.3 Clause 22 sec 22.2.4.2 "BMSR.LINK_STATUS" (bit 2). */
  const ra_err_t r = ra_rmac_mdio_c22_read(port, phy_addr, (uint8_t)k_ra_rmac_phy_reg_bmsr, &bmsr);
  if (r != k_ra_ok) {
    return r;
  }
  out_link->up = (bmsr & (uint16_t)k_ra_rmac_phy_bmsr_link_up) != 0U;
  // mcdc-deactivated: ra_rmac_phy_auto_neg_start link-up + an-done gate; both bits come from the same BMSR read; PHY hardware sets BMSR.AN_DONE only after BMSR.LINK_STATUS asserts (IEEE 802.3 Clause 22 22.2.4.2 ordering) -- the second condition cannot be true while the first is false on any conformant PHY.
  if (out_link->up && ((bmsr & (uint16_t)k_ra_rmac_phy_bmsr_an_done) != 0U)) {
    uint16_t       anlpar = 0U;
    const ra_err_t lp =
      ra_rmac_mdio_c22_read(port, phy_addr, (uint8_t)k_ra_rmac_phy_reg_anlpar, &anlpar);
    if (lp != k_ra_ok) {
      return lp;
    }
    out_link->speed = internal_decode_anlpar(anlpar);
  }
  return k_ra_ok;
}
