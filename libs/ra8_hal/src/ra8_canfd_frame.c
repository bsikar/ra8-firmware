/**
 * @file ra8_canfd_frame.c
 * @brief CANFD frame transmit / receive data path (split from ra8_canfd.c)
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Frame-level half of the RA8D2 CANFD driver, split out of
 * ``ra8_canfd.c`` purely to satisfy the per-file size cap. This
 * translation unit owns the TX/RX message-buffer path:
 *
 *  - Range-check a ``ra8_canfd_frame_t`` against the protocol limits,
 *    assemble the ``CFDTM[0].ID/PTR/FDCTR`` words, copy the payload
 *    into ``CFDTM[0].DF[]`` and assert ``CFDTMC[0].TMTR``.
 *  - Pop a frame from RX FIFO 0 by polling ``CFDRFSTS[0].RFEMP``,
 *    reading ``CFDRF[0].ID/PTR/FDSTS/DF[]``, decoding the header, and
 *    writing ``CFDRFPCTR[0]`` to advance the pointer.
 *  - Read TEC/REC out of ``CFDC[0].STS`` for the error-state query.
 *
 * This TU compiles the identical register sequence on every build; host
 * unit tests stage the RAM-backed ``CFDRF[0]`` receive buffer and the
 * ``CFDRFSTS[0]`` empty flag directly (the values the silicon RX engine
 * would have latched) and drive the real receive path against them.
 *
 * Every register access carries a HUM Ch 41 "CAN with Flexible
 * Data-rate (CANFD)" citation (pages 2702..2867, chapter map row 41)
 * or an FSP ``r_canfd.c`` line citation when the bit semantics come
 * from the reference driver.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_canfd.h"
#include "ra8_canfd_regs.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hw_err.h"

static const char* s_tag = "CANFD";

/**
 * @enum ra8_canfd_frame_spin_t
 * @brief TX-completion poll budget owned by the frame data path.
 *
 * @details
 * Private copy of the matching constant in ``ra8_canfd.c``; this TU only
 * needs the TX-completion budget. Bounds the back-to-back TMTRF poll in
 * ::ra8_canfd_transmit. ``k_ra8_canfd_tx_spin`` is a read-only compile-time
 * constant, so each translation unit keeps its own copy rather than
 * promoting it to external linkage.
 */
typedef enum : uint32_t {
  k_ra8_canfd_tx_spin = 100000U, /**< TX-completion poll: 500 us @ 1 GHz / 5 cyc. */
} ra8_canfd_frame_spin_t;

/**
 * @enum ra8_canfd_buffer_idx_t
 * @brief Indices the driver currently owns within the channel block.
 */
typedef enum : uint8_t {
  k_ra8_canfd_tx_mb_default   = 0U, /**< Driver uses TX MB 0 for fire-and-forget. */
  k_ra8_canfd_rx_fifo_default = 0U, /**< Driver uses RX FIFO 0 for poll-receive.  */
} ra8_canfd_buffer_idx_t;

