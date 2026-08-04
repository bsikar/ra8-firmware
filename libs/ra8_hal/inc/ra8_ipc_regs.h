/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_ipc_regs.h
 * @brief Inter-Processor Communication (IPC) register layout for the RA8D2
 * @ingroup grp_hal_system
 *
 * @details
 * The RA8D2 hosts two CPU cores (Cortex-M85 / "CPU0" and Cortex-M33 /
 * "CPU1") connected by a single IPC peripheral block. The block is
 * partitioned into two units:
 *
 *   - **IPC0** -- traffic from CPU1 (M33) to CPU0 (M85), with one
 *     non-maskable interrupt path (IPC0NMI*) plus two independent
 *     32-bit message FIFOs (channels IPC0_0 and IPC0_1). Each FIFO
 *     channel carries 8 maskable IRQ event lines, an RDY/FULL status
 *     pair, and FIFO read/write error flags.
 *   - **IPC1** -- mirror image, traffic from CPU0 (M85) to CPU1 (M33),
 *     with IPC1NMI*, IPC1_0 and IPC1_1.
 *
 * In addition the block exposes 16 hardware semaphores (IPCSEM0..15)
 * shared between the cores and a security/privilege attribution pair
 * (IPCSAR / IPCPAR) hosted in the CPSCU register window.
 *
 * Per-FIFO-channel registers (5 x 32-bit at +0x20 stride):
 *
 * | Offset | Name        | Width | Purpose                              |
 * |-------:|-------------|------:|--------------------------------------|
 * |  0x00  | STA         | 32    | Status (IRQ/RDY/FULL/RERR/FERR)      |
 * |  0x04  | ISET        | 32    | IRQ request set (write-1-to-trigger) |
 * |  0x08  | TXD         | 32    | FIFO transmit data (write-only)      |
 * |  0x0C  | RXD         | 32    | FIFO receive data (read-only)        |
 * |  0x10  | CLR         | 32    | Status / error / FIFO clear          |
 *
 * Per-NMI-channel registers (3 x 32-bit at +0x10 stride):
 *
 * | Offset | Name      | Width | Purpose                                   |
 * |-------:|-----------|------:|-------------------------------------------|
 * |  0x00  | NMISTA    | 32    | Non-maskable interrupt status (read-only) |
 * |  0x04  | NMISET    | 32    | Write 1 to NMI bit -> raise NMI to peer   |
 * |  0x08  | NMICLR    | 32    | Write 1 to CLR bit -> clear NMISTA.NMI    |
 *
 * Register offsets tracked against HUM Ch 3 "Inter-Processor
 * Communication (IPC)" p 204-233.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"

/**
 * @enum ra8_ipc_addr_t
 * @brief Memory-mapped base addresses for the IPC block and the
 *        CPSCU window that holds IPCSAR / IPCPAR.
 *
 * @details
 * Secure / non-secure aliases differ by bit 28 (0x40000000 vs
 * 0x50000000). HAL code uses the secure alias since unit tests run
 * with `RA8_OFF_TARGET` and the fake mmap covers the secure
 * 0x4xxxxxxx peripheral window.
 */
typedef enum : uintptr_t {
  k_ra8_ipc_base_addr   = 0x40020000UL, /**< IPC block base (Secure).        */
  k_ra8_ipc_cpscu_addr  = 0x40008000UL, /**< CPSCU base for IPCSAR/IPCPAR.   */
  k_ra8_ipc_ipcsar_addr = 0x40008610UL, /**< IPC Security Attribution Reg.   */
  k_ra8_ipc_ipcpar_addr = 0x40008614UL, /**< IPC Privileged Attribution Reg. */
} ra8_ipc_addr_t;

/**
 * @enum ra8_ipc_offset_t
 * @brief In-block offsets relative to ``k_ra8_ipc_base_addr``.
 *
 * @details
 * Verified against the IPC register address map figure on HUM
 * Ch 3.2 "Register Descriptions" p 205. IPCSEM0..15 occupy 0x000..
 * 0x03C; the two NMI register triplets sit at 0x080 (IPC0) and
 * 0x090 (IPC1); the four FIFO channel windows start at 0x0C0
 * (IPC0_0), 0x0E0 (IPC0_1), 0x100 (IPC1_0), 0x120 (IPC1_1).
 */
