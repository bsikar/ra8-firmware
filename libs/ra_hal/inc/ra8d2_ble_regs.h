/**
 * @file ra8d2_ble_regs.h
 * @brief BLE controller register layout (stubbed) for the Renesas RA8D2
 *
 * @details
 * The RA8D2 group lists a Bluetooth Low Energy 5.x radio block in the
 * datasheet feature summary, but the public FSP CMSIS device file
 * `R7KA8D2KF_core0.h` does **not** publish a memory-mapped peripheral
 * struct for it. The Renesas BLE controller is a vendor-private
 * sub-system: its bring-up sequence loads an encrypted firmware patch
 * blob into a controller-private RAM via a small mailbox-style
 * register window, and the host then talks to it over a virtual HCI
 * transport (HCI command, HCI event, HCI ACL data) instead of poking
 * radio registers directly.
 *
 * This header therefore models the **HCI mailbox surface** of the BLE
 * block rather than the radio physical layer. The field names and
 * sequencing match the Bluetooth Core 5.3 specification, Volume 4
 * "Host Controller Interface", Part E "HCI Functional Specification"
 * (the part that is portable across silicon vendors). The exact byte
 * offsets below are placeholders pending datasheet release of the
 * RA8D2 BLE block addendum.
 *
 * @warning Real silicon needs the Renesas-supplied BLE firmware patch
 *          image. The fields documented here describe the host-visible
 *          mailbox; the firmware-load path is stubbed in the driver and
 *          must be wired to the production loader before the controller
 *          will respond to any HCI command.
 *
 * Layout summary (offsets relative to the block base):
 *
 *  - 0x000 : CTRL    -- enable / reset / patch-RAM-load select
 *  - 0x004 : STATUS  -- ready / cmd-busy / event-pending flags
 *  - 0x008 : INTEN   -- interrupt enable for tx-done / rx-ready
 *  - 0x00C : INTSTAT -- write-1-to-clear interrupt status
 *  - 0x010 : OSCCTL  -- 32 MHz radio oscillator trim/enable
 *  - 0x014 : PATCHADDR -- patch-RAM destination word address
 *  - 0x018 : PATCHDATA -- patch-RAM data port (auto-increments addr)
 *  - 0x020 : HCI_CMD_FIFO  -- HCI command write port (host -> ctrl)
 *  - 0x024 : HCI_ACL_FIFO  -- HCI ACL data write port (host -> ctrl)
 *  - 0x028 : HCI_EVT_FIFO  -- HCI event read port  (ctrl -> host)
 *  - 0x02C : HCI_RX_FIFO   -- HCI ACL data read port (ctrl -> host)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/* =============================================================================
 * Base address (placeholder)
 * =============================================================================
 *
 * @todo Cite real RA8D2 BLE block base once the datasheet addendum is
 *       published. The placeholder address sits inside the peripheral
 *       window already mapped by the host-test mmap helper.
 */

typedef enum : uintptr_t {
  k_ra_ble_base_addr = 0x40700000UL, /**< Placeholder; FSP r_ble loads patch RAM via vendor RPC. */
} ra_ble_addr_t;

/* =============================================================================
 * Register block
 * =============================================================================
 */

/**
 * @struct r_ble_t
 * @brief Memory-mapped layout of the RA8D2 BLE controller mailbox.
 *
 * @invariant All offsets are word-aligned.
 */
typedef struct {
  volatile uint32_t CTRL;       /**< 0x000 Enable / reset / patch-load select. */
  volatile uint32_t STATUS;     /**< 0x004 Read-only status word.              */
  volatile uint32_t INTEN;      /**< 0x008 Interrupt-enable mask.              */
  volatile uint32_t INTSTAT;    /**< 0x00C Interrupt status (W1C).             */
  volatile uint32_t OSCCTL;     /**< 0x010 Radio oscillator control.           */
  volatile uint32_t PATCHADDR;  /**< 0x014 Patch-RAM destination word addr.    */
  volatile uint32_t PATCHDATA;  /**< 0x018 Patch-RAM data port (auto-inc).     */
  volatile uint32_t RESERVED0;  /**< 0x01C reserved.                           */
  volatile uint32_t HCI_CMD;    /**< 0x020 HCI command FIFO write port.        */
  volatile uint32_t HCI_ACL_TX; /**< 0x024 HCI ACL TX FIFO write port.         */
  volatile uint32_t HCI_EVT;    /**< 0x028 HCI event FIFO read port.           */
  volatile uint32_t HCI_ACL_RX; /**< 0x02C HCI ACL RX FIFO read port.          */
} r_ble_t;

/**
 * @brief Block byte size guarded by ``static_assert``.
 */
typedef enum : uint8_t {
  k_ra_ble_block_bytes = 48U, /**< 12 x uint32_t = 48 bytes. */
} ra_ble_block_size_t;

