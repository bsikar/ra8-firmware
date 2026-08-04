/**
 * @file board_periph_prcr_internal.h
 * @brief Module-private PRCR unlock state shared with the protected blocks
 *
 * @details
 * The RA8D2 gates writes to whole families of registers behind the SYSC
 * Protect Register (PRCR, @c 0x4001_E3FA) -- HUM Ch 13.1 Table 13.1
 * "Association between PRCR bits and use of registers to be protected"
 * p 520-521. A write issued to a protected register while its group bit is
 * 0 is discarded by the hardware with no fault and no status flag, so a
 * driver that forgets the unlock appears to work and changes nothing.
 *
 * @c board_periph_prcr.c snoops PRCR and keeps the live group mask; the
 * blocks that model protected registers ask ::board_prcr_group_unlocked
 * before accepting a write, so the emulator drops exactly the writes the
 * silicon drops. This header is the seam between them.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_attributes.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief PRCR group bits (HUM Ch 13.2.1 "PRCR_S" p 522).
 *
 * @details
 * Bit 2 and bits 7:6 are reserved (read as 0). Bits 15:8 carry the
 * mandatory 0xA5 write key and are not part of the retained mask.
 *
 * @invariant Each value sets exactly one bit.
 * @see board_prcr_group_unlocked
 */
typedef enum : uint16_t {
  k_board_prcr_grp0_cgc = 0x0001U, /**< PRC0: clock generation circuit.       */
  k_board_prcr_grp1_lpm = 0x0002U, /**< PRC1: low-power modes + VBATT backup. */
  k_board_prcr_grp3_pvd = 0x0008U, /**< PRC3: PVD + VBATTMNSELR.              */
  k_board_prcr_grp4_sar = 0x0010U, /**< PRC4: security/privilege attribution. */
  k_board_prcr_grp5_rst = 0x0020U, /**< PRC5: reset control.                  */
} board_prcr_group_t;

/**
 * @brief Report whether every bit in @p group_mask is currently unlocked.
 *
 * @details
 * Reflects the last PRCR write that carried the correct 0xA5 key. A write
 * with a wrong key is ignored by the hardware and by this model, so the
 * mask keeps its previous value.
 *
 * @param[in] group_mask One or more ``k_board_prcr_grp*`` bits.
 *
 * @return @c true when all requested groups are write-enabled.
 * @retval true  Every bit in @p group_mask is set in the live PRCR mask.
 * @retval false At least one requested group is still locked.
 *
 * @pre @p group_mask names only bits defined by ::board_prcr_group_t.
 * @pre The PRCR block is registered (host constructor, before main).
 * @post No state is mutated.
 *
 * @note Not thread-safe; ra8_emulator drives all blocks from one thread.
 * @since 0.1.0
 */
RA8_PRIV bool board_prcr_group_unlocked(uint16_t group_mask);

#ifdef __cplusplus
}
#endif