typedef enum : uint16_t {
  k_ra8_ipc_offset_ipcsem0    = 0x000U, /**< First semaphore register.       */
  k_ra8_ipc_offset_ipcsem15   = 0x03CU, /**< Last semaphore register.        */
  k_ra8_ipc_offset_ipc0nmista = 0x080U, /**< IPC0 NMI status (CPU1 -> CPU0). */
  k_ra8_ipc_offset_ipc0nmiset = 0x084U, /**< IPC0 NMI set.                   */
  k_ra8_ipc_offset_ipc0nmiclr = 0x088U, /**< IPC0 NMI clear.                 */
  k_ra8_ipc_offset_ipc1nmista = 0x090U, /**< IPC1 NMI status (CPU0 -> CPU1). */
  k_ra8_ipc_offset_ipc1nmiset = 0x094U, /**< IPC1 NMI set.                   */
  k_ra8_ipc_offset_ipc1nmiclr = 0x098U, /**< IPC1 NMI clear.                 */
  k_ra8_ipc_offset_ipc0_ch0   = 0x0C0U, /**< IPC0 channel 0 STA (FIFO00).    */
  k_ra8_ipc_offset_ipc0_ch1   = 0x0E0U, /**< IPC0 channel 1 STA (FIFO01).    */
  k_ra8_ipc_offset_ipc1_ch0   = 0x100U, /**< IPC1 channel 0 STA (FIFO10).    */
  k_ra8_ipc_offset_ipc1_ch1   = 0x120U, /**< IPC1 channel 1 STA (FIFO11).    */
} ra8_ipc_offset_t;

/**
 * @enum ra8_ipc_limits_t
 * @brief Counts and strides used by the driver / accessors.
 *
 * @details
 * HUM Ch 3.1 "Overview" p 204 fixes the FIFO depth at 4 stages and
 * the per-channel maskable-IRQ-event count at 8. The stride values
 * are derived from the register-address map on p 205.
 */
typedef enum : uint8_t {
  k_ra8_ipc_sem_count       = 16U,   /**< IPCSEM0..IPCSEM15.                   */
  k_ra8_ipc_nmi_unit_count  = 2U,    /**< IPC0 + IPC1 NMI units.               */
  k_ra8_ipc_channel_count   = 4U,    /**< Total FIFO channels: 2 per unit x 2. */
  k_ra8_ipc_irq_event_count = 8U,    /**< 8 IRQ event lines per channel.       */
  k_ra8_ipc_fifo_depth      = 4U,    /**< 4 stages per FIFO (HUM 3.1).         */
  k_ra8_ipc_channel_stride  = 0x20U, /**< Bytes between channel windows.       */
  k_ra8_ipc_nmi_stride      = 0x10U, /**< Bytes between NMI unit windows.      */
} ra8_ipc_limits_t;

/**
 * @enum ra8_ipc_unit_id_t
 * @brief Logical IPC unit identifiers (IPC0 vs IPC1).
 *
 * @details
 * The IPC block is split into two mirror-image units (HUM Ch 3.1
 * "Overview" p 204): IPC0 carries CPU1 -> CPU0 traffic (M33 to M85
 * direction), IPC1 carries CPU0 -> CPU1 (M85 to M33). Channel id
 * 0/1 maps to IPC0_0/IPC0_1 (unit 0); channel id 2/3 maps to
 * IPC1_0/IPC1_1 (unit 1).
 */
typedef enum : uint8_t {
  k_ra8_ipc_unit_ipc0 = 0U, /**< CPU1 -> CPU0 (M33 sends, M85 receives). */
  k_ra8_ipc_unit_ipc1 = 1U, /**< CPU0 -> CPU1 (M85 sends, M33 receives). */
} ra8_ipc_unit_id_t;

/**
 * @enum ra8_ipc_core_id_t
 * @brief Logical CPU core identifiers used by attribution helpers.
 *
 * @details
 * Matches the FSP convention (`BSP_CFG_CPU_CORE` 0 = Cortex-M85
 * / CPU0, 1 = Cortex-M33 / CPU1). Used by the channel-pair
 * convention helper to decide which IPC unit a given core writes
 * to and which it reads from.
 */