static_assert(sizeof(r_ble_t) == (size_t)k_ra_ble_block_bytes,
              "r_ble_t layout drifted from RA8D2 BLE mailbox map");

/**
 * @brief Pointer to the BLE controller mailbox.
 *
 * @return volatile r_ble_t* Mailbox pointer.
 * @since 0.1.0
 */
static inline volatile r_ble_t* ra_ble_regs(void)
{
  return (volatile r_ble_t*)k_ra_ble_base_addr;
}

/* =============================================================================
 * CTRL bit masks
 * =============================================================================
 */

typedef enum : uint32_t {
  k_ra_ble_ctrl_enable     = 0x00000001UL, /**< 1 = controller out of reset.     */
  k_ra_ble_ctrl_reset      = 0x00000002UL, /**< 1 = self-clearing soft reset.    */
  k_ra_ble_ctrl_patch_load = 0x00000004UL, /**< 1 = patch-RAM write window open. */
  k_ra_ble_ctrl_hci_enable = 0x00000008UL, /**< 1 = HCI mailbox accepts traffic. */
} ra_ble_ctrl_bits_t;

/* =============================================================================
 * STATUS bit masks
 * =============================================================================
 */

typedef enum : uint32_t {
  k_ra_ble_status_ready    = 0x00000001UL, /**< Controller ready for HCI.          */
  k_ra_ble_status_cmd_busy = 0x00000002UL, /**< Last HCI command still in flight.  */
  k_ra_ble_status_evt_pend = 0x00000004UL, /**< HCI event waiting in HCI_EVT FIFO. */
  k_ra_ble_status_acl_pend = 0x00000008UL, /**< HCI ACL data waiting in RX FIFO.   */
} ra_ble_status_bits_t;

/* =============================================================================
 * HCI packet indicator bytes -- Bluetooth Core 5.3 Vol 4 Part A 2 "HCI"
 * =============================================================================
 */

typedef enum : uint8_t {
  k_ra_ble_pkt_cmd      = 0x01U, /**< HCI command packet indicator.  */
  k_ra_ble_pkt_acl_data = 0x02U, /**< HCI ACL data packet indicator. */
  k_ra_ble_pkt_sco_data = 0x03U, /**< HCI SCO data packet indicator. */
  k_ra_ble_pkt_event    = 0x04U, /**< HCI event packet indicator.    */
  k_ra_ble_pkt_iso_data = 0x05U, /**< HCI ISO data packet indicator. */
} ra_ble_pkt_type_t;

/* =============================================================================
 * Selected HCI opcodes -- Bluetooth Core 5.3 Vol 4 Part E 7.8 "LE Controller
 * Commands". OGF=0x08 (LE), OCF in low 10 bits, opcode = (OGF << 10) | OCF.
 * =============================================================================
 */

typedef enum : uint16_t {
  k_ra_ble_op_le_set_random_address = 0x2005U, /**< 7.8.4  LE_Set_Random_Address.         */
  k_ra_ble_op_le_set_adv_params     = 0x2006U, /**< 7.8.5  LE_Set_Advertising_Parameters. */
  k_ra_ble_op_le_set_adv_data       = 0x2008U, /**< 7.8.7  LE_Set_Advertising_Data.       */
  k_ra_ble_op_le_set_adv_enable     = 0x200AU, /**< 7.8.9  LE_Set_Advertising_Enable.     */
  k_ra_ble_op_le_set_scan_params    = 0x200BU, /**< 7.8.10 LE_Set_Scan_Parameters.        */
  k_ra_ble_op_le_set_scan_enable    = 0x200CU, /**< 7.8.11 LE_Set_Scan_Enable.            */
  k_ra_ble_op_le_create_connection  = 0x200DU, /**< 7.8.12 LE_Create_Connection.          */
} ra_ble_opcode_t;

/* =============================================================================
 * Selected HCI event codes -- Bluetooth Core 5.3 Vol 4 Part E 7.7 "Events".
 * =============================================================================
 */

typedef enum : uint8_t {
  k_ra_ble_evt_cmd_complete  = 0x0EU, /**< 7.7.14 Command Complete.       */
  k_ra_ble_evt_cmd_status    = 0x0FU, /**< 7.7.15 Command Status.         */
  k_ra_ble_evt_le_meta       = 0x3EU, /**< 7.7.65 LE Meta event.          */
  k_ra_ble_evt_disconn_compl = 0x05U, /**< 7.7.5  Disconnection Complete. */
} ra_ble_event_code_t;

typedef enum : uint8_t {
  k_ra_ble_le_subevt_conn_complete = 0x01U, /**< 7.7.65.1 LE_Connection_Complete. */
  k_ra_ble_le_subevt_adv_report    = 0x02U, /**< 7.7.65.2 LE_Advertising_Report.  */
} ra_ble_le_subevt_t;

#ifdef __cplusplus
}
#endif
