/**
 * @file ra8_hal_internal.h
 * @brief Cross-translation-unit private helpers shared inside ra8_hal.
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Private (library-internal) header used to share small inline helpers and
 * extern declarations between sibling translation units of a single ra8_hal
 * driver that has been split across multiple ``.c`` files purely to satisfy
 * the per-file size cap. It is NOT part of the public HAL surface and must
 * never be included from outside ``libs/ra8_hal/src``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_canfd_regs.h"
#include "ra8_dotf.h"
#include "ra8_err.h"
#include "ra8_rmac_regs.h"
#include "ra8_ssie_regs.h"
#include "ra8_usb_hmsc.h"

/**
 * @brief Bound-check a DOTF channel index.
 *
 * @details
 * Shared between ``ra8_dotf.c`` and ``ra8_dotf_power.c``. Defined ``static
 * inline`` in this private header so each translation unit gets its own
 * copy with no external linkage symbol, keeping the split link-clean while
 * preserving the original single-definition behavior.
 *
 * @param[in] channel Caller-provided channel value.
 * @return ``true`` if ``channel`` is in [0, k_ra8_dotf_channel_count).
 *
 * @retval true  Channel index is in range.
 * @retval false Channel index is out of range.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static inline bool ra8_dotf_internal_channel_in_range(uint8_t channel)
{
  return (uint16_t)channel < (uint16_t)k_ra8_dotf_channel_count;
}

/* =============================================================================
 * ra8_rmac: split between ra8_rmac.c (lifecycle / config / MDIO primitives /
 * status / dispatch) and ra8_rmac_mgmt.c (statistic-counter snapshot + IEEE
 * 802.3 Clause-22 PHY management). Both translation units range-check the
 * port argument before touching the RMAC register window, so the port-bounds
 * predicate is shared here as a header ``static inline`` -- each TU gets its
 * own copy with no external linkage symbol, keeping the split link-clean.
 * =============================================================================
 */

/**
 * @brief Range-check an RMAC port argument.
 *
 * @details
 * Shared between ``ra8_rmac.c`` and ``ra8_rmac_mgmt.c``. Defined ``static
 * inline`` in this private header so each translation unit gets its own copy
 * with no external linkage symbol, keeping the split link-clean while
 * preserving the original single-definition behavior.
 *
 * @param[in] port Port to validate.
 * @return true if port is one of ::ra8_rmac_port_t.
 *
 * @retval true  Port index is in range.
 * @retval false Port index is out of range.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static inline bool internal_port_ok(ra8_rmac_port_t port)
{
  return (uint8_t)port < k_ra8_rmac_port_count;
}

/* =============================================================================
 * ra8_mipi_csi: split between ra8_mipi_csi.c (lifecycle / register knobs) and
 * ra8_mipi_csi_irq.c (interrupt path + per-VC framing). The interrupt TU owns
 * every callback function-pointer / context static, so lifecycle teardown in
 * ra8_mipi_csi.c detaches them through this promoted helper rather than
 * touching the statics across the TU boundary.
 * =============================================================================
 */

/**
 * @brief Detach every MIPI CSI-2 receiver callback (module / data-lane /
 *        virtual-channel / power-management / short-packet).
 *
 * @details
 * Clears the ten callback function-pointer / context statics owned by
 * ``ra8_mipi_csi_irq.c`` back to ``nullptr``. Invoked by
 * ::ra8_mipi_csi_deinit during teardown so that no stale callback can fire
 * after the receiver has been gated. The per-VC error callback is left
 * untouched, mirroring the pre-split ``ra8_mipi_csi_deinit`` behaviour.
 *
 * @return Nothing.
 *
 * @pre The MIPI CSI-2 receiver is being torn down by ::ra8_mipi_csi_deinit.
 * @pre Caller serialises with the CSI IRQ sources.
 * @post All ten module/lane/VC/PM/short callback slots are ``nullptr``.
 * @post No subsequent dispatch invokes a previously-registered callback.
 *
 * @note Not thread-safe; the caller must serialise concurrent access.
 * @since 0.1.0
 */