typedef enum : uint8_t {
  k_ra8_ipc_core_cpu0 = 0U, /**< Cortex-M85 (primary core).   */
  k_ra8_ipc_core_cpu1 = 1U, /**< Cortex-M33 (secondary core). */
} ra8_ipc_core_id_t;

/**
 * @enum ra8_ipc_sta_mask_t
 * @brief Bit masks for the IPCnSTAm "Status" registers.
 *
 * @details
 * Verified against HUM Ch 3.2.10 "IPC0STA0" p 214 (and the identical
 * IPC0STA1 / IPC1STA0 / IPC1STA1 on pages 217, 220, 223). Bits [7:0]
 * are the eight maskable IRQ event status bits; [16] RDY indicates
 * the receive FIFO is non-empty; [17] FULL indicates the FIFO is full;
 * [24] RERR signals a read from an empty FIFO; [25] FERR signals a
 * write to a full FIFO.
 */
typedef enum : uint32_t {
  k_ra8_ipc_sta_mask_irq0    = 0x00000001U, /**< IRQ0 event.                    */
  k_ra8_ipc_sta_mask_irq1    = 0x00000002U, /**< IRQ1 event.                    */
  k_ra8_ipc_sta_mask_irq2    = 0x00000004U, /**< IRQ2 event.                    */
  k_ra8_ipc_sta_mask_irq3    = 0x00000008U, /**< IRQ3 event.                    */
  k_ra8_ipc_sta_mask_irq4    = 0x00000010U, /**< IRQ4 event.                    */
  k_ra8_ipc_sta_mask_irq5    = 0x00000020U, /**< IRQ5 event.                    */
  k_ra8_ipc_sta_mask_irq6    = 0x00000040U, /**< IRQ6 event.                    */
  k_ra8_ipc_sta_mask_irq7    = 0x00000080U, /**< IRQ7 event.                    */
  k_ra8_ipc_sta_mask_irq_all = 0x000000FFU, /**< IRQ7..IRQ0 event bits.         */
  k_ra8_ipc_sta_mask_rdy     = 0x00010000U, /**< RDY: FIFO non-empty.           */
  k_ra8_ipc_sta_mask_full    = 0x00020000U, /**< FULL: FIFO full.               */
  k_ra8_ipc_sta_mask_rerr    = 0x01000000U, /**< RERR: read-while-empty.        */
  k_ra8_ipc_sta_mask_ferr    = 0x02000000U, /**< FERR: write-while-full.        */
  k_ra8_ipc_sta_mask_event   = 0x030000FFU, /**< Any event the ISR cares about. */
  k_ra8_ipc_sta_mask_err_any = 0x03000000U, /**< RERR | FERR.                   */
} ra8_ipc_sta_mask_t;

/**
 * @enum ra8_ipc_clr_mask_t
 * @brief Bit masks for the IPCnCLRm "Clear" registers.
 *
 * @details
 * Verified against HUM Ch 3.2.14 "IPC0CLR0" p 216-217. CLR7..CLR0
 * in [7:0] cancel the corresponding IRQn status; bit [16] RST
 * resets the message FIFO (also clears RDY + FULL); [24] RCLR
 * clears RERR; [25] FCLR clears FERR.
 */
typedef enum : uint32_t {
  k_ra8_ipc_clr_mask_clr0    = 0x00000001U, /**< CLR0 -> drop STA.IRQ0.    */
  k_ra8_ipc_clr_mask_clr1    = 0x00000002U, /**< CLR1 -> drop STA.IRQ1.    */
  k_ra8_ipc_clr_mask_clr2    = 0x00000004U, /**< CLR2 -> drop STA.IRQ2.    */
  k_ra8_ipc_clr_mask_clr3    = 0x00000008U, /**< CLR3 -> drop STA.IRQ3.    */
  k_ra8_ipc_clr_mask_clr4    = 0x00000010U, /**< CLR4 -> drop STA.IRQ4.    */
  k_ra8_ipc_clr_mask_clr5    = 0x00000020U, /**< CLR5 -> drop STA.IRQ5.    */
  k_ra8_ipc_clr_mask_clr6    = 0x00000040U, /**< CLR6 -> drop STA.IRQ6.    */
  k_ra8_ipc_clr_mask_clr7    = 0x00000080U, /**< CLR7 -> drop STA.IRQ7.    */
  k_ra8_ipc_clr_mask_irq_all = 0x000000FFU, /**< CLR7..CLR0 -> clear IRQn. */
  k_ra8_ipc_clr_mask_rst     = 0x00010000U, /**< RST: reset message FIFO.  */
  k_ra8_ipc_clr_mask_rclr    = 0x01000000U, /**< RCLR: clear RERR.         */
  k_ra8_ipc_clr_mask_fclr    = 0x02000000U, /**< FCLR: clear FERR.         */
  k_ra8_ipc_clr_mask_all     = 0x030100FFU, /**< Every clearable bit.      */
} ra8_ipc_clr_mask_t;