/**
 * @brief Range-check a `ra8_canfd_frame_t` against the protocol limits.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] frame See header declaration for direction and constraints.
 * @return ``ra8_err_t`` error code (or void if the signature returns void).
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_validate_frame(const ra8_canfd_frame_t* frame)
{
  if (frame->dlc > k_ra8_canfd_dlc_max) {
    return k_ra8_err_invalid_arg;
  }
  if (frame->is_extended == 0U) {
    if ((frame->id & ~k_ra8_canfd_id_std_mask) != 0U) {
      return k_ra8_err_invalid_arg;
    }
  } else {
    if ((frame->id & ~k_ra8_canfd_id_ext_mask) != 0U) {
      return k_ra8_err_invalid_arg;
    }
  }
  if ((frame->is_brs != 0U) && (frame->is_fd == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Copy the frame payload into `CFDTM[0].DF[]`. Unused bytes left intact.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] reg See header declaration for direction and constraints.
 * @param[in] frame See header declaration for direction and constraints.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_write_tx_data(volatile r_canfd_t* reg, const ra8_canfd_frame_t* frame)
{
  for (uint8_t b = 0U; b < k_ra8_canfd_data_bytes_max; b++) {
    reg->CFDTM[k_ra8_canfd_tx_mb_default].DF[b] = frame->data[b];
  }
}

/**
 * @brief Assemble the CFDTM[0].ID word (raw ID plus IDE flag).
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] frame See header declaration for direction and constraints.
 * @return ``ra8_err_t`` error code (or void if the signature returns void).
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_tx_id(const ra8_canfd_frame_t* frame)
{
  const uint32_t masked = (frame->is_extended != 0U) ? (frame->id & k_ra8_canfd_id_ext_mask)
                                                     : (frame->id & k_ra8_canfd_id_std_mask);
  return (frame->is_extended != 0U) ? (masked | k_ra8_canfd_id_ide) : masked;
}

/**
 * @brief Assemble the CFDTM[0].FDCTR word (FDF/BRS/ESI flags).
 *
 * @details
 * FSP r_canfd.c line ~676: `p_frame->options & 7` packs ESI/BRS/FDF
 * directly into bits [2:0]. We model the same here.
 *
 * @param[in] frame See header declaration for direction and constraints.
 * @return ``ra8_err_t`` error code (or void if the signature returns void).
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_tx_fdctr(const ra8_canfd_frame_t* frame)
{
  uint32_t w = 0U;
  if (frame->is_fd != 0U) {
    w |= k_ra8_canfd_fd_fdf;
  }
  if (frame->is_brs != 0U) {
    w |= k_ra8_canfd_fd_brs;
  }
  return w;
}

/**
 * @brief Best-effort bounded spin until CFDTMSTSj.TMTRF reads "complete".
 *
 * @details Waits for CFDTMSTSj.TMTRF[1:0] to read "transmission
 * complete" (10b) -- HUM Ch 41 "CFDTMSTSj.TMTRF" p ~2756. The previous
 * mask (0x06) also matched 01b ("transmission requested"), which on
 * back-to-back TX calls let the second call clobber CFDTMSTS while the
 * first frame was still in flight; the chip then silently dropped the
 * second TX and canfd_filter_demo's mask sub-round (sent immediately
 * after the exact sub-round) saw the frame disappear. Mask 0x04 keys
 * on TMTRF[1] only, which is only set once the TX is actually on the
 * wire and the MB is free. 500 kbit/s + 8 bytes = ~240 us;
 * k_ra8_canfd_tx_spin (~500 us at 1 GHz / 5 cycles per iter) covers it
 * with margin. The wait is best-effort by design (no error on
 * exhaustion); on host tests the loop-exit decision comes from the
 * ra8_fake_mmio seam so the retry and full-budget legs run there too.
 *
 * @param[in] reg CANFD register block for the transmitting channel.
 *
 * @pre ``reg`` is non-NULL (validated by the caller).
 * @pre TXREQ was just asserted on the default TX mailbox.
 * @post At most ::k_ra8_canfd_tx_spin polls have been issued.
 * @post No register state is modified (read-only poll).
 *
 * @note Not thread-safe; fire-and-forget TX path only.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_wait_tx_complete(volatile r_canfd_t* reg)
{
  enum : uint8_t { k_ra8_tmsts_tmtrf_done = 0x04U /**< RA8 tmsts tmtrf done. */ };
  for (uint32_t i = 0U; i < k_ra8_canfd_tx_spin; i++) {
#if defined(RA8_OFF_TARGET) && defined(UNIT_TEST)
    if (ra8_fake_mmio_wait_eval(
          &reg->CFDTMSTS[k_ra8_canfd_tx_mb_default],
          i,
          ((reg->CFDTMSTS[k_ra8_canfd_tx_mb_default] & k_ra8_tmsts_tmtrf_done) != 0U))) {
      break;
    }
#else
    if ((reg->CFDTMSTS[k_ra8_canfd_tx_mb_default] & k_ra8_tmsts_tmtrf_done) != 0U) {
      break;
    }
#endif
  }
}