RA8_PRIV void ra8_mipi_csi_detach_all_handlers(void);

/* =============================================================================
 * ra8_ssie: split between ra8_ssie.c (lifecycle / register knobs / polled FIFO /
 * status / IRQ) and ra8_ssie_stream.c (DMA channel attach/detach + polled ISO
 * bulk send/recv). Both translation units read and write the per-channel
 * runtime bookkeeping array and call the channel-validating register accessor,
 * so the runtime state stays a single object and the accessor is promoted to
 * TU-external linkage here.
 * =============================================================================
 */

/**
 * @enum ra8_ssie_internal_const_t
 * @brief Internal magic-number constants promoted to typed enums.
 */
typedef enum : uint8_t {
  k_ra8_ssie_dma_ch_unused = 0xFFU, /**< Sentinel: no DMA channel.   */
  k_ra8_ssie_dma_max_ch    = 8U,    /**< DMAC channel count (0..7).  */
  k_ra8_ssie_thresh_max    = 0x1FU, /**< Max SSISCR threshold value. */
} ra8_ssie_internal_const_t;

/**
 * @struct ra8_ssie_runtime_t
 * @brief Per-channel runtime bookkeeping.
 */
typedef struct {
  uint8_t tx_dma_channel; /**< DMAC channel pumping TX (or unused). */
  uint8_t rx_dma_channel; /**< DMAC channel pumping RX (or unused). */
  bool    initialized;    /**< true after ra8_ssie_init succeeded.  */
  bool    dma_attached;   /**< true after ra8_ssie_attach_dma.      */
} ra8_ssie_runtime_t;

/**
 * @var s_ssie_runtime
 * @brief Per-channel SSIE runtime state, shared across the ra8_ssie split.
 *
 * @details
 * Single definition lives in ``ra8_ssie.c``; ``ra8_ssie_stream.c`` reads and
 * writes the same object through this ``extern`` declaration. The mutable
 * DMA-channel bookkeeping must remain one object so that attach/detach in the
 * stream TU and init/deinit in the lifecycle TU agree on per-channel state.
 *
 * @note Module-private to ``libs/ra8_hal/src``; not part of the public surface.
 * @warning Not thread-safe; callers must serialise concurrent access.
 * @since 0.1.0
 */
extern ra8_ssie_runtime_t s_ssie_runtime[k_ra8_ssie_channel_count];

/**
 * @brief Validate an SSIE channel index and return its register block.
 *
 * @details
 * Shared between ``ra8_ssie.c`` and ``ra8_ssie_stream.c``. Promoted from a
 * file-scope ``static`` helper to TU-external linkage so the streaming TU can
 * reuse the same channel-bounds check before touching SSIE registers. Returns
 * ``nullptr`` for an out-of-range channel exactly like ``ra8_ssie()``.
 *
 * @param[in] channel Channel index supplied by the caller.
 * @return Volatile pointer to the channel's register block, or ``nullptr`` on
 *         an out-of-range channel index.
 *
 * @retval nullptr Channel index is out of range.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_PRIV volatile r_ssie_regs_t* ra8_ssie_internal_regs(uint8_t channel);

/* =============================================================================
 * ra8_usb_hmsc: split between ra8_usb_hmsc.c (CBW/CSW helpers, lifecycle, attach
 * callback, BOT/SCSI command path) and ra8_usb_hmsc_enum.c (the polled
 * enumeration ladder). Both translation units read and write the singleton
 * shadow state, so it stays a single object: the type and the extern
 * declaration live here, the one definition lives in ra8_usb_hmsc.c.
 * =============================================================================
 */