/**
 * @enum ra8_ipc_iset_mask_t
 * @brief Bit masks for the IPCnISETm "IRQ Request Set" registers.
 *
 * @details
 * Verified against HUM Ch 3.2.11 "IPC0ISET0" p 215. SET7..SET0 in
 * [7:0] each raise the corresponding IRQn status bit which in turn
 * triggers the maskable IPCnIRQm interrupt on the receiving core.
 */
typedef enum : uint32_t {
  k_ra8_ipc_iset_mask_set0 = 0x00000001U, /**< SET0 -> IRQ0 request. */
  k_ra8_ipc_iset_mask_set1 = 0x00000002U, /**< SET1 -> IRQ1 request. */
  k_ra8_ipc_iset_mask_set2 = 0x00000004U, /**< SET2 -> IRQ2 request. */
  k_ra8_ipc_iset_mask_set3 = 0x00000008U, /**< SET3 -> IRQ3 request. */
  k_ra8_ipc_iset_mask_set4 = 0x00000010U, /**< SET4 -> IRQ4 request. */
  k_ra8_ipc_iset_mask_set5 = 0x00000020U, /**< SET5 -> IRQ5 request. */
  k_ra8_ipc_iset_mask_set6 = 0x00000040U, /**< SET6 -> IRQ6 request. */
  k_ra8_ipc_iset_mask_set7 = 0x00000080U, /**< SET7 -> IRQ7 request. */
  k_ra8_ipc_iset_mask_all  = 0x000000FFU, /**< All eight SET bits.   */
} ra8_ipc_iset_mask_t;

/**
 * @enum ra8_ipc_nmi_mask_t
 * @brief Bit masks for the IPC NMI status / set / clear registers.
 *
 * @details
 * HUM Ch 3.2.4 "IPC0NMISTA" p 210 + Ch 3.2.5 "IPC0NMISET" p 211 +
 * Ch 3.2.6 "IPC0NMICLR" p 212 (mirrored on Ch 3.2.7..3.2.9 p 212-
 * 213) document bit [0] as the only valid bit in NMISTA / NMISET /
 * NMICLR.
 */
typedef enum : uint32_t {
  k_ra8_ipc_nmi_mask_bit = 0x00000001U, /**< Single-bit NMI request / status. */
} ra8_ipc_nmi_mask_t;

/**
 * @enum ra8_ipc_sem_mask_t
 * @brief Bit masks for the IPCSEMn semaphore register.
 *
 * @details
 * HUM Ch 3.2.3 "IPCSEMn" p 210 documents bit [0] LOCK as the only
 * valid bit. Reading the register through a 32-bit access *sets*
 * LOCK (test-and-set semantics): the read returns 0 if the
 * acquisition succeeded, 1 if another core had already taken it.
 * Writing 1 to LOCK releases the resource. Halfword and byte reads
 * that do not include bit 0 do not trigger the set, which is why
 * this driver only ever issues 32-bit accessors.
 */
typedef enum : uint32_t {
  k_ra8_ipc_sem_mask_lock = 0x00000001U, /**< LOCK bit (read-to-acquire). */
} ra8_ipc_sem_mask_t;

/**
 * @enum ra8_ipcsar_mask_t
 * @brief Bit masks for the IPCSAR Security Attribution Register.
 *
 * @details
 * Verified against HUM Ch 3.2.1 "IPCSAR" p 205-207. Bit 0/1 control
 * the security of IPCSEM[0..7] and IPCSEM[8..15]; bits 8/9 control
 * the NMI register pair for IPC0/IPC1; bits 16..19 control the four
 * IPC channel register groups.
 */