ra8_err_t ra8_canfd_transmit(uint8_t channel, const ra8_canfd_frame_t* frame)
{
  volatile r_canfd_t* reg = ra8_canfd(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  RA8_CHECK_NULL_PTR(frame, s_tag, "frame must not be nullptr");

  const ra8_err_t v = internal_validate_frame(frame);
  if (v != k_ra8_ok) {
    return v;
  }

  /* Clear the previous transmission's TMTRF before asserting TXREQ:
   * HUM Ch 41 "CFDTMSTSj.TMTRF" p ~2756 says TMTR is only honored when
   * TMTRF is 00b. After the first successful TX the chip leaves
   * TMTRF=10b ("transmission successful") and silently drops every
   * subsequent TXREQ -- the symptom is the first round-trip working
   * and every later one returning no_data. */
  reg->CFDTMSTS[k_ra8_canfd_tx_mb_default] = 0U;

  /* HUM Ch 41 p 2806 "CFDTMID/CFDTMPTR/CFDTMFDCTR/CFDTMDF" +
   * FSP r_canfd.c line ~668..684. */
  reg->CFDTM[k_ra8_canfd_tx_mb_default].ID  = internal_tx_id(frame);
  reg->CFDTM[k_ra8_canfd_tx_mb_default].PTR = ((uint32_t)frame->dlc & k_ra8_canfd_ptr_mask_dlc)
                                              << (uint32_t)k_ra8_canfd_ptr_shift_dlc;
  reg->CFDTM[k_ra8_canfd_tx_mb_default].FDCTR = internal_tx_fdctr(frame);
  internal_write_tx_data(reg, frame);

  /* HUM Ch 41 p 2810 "CFDTMC" -- single-byte transmit-request register.
   * FSP r_canfd.c line ~724: `p_reg->CFDTMC[idx] = 1`. */
  reg->CFDTMC[k_ra8_canfd_tx_mb_default] = k_ra8_canfd_tmc_txreq;

  internal_wait_tx_complete(reg);
  return k_ra8_ok;
}

/**
 * @brief Copy 64 bytes of RX FIFO data from `CFDRF[0].DF[]` into the caller buffer.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] reg See header declaration for direction and constraints.
 * @param[in] out See header declaration for direction and constraints.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_read_rx_data(volatile r_canfd_t* reg, ra8_canfd_frame_t* out)
{
  for (uint8_t b = 0U; b < k_ra8_canfd_data_bytes_max; b++) {
    out->data[b] = reg->CFDRF[k_ra8_canfd_rx_fifo_default].DF[b];
  }
}

/**
 * @brief Decode the raw CFDRF[0].ID/PTR/FDSTS into `out`.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] id_word See header declaration for direction and constraints.
 * @param[in] ptr_word See header declaration for direction and constraints.
 * @param[in] fdsts_word See header declaration for direction and constraints.
 * @param[in] out See header declaration for direction and constraints.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_decode_rx_header(uint32_t           id_word,
                                      uint32_t           ptr_word,
                                      uint32_t           fdsts_word,
                                      ra8_canfd_frame_t* out)
{
  const uint8_t is_ext = (uint8_t)(((id_word & k_ra8_canfd_id_ide) != 0U) ? 1U : 0U);
  out->is_extended     = is_ext;
  out->id =
    (is_ext != 0U) ? (id_word & k_ra8_canfd_id_ext_mask) : (id_word & k_ra8_canfd_id_std_mask);
  out->dlc =
    (uint8_t)((ptr_word >> (uint32_t)k_ra8_canfd_ptr_shift_dlc) & k_ra8_canfd_ptr_mask_dlc);
  out->is_fd  = ((fdsts_word & k_ra8_canfd_fd_fdf) != 0U) ? 1U : 0U;
  out->is_brs = ((fdsts_word & k_ra8_canfd_fd_brs) != 0U) ? 1U : 0U;
}

ra8_err_t ra8_canfd_receive(uint8_t channel, ra8_canfd_frame_t* out_frame)
{
  volatile r_canfd_t* reg = ra8_canfd(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  RA8_CHECK_NULL_PTR(out_frame, s_tag, "out_frame must not be nullptr");

  /* HUM Ch 41 "CFDRFSTSn.RFEMP" p 2754 */ /* "CFDRFSTSn.RFEMP" -- empty flag is bit 0. */
  if ((reg->CFDRFSTS[k_ra8_canfd_rx_fifo_default] & k_ra8_rfsts_bit_empty) != 0U) {
    return k_ra8_err_no_data;
  }

  /* HUM Ch 41 "CFDRFn ID/PTR/FDSTS/DF" p 2796 */ /* "CFDRFn ID/PTR/FDSTS/DF". */
  internal_decode_rx_header(reg->CFDRF[k_ra8_canfd_rx_fifo_default].ID,
                            reg->CFDRF[k_ra8_canfd_rx_fifo_default].PTR,
                            reg->CFDRF[k_ra8_canfd_rx_fifo_default].FDSTS,
                            out_frame);
  internal_read_rx_data(reg, out_frame);

  /* HUM Ch 41 p 2756 "CFDRFPCTRn.RFPC" -- write 0xFF to advance pointer.
   * FSP r_canfd.c uses the same dummy 0xFF write to pop. */
  reg->CFDRFPCTR[k_ra8_canfd_rx_fifo_default] = k_ra8_rfpctr_value_ack;
  return k_ra8_ok;
}

ra8_err_t ra8_canfd_get_error_state(uint8_t channel, uint8_t* tx_err, uint8_t* rx_err)
{
  volatile r_canfd_t* reg = ra8_canfd(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  RA8_CHECK_NULL_PTR(tx_err, s_tag, "tx_err must not be nullptr");
  RA8_CHECK_NULL_PTR(rx_err, s_tag, "rx_err must not be nullptr");

  /* HUM Ch 41 p 2766 "CFDCnSTS" -- TEC[31:24] / REC[23:16] live in
   * CFDC[0].STS, NOT in CFDC[0].ERFL like the previous header model. */
  const uint32_t sts = reg->CFDC[0].STS;
  *tx_err            = (uint8_t)((sts >> (uint32_t)k_ra8_cnsts_bit_tec) & k_ra8_cnsts_mask_tec);
  *rx_err            = (uint8_t)((sts >> (uint32_t)k_ra8_cnsts_bit_rec) & k_ra8_cnsts_mask_rec);
  return k_ra8_ok;
}