/**
 * @struct ra8_usb_hmsc_state_t
 * @brief Singleton shadow state for the host-MSC driver.
 *
 * @details
 * Shared across the ra8_usb_hmsc split. The lifecycle / command-path TU
 * (``ra8_usb_hmsc.c``) and the enumeration-ladder TU (``ra8_usb_hmsc_enum.c``)
 * both read and write this object, so it remains a single instance with the
 * type declared here in the shared private header.
 *
 * @invariant ``attached`` is only true after a completed enumeration pass.
 * @note Module-private to ``libs/ra8_hal/src``; not part of the public surface.
 * @since 0.1.0
 */
typedef struct {
  bool                     initialized;  /**< True after init.          */
  bool                     attached;     /**< True after enum done.     */
  ra8_usb_speed_t          speed;        /**< Underlying controller.    */
  ra8_usb_hmsc_attach_fn_t attach_cb;    /**< Attach callback, or NULL. */
  void*                    attach_ctx;   /**< Attach callback ctx.      */
  ra8_usb_hmsc_device_t    device;       /**< Snapshot of attached dev. */
  uint32_t                 next_cbw_tag; /**< Monotonic BOT tag.        */
} ra8_usb_hmsc_state_t;

/**
 * @var s_usb_hmsc_state
 * @brief Singleton host-MSC shadow state, shared across the ra8_usb_hmsc split.
 *
 * @details
 * Single definition lives in ``ra8_usb_hmsc.c``; ``ra8_usb_hmsc_enum.c`` reads
 * and writes the same object through this ``extern`` declaration. The mutable
 * state (attach flags, negotiated speed, device snapshot, BOT tag counter)
 * must remain one object so the enumeration ladder and the command path agree
 * on the attached device.
 *
 * @note Module-private to ``libs/ra8_hal/src``; not part of the public surface.
 * @warning Not thread-safe; callers must serialise concurrent access.
 * @since 0.1.0
 */
extern ra8_usb_hmsc_state_t s_usb_hmsc_state;

/* =============================================================================
 * ra8_canfd: split between ra8_canfd.c (clock / MSTP / global+channel mode state
 * machine, AFL / RX-FIFO bring-up, lifecycle, status / IRQ / filter / test-mode
 * / power) and two siblings: ra8_canfd_frame.c (TX/RX frame data path) and
 * ra8_canfd_timing.c (bit-timing solver + bitrate / BRS configuration). The
 * timing TU has to drive the channel mode state machine to land NCFG / DCFG
 * edits in CH_RESET, so the channel-mode helper -- whose definition stays in
 * ra8_canfd.c next to the rest of the mode machinery -- is promoted to
 * TU-external linkage here. No mutable state crosses the split.
 * =============================================================================
 */

/**
 * @brief Drive ``CFDC[0].CTR.CHMDC`` and wait for the matching status bit.
 *
 * @details
 * Promoted from a file-scope ``static`` helper in ``ra8_canfd.c`` to TU-external
 * linkage so the bit-timing TU (``ra8_canfd_timing.c``) can transition the
 * channel through CH_RESET / CH_OPERATION around an NCFG / DCFG edit, which is
 * only writable while the channel is halted or in reset. The single definition
 * stays in ``ra8_canfd.c`` beside the rest of the mode-transition machinery
 * (HUM Ch 41 p 2762 "CFDCnCTR.CHMDC", p 2766 "CFDCnSTS").
 *
 * @param[in] reg  CANFD channel register block.
 * @param[in] mode Requested channel mode (reset / halt / operation).
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok            The channel latched the requested mode.
 * @retval k_ra8_err_hw_timeout The status bit never latched within the budget.
 *
 * @pre ``reg`` is non-NULL and points to a CANFD instance.
 * @pre The CANFD block clock and channel MSTP gate are already alive.
 * @post On success the channel state machine reflects @p mode.
 * @post No global state is modified on the error path.
 *
 * @note Not thread-safe; init / reconfigure context only.
 * @note Module-private to ``libs/ra8_hal/src``; not part of the public surface.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t ra8_canfd_internal_set_channel_mode(volatile r_canfd_t* reg,
                                                       ra8_chmdc_mode_t    mode);