typedef enum : uint32_t {
  k_ra8_ipcsar_mask_saipcsem0 = 0x00000001U, /**< IPCSEM0..7 secure attr.     */
  k_ra8_ipcsar_mask_saipcsem1 = 0x00000002U, /**< IPCSEM8..15 secure attr.    */
  k_ra8_ipcsar_mask_saipcnmi0 = 0x00000100U, /**< IPC0NMI* secure attr.       */
  k_ra8_ipcsar_mask_saipcnmi1 = 0x00000200U, /**< IPC1NMI* secure attr.       */
  k_ra8_ipcsar_mask_saipcir0  = 0x00010000U, /**< IPC0 channel 0 secure attr. */
  k_ra8_ipcsar_mask_saipcir1  = 0x00020000U, /**< IPC0 channel 1 secure attr. */
  k_ra8_ipcsar_mask_saipcir2  = 0x00040000U, /**< IPC1 channel 0 secure attr. */
  k_ra8_ipcsar_mask_saipcir3  = 0x00080000U, /**< IPC1 channel 1 secure attr. */
} ra8_ipcsar_mask_t;

/**
 * @enum ra8_ipcpar_mask_t
 * @brief Bit masks for the IPCPAR Privileged Attribution Register.
 *
 * @details
 * HUM Ch 3.2.2 "IPCPAR" p 208-209. Each bit selects "Privileged" (0)
 * vs "Unprivileged" (1) access to the matching IPCSAR target group.
 */
typedef enum : uint32_t {
  k_ra8_ipcpar_mask_paipcsem0 = 0x00000001U, /**< IPCSEM0..7 priv attr.     */
  k_ra8_ipcpar_mask_paipcsem1 = 0x00000002U, /**< IPCSEM8..15 priv attr.    */
  k_ra8_ipcpar_mask_paipcnmi0 = 0x00000100U, /**< IPC0NMI* priv attr.       */
  k_ra8_ipcpar_mask_paipcnmi1 = 0x00000200U, /**< IPC1NMI* priv attr.       */
  k_ra8_ipcpar_mask_paipcir0  = 0x00010000U, /**< IPC0 channel 0 priv attr. */
  k_ra8_ipcpar_mask_paipcir1  = 0x00020000U, /**< IPC0 channel 1 priv attr. */
  k_ra8_ipcpar_mask_paipcir2  = 0x00040000U, /**< IPC1 channel 0 priv attr. */
  k_ra8_ipcpar_mask_paipcir3  = 0x00080000U, /**< IPC1 channel 1 priv attr. */
} ra8_ipcpar_mask_t;

/**
 * @enum ra8_ipc_attr_shift_t
 * @brief Bit positions of SAIPCIRn / PAIPCIRn within IPCSAR / IPCPAR.
 *
 * @details
 * HUM Ch 3.2.1 "IPCSAR" p 205 places SAIPCIR0 at bit 16, SAIPCIR1 at
 * bit 17, SAIPCIR2 at 18 and SAIPCIR3 at 19. IPCPAR mirrors these
 * positions (HUM Ch 3.2.2 p 208) so the same shift works for both.
 */
typedef enum : uint8_t {
  k_ra8_ipc_attr_shift_ir_base  = 16U, /**< SAIPCIR0 / PAIPCIR0 bit position. */
  k_ra8_ipc_attr_shift_nmi_base = 8U,  /**< SAIPCNMI0 / PAIPCNMI0 position.   */
} ra8_ipc_attr_shift_t;

/**
 * @enum ra8_ipc_elc_event_t
 * @brief ELC event numbers for the IPC maskable IRQs.
 *
 * @details
 * The RA8D2 collapses every IPCnIRQm line into the two ELC events
 * documented in HUM Ch 18 "Event Link Controller (ELC)" event-list
 * (verified against ``fsp/ra/fsp/src/bsp/mcu/ra8d2/bsp_elc.h``):
 * ELC_EVENT_IPC_IRQ0 = 0x05B carries IPC unit 0 channel 0 + 1, and
 * ELC_EVENT_IPC_IRQ1 = 0x05C carries IPC unit 1. The driver casts
 * these to ``ra8_elc_event_t`` when calling ``ra8_isr_register``.
 */
typedef enum : uint16_t {
  k_ra8_ipc_elc_event_irq0 = 0x05BU, /**< Receiving-side IRQ for IPC0_*. */
  k_ra8_ipc_elc_event_irq1 = 0x05CU, /**< Receiving-side IRQ for IPC1_*. */
} ra8_ipc_elc_event_t;

/**
 * @enum ra8_ipc_sem_attr_group_t
 * @brief IPCSEMn attribution-group selector (HUM Ch 3.3.1 p 227-228).
 *
 * @details
 * The semaphores are attributed in two groups of eight: IPCSEM0..7
 * share SAIPCSEM0/PAIPCSEM0 and IPCSEM8..15 share SAIPCSEM1/PAIPCSEM1
 * (HUM Ch 3.3.1 "Semaphore" p 228 Figure 3.2). The driver uses this
 * enum so the security/privilege accessors can address either group
 * without callers tracking which IPCSEM index lives where.
 */
typedef enum : uint8_t {
  k_ra8_ipc_sem_group_low  = 0U, /**< IPCSEM0..IPCSEM7  attribution group. */
  k_ra8_ipc_sem_group_high = 1U, /**< IPCSEM8..IPCSEM15 attribution group. */
} ra8_ipc_sem_attr_group_t;

/**
 * @struct r_ipc_channel_regs_t
 * @brief Per-FIFO-channel register window (5 x 32-bit at +0x20 stride).
 *
 * @details
 * Layout matches the IPC0_0 (IPC0STA0..IPC0CLR0) block at offset
 * 0x0C0 documented in HUM Ch 3.2.10..3.2.14 p 214-216, and is reused
 * with offset 0x20/0x40/0x60 for IPC0_1, IPC1_0, IPC1_1 (see the
 * IPC register address map figure on HUM p 205).
 */
typedef struct {
  volatile uint32_t STA;     /**< +0x00 Status (RDY/FULL/RERR/FERR/IRQn). */
  volatile uint32_t ISET;    /**< +0x04 IRQ request set (W).              */
  volatile uint32_t TXD;     /**< +0x08 FIFO transmit data (W).           */
  volatile uint32_t RXD;     /**< +0x0C FIFO receive data  (R).           */
  volatile uint32_t CLR;     /**< +0x10 Status / error / FIFO clear (W).  */
  volatile uint32_t _pad[3]; /**< +0x14..+0x1F padding to next channel.   */
} r_ipc_channel_regs_t;

/**
 * @struct r_ipc_nmi_regs_t
 * @brief Per-NMI-unit register window (3 x 32-bit at +0x10 stride).
 *
 * @details
 * Layout matches IPC0NMISTA..IPC0NMICLR at +0x080 documented in HUM
 * Ch 3.2.4..3.2.6 / 3.2.7..3.2.9 p 210-213, mirrored at +0x090 for
 * IPC1.
 */
typedef struct {
  volatile uint32_t NMISTA; /**< +0x00 NMI status (R).       */
  volatile uint32_t NMISET; /**< +0x04 NMI set (W).          */
  volatile uint32_t NMICLR; /**< +0x08 NMI clear (W).        */
  volatile uint32_t _pad;   /**< +0x0C padding to next unit. */
} r_ipc_nmi_regs_t;

/**
 * @brief Get a pointer to one IPC FIFO channel register window.
 *
 * @param[in] channel Logical channel id 0..3:
 *   - 0 = IPC0 channel 0 (CPU1 -> CPU0, FIFO00) at +0x0C0
 *   - 1 = IPC0 channel 1 (CPU1 -> CPU0, FIFO01) at +0x0E0
 *   - 2 = IPC1 channel 0 (CPU0 -> CPU1, FIFO10) at +0x100
 *   - 3 = IPC1 channel 1 (CPU0 -> CPU1, FIFO11) at +0x120
 * @return Volatile pointer to the channel block, or `nullptr` when
 *         `channel >= k_ra8_ipc_channel_count`.
 */
RA8_HW_REGISTER_ACCESS
static inline volatile r_ipc_channel_regs_t* ra8_ipc_channel(uint8_t channel)
{
  if ((uint16_t)channel >= (uint16_t)k_ra8_ipc_channel_count) {
    return nullptr;
  }
  /* HUM Ch 3.2 "Register Descriptions" p 205 -- channel 0 starts at
   * +0x0C0 with a 0x20 stride between consecutive channels. */
  const uintptr_t base = k_ra8_ipc_base_addr + (uintptr_t)k_ra8_ipc_offset_ipc0_ch0 +
                         ((uintptr_t)channel * (uintptr_t)k_ra8_ipc_channel_stride);
  return (volatile r_ipc_channel_regs_t*)base;
}

/**
 * @brief Get a pointer to one IPC NMI unit register window.
 *
 * @param[in] unit NMI unit id 0..1:
 *   - 0 = IPC0 NMI (CPU1 -> CPU0) at +0x080
 *   - 1 = IPC1 NMI (CPU0 -> CPU1) at +0x090
 * @return Volatile pointer to the NMI block, or `nullptr` when
 *         `unit >= k_ra8_ipc_nmi_unit_count`.
 */
RA8_HW_REGISTER_ACCESS
static inline volatile r_ipc_nmi_regs_t* ra8_ipc_nmi(uint8_t unit)
{
  if ((uint16_t)unit >= (uint16_t)k_ra8_ipc_nmi_unit_count) {
    return nullptr;
  }
  /* HUM Ch 3.2 "Register Descriptions" p 205 -- IPC0NMI at +0x080,
   * IPC1NMI at +0x090 (0x10 stride). */
  const uintptr_t base = k_ra8_ipc_base_addr + (uintptr_t)k_ra8_ipc_offset_ipc0nmista +
                         ((uintptr_t)unit * (uintptr_t)k_ra8_ipc_nmi_stride);
  return (volatile r_ipc_nmi_regs_t*)base;
}

/**
 * @brief Get a pointer to the IPCSAR Security Attribution register.
 * @return Volatile pointer to the 32-bit IPCSAR.
 */
RA8_HW_REGISTER_ACCESS
static inline volatile uint32_t* ra8_ipc_ipcsar(void)
{
  /* HUM Ch 3.2.1 "IPCSAR" p 205 */
  return (volatile uint32_t*)k_ra8_ipc_ipcsar_addr;
}

/**
 * @brief Get a pointer to the IPCPAR Privileged Attribution register.
 * @return Volatile pointer to the 32-bit IPCPAR.
 */
RA8_HW_REGISTER_ACCESS
static inline volatile uint32_t* ra8_ipc_ipcpar(void)
{
  /* HUM Ch 3.2.2 "IPCPAR" p 208 */
  return (volatile uint32_t*)k_ra8_ipc_ipcpar_addr;
}

/**
 * @brief Get a pointer to one IPCSEM hardware semaphore register.
 *
 * @param[in] sem_id Semaphore index 0..15.
 * @return Volatile pointer to the semaphore register, or `nullptr`
 *         when `sem_id >= k_ra8_ipc_sem_count`.
 */
RA8_HW_REGISTER_ACCESS
static inline volatile uint32_t* ra8_ipc_sem(uint8_t sem_id)
{
  if ((uint16_t)sem_id >= (uint16_t)k_ra8_ipc_sem_count) {
    return nullptr;
  }
  /* HUM Ch 3.2.3 "IPCSEMn" p 210 -- IPCSEM0 at +0x000, each
   * semaphore is a 32-bit word so the stride is 4 bytes. */
  const uintptr_t base = k_ra8_ipc_base_addr + ((uintptr_t)sem_id * sizeof(uint32_t));
  return (volatile uint32_t*)base;
}

/**
 * @brief Map a logical channel id to its containing IPC unit.
 *
 * @param[in] channel Channel id 0..3.
 * @return Unit id (0 for channels 0/1, 1 for channels 2/3), or
 *         ``k_ra8_ipc_nmi_unit_count`` when ``channel`` is out of
 *         range (used as a sentinel by callers).
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static inline uint8_t ra8_ipc_channel_to_unit(uint8_t channel)
{
  if ((uint16_t)channel >= (uint16_t)k_ra8_ipc_channel_count) {
    return k_ra8_ipc_nmi_unit_count;
  }
  /* HUM Ch 3.1 "Overview" p 204 -- channels 0/1 belong to IPC0,
   * channels 2/3 belong to IPC1. */
  return (uint8_t)(channel >> 1U);
}

#ifdef __cplusplus
}
#endif
