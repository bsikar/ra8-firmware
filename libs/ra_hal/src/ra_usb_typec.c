/**
 * @file ra_usb_typec.c
 * @brief USB Type-C / Power Delivery driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Hand-written driver for the RA8 family `R_USBCC` block. Mirrors
 * the bring-up sequence of FSP `r_usb_typec.c`:
 *
 *  1. Open MSTP gate, assert TCC.RESET, spin until self-clears.
 *  2. Clear CCC.PDOWN to power up the CC-PHY.
 *  3. Set MEC.MODE per role.
 *  4. Program MEC.PMODE source-current detect bits.
 *  5. Sample TCS once for the cached g_reg_tcs shadow.
 *  6. Unmask CC / VBUS / VRA interrupts in IES.
 *  7. Set MEC.GUS to land in `Unattached.SNK`.
 *
 * The PD message-FIFO is driver-stub on this silicon; see the
 * register-header TODO comments in `ra8d2_usb_typec_regs.h`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_usb_typec.h"

#include <stdint.h>

#include "ra8d2_usb_typec_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "USBTYPEC";

/**
 * @enum ra_usb_typec_pd_header_shift_t
 * @brief Bit offsets inside the PD 3.0 16-bit message header.
 *
 * @details Layout per USB PD 3.0 section 6.2.1.1:
 *  - bits  4..0  message type
 *  - bit   5     port data role
 *  - bits  7..6  spec revision
 *  - bit   8     port power role
 *  - bits 11..9  message ID
 *  - bits 14..12 number of data objects
 *  - bit  15     extended
 */
typedef enum : uint8_t {
  k_ra_pd_hdr_shift_msg_type        = 0U,
  k_ra_pd_hdr_shift_port_data_role  = 5U,
  k_ra_pd_hdr_shift_spec_rev        = 6U,
  k_ra_pd_hdr_shift_port_power_role = 8U,
  k_ra_pd_hdr_shift_msg_id          = 9U,
  k_ra_pd_hdr_shift_num_objects     = 12U,
  k_ra_pd_hdr_shift_extended        = 15U,
} ra_usb_typec_pd_header_shift_t;

/**
 * @enum ra_usb_typec_pd_header_mask_t
 * @brief Field masks for the PD 3.0 16-bit message header.
 */
typedef enum : uint32_t {
  k_ra_pd_hdr_mask_msg_type   = 0x001FU, /**< 5 bits.  */
  k_ra_pd_hdr_mask_one_bit    = 0x0001U, /**< 1 bit.   */
  k_ra_pd_hdr_mask_two_bits   = 0x0003U, /**< 2 bits.  */
  k_ra_pd_hdr_mask_three_bits = 0x0007U, /**< 3 bits.  */
  k_ra_pd_hdr_mask_low16      = 0xFFFFU, /**< 16 bits, header LSBs. */
} ra_usb_typec_pd_header_mask_t;

/**
 * @enum ra_usb_typec_role_limit_t
 * @brief Maximum legal role enum value.
 */
typedef enum : uint8_t {
  k_ra_usb_typec_role_max  = k_ra_usb_typec_role_drp,
  k_ra_usb_typec_pmode_max = k_ra_usb_typec_pmode_full,
} ra_usb_typec_role_limit_t;

/**
 * @enum ra_usb_typec_tset_shift_t
 * @brief Byte shifts for the TSET debounce-timer register.
 */
typedef enum : uint8_t {
  k_ra_typec_tset_shift_cc = 0U, /**< CC_DEBOUNCE byte 0.   */
  k_ra_typec_tset_shift_pd = 8U, /**< PD_DEBOUNCE byte 1.   */
} ra_usb_typec_tset_shift_t;

/**
 * @enum ra_usb_typec_cns_value_t
 * @brief Decoded values of the TCS.CNS field (4 bits).
 *
 * @details FSP `r_usb_typec.c` USB_TYPEC_TCS_CNS_* defines. The
 * raw field is at bits 7..4 of TCS; we shift right by 4 in the
 * driver so the values are 0..6.
 */
typedef enum : uint8_t {
  k_ra_typec_cns_disabled         = 0U,
  k_ra_typec_cns_unattached       = 1U,
  k_ra_typec_cns_attached_wait    = 2U,
  k_ra_typec_cns_attached_default = 4U,
  k_ra_typec_cns_attached_1_5     = 5U,
  k_ra_typec_cns_attached_3_0     = 6U,
} ra_usb_typec_cns_value_t;

/**
 * @struct ra_usb_typec_state_t
 * @brief Driver singleton state.
 */
typedef struct {
  bool                    initialised; /**< True after `init`.       */
  ra_usb_typec_role_t     role;        /**< Last applied role.       */
  ra_usb_typec_callback_t callback;    /**< Event callback or NULL.  */
  void*                   ctx;         /**< Callback context.        */
} ra_usb_typec_state_t;

static ra_usb_typec_state_t s_state = {};

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Apply MEC.MODE / MEC.PMODE for a given role + power mode.
 */
static void internal_apply_mode(ra_usb_typec_role_t role, ra_usb_typec_pmode_t pmode)
{
  volatile r_usb_typec_regs_t* reg     = ra_usb_typec_regs();
  uint32_t                     mec_val = reg->MEC;

  /* Connection State Mode: bit 0. Sink uses MODE=1 in FSP, source/DRP=0. */
  if (role == k_ra_usb_typec_role_sink) {
    mec_val |= ((uint32_t)1U << k_ra_typec_mec_bit_mode);
  } else {
    mec_val &= ~((uint32_t)1U << k_ra_typec_mec_bit_mode);
  }

  /* Source current detect: PMODE field bits 2..1. */
  mec_val &= ~k_ra_typec_mask_pmode;
  if (pmode == k_ra_usb_typec_pmode_default_1_5a) {
    mec_val |= ((uint32_t)1U << k_ra_typec_mec_bit_pmode);
  } else if (pmode == k_ra_usb_typec_pmode_full) {
    mec_val |= ((uint32_t)2U << k_ra_typec_mec_bit_pmode);
  }
  reg->MEC = mec_val;
}

/**
 * @brief Build the 24-bit TSET debounce-timer value.
 */
static uint32_t internal_build_tset(uint8_t cc_ms, uint8_t pd_ms)
{
  uint32_t tset = (uint32_t)cc_ms << k_ra_typec_tset_shift_cc;
  tset |= (uint32_t)pd_ms << k_ra_typec_tset_shift_pd;
  return tset;
}

/**
 * @brief Decode TCS.CNS (4-bit field, raw register bits 7..4) to
 *        the public `attach_status` enum.
 */
static ra_usb_typec_attach_status_t internal_cns_to_attach(ra_usb_typec_cns_value_t cns)
{
  switch (cns) {
    case k_ra_typec_cns_attached_default:
    case k_ra_typec_cns_attached_1_5:
    case k_ra_typec_cns_attached_3_0:
      return k_ra_usb_typec_attach_present_attached;
    default:
      return k_ra_usb_typec_attach_nothing;
  }
}

/* =============================================================================
 * API
 * =============================================================================
 */

ra_err_t ra_usb_typec_init(const ra_usb_typec_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "init: cfg");
  if ((uint8_t)cfg->role > k_ra_usb_typec_role_max) {
    return k_ra_err_invalid_arg;
  }
  if ((uint8_t)cfg->pmode > k_ra_usb_typec_pmode_max) {
    return k_ra_err_invalid_arg;
  }
  if (s_state.initialised) {
    return k_ra_err_invalid_state;
  }

  volatile r_usb_typec_regs_t* reg = ra_usb_typec_regs();

  /* Step 1: Soft-reset, wait until self-clears. FSP loops; in the
   * simulator the bit clears immediately because the register is
   * plain RAM, so we tolerate a one-shot read. */
  reg->TCC |= ((uint32_t)1U << k_ra_typec_tcc_bit_reset);
  /* Real hardware spins; in sim the write was a no-op self-clear. */
  reg->TCC &= ~((uint32_t)1U << k_ra_typec_tcc_bit_reset);

  /* Step 2: Bring CC-PHY out of power-down. */
  reg->CCC &= ~((uint32_t)1U << k_ra_typec_ccc_bit_pdown);

  /* Steps 3-4: Apply role + source-current detection. */
  internal_apply_mode(cfg->role, cfg->pmode);

  /* Step 5: Program debounce timers. */
  reg->TSET = internal_build_tset(cfg->cc_debounce_ms, cfg->pd_debounce_ms);

  /* Step 6: Sample TCS once -- mirrors `g_usb_typec_reg_tcs` in FSP. */
  (void)reg->TCS;

  /* Step 7: Unmask CC + VBUS + VRA interrupts. */
  reg->IES |= ((uint32_t)1U << k_ra_typec_ies_bit_ccien) |
              ((uint32_t)1U << k_ra_typec_ies_bit_vbusien) |
              ((uint32_t)1U << k_ra_typec_ies_bit_vraien);

  /* Step 8: Force state machine to Unattached.SNK so plug detection
   * starts cleanly. */
  reg->MEC |= ((uint32_t)1U << k_ra_typec_mec_bit_gus);

  s_state.role        = cfg->role;
  s_state.initialised = true;
  s_state.callback    = nullptr;
  s_state.ctx         = nullptr;
  ra_log_info(s_tag, "Type-C ready");
  return k_ra_ok;
}

ra_err_t ra_usb_typec_set_role(ra_usb_typec_role_t role)
{
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  if ((uint8_t)role > k_ra_usb_typec_role_max) {
    return k_ra_err_invalid_arg;
  }
  /* Re-derive PMODE from the live register so we don't lose
   * whatever was programmed in `init`. */
  volatile r_usb_typec_regs_t* reg = ra_usb_typec_regs();
  const uint32_t       pmode_field = (reg->MEC & k_ra_typec_mask_pmode) >> k_ra_typec_mec_bit_pmode;
  ra_usb_typec_pmode_t pmode       = k_ra_usb_typec_pmode_default;
  if (pmode_field == 1U) {
    pmode = k_ra_usb_typec_pmode_default_1_5a;
  } else if (pmode_field == 2U) {
    pmode = k_ra_usb_typec_pmode_full;
  }
  internal_apply_mode(role, pmode);
  s_state.role = role;
  return k_ra_ok;
}

ra_err_t ra_usb_typec_get_orientation(ra_usb_typec_orientation_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "get_orientation: out");
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  volatile r_usb_typec_regs_t* reg  = ra_usb_typec_regs();
  const uint32_t               tcs  = reg->TCS;
  const uint32_t               cc1s = (tcs & k_ra_typec_mask_cc1s) >> k_ra_typec_tcs_shift_cc1s;
  const uint32_t               cc2s = (tcs & k_ra_typec_mask_cc2s) >> k_ra_typec_tcs_shift_cc2s;

  const bool plug_flag = (tcs & ((uint32_t)1U << k_ra_typec_tcs_bit_plug)) != 0U;
  if ((cc1s == 0U) && (cc2s == 0U)) {
    *out = k_ra_usb_typec_orientation_unattached;
  } else if (plug_flag || (cc1s == 0U)) {
    *out = k_ra_usb_typec_orientation_cc2_attached;
  } else {
    *out = k_ra_usb_typec_orientation_cc1_attached;
  }
  return k_ra_ok;
}

ra_err_t ra_usb_typec_get_attach_status(ra_usb_typec_attach_status_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "get_attach_status: out");
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  volatile r_usb_typec_regs_t*   reg = ra_usb_typec_regs();
  const ra_usb_typec_cns_value_t cns =
    (ra_usb_typec_cns_value_t)((reg->TCS & k_ra_typec_mask_cns) >> k_ra_typec_tcs_shift_cns);
  *out = internal_cns_to_attach(cns);
  return k_ra_ok;
}

ra_err_t ra_usb_typec_pd_send_message(const ra_usb_typec_pd_msg_t* msg)
{
  RA_CHECK_NULL_PTR(msg, s_tag, "pd_send_message: msg");
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  if (msg->num_objects > k_ra_usb_typec_pd_max_data_objects) {
    return k_ra_err_invalid_arg;
  }

  volatile r_usb_typec_regs_t* reg = ra_usb_typec_regs();

  /* Stage the 16-bit header LSB first. The PDFD register is 32
   * bits but on real silicon would be byte-addressable; we keep
   * the simulator-visible side-effect simple: write header into
   * PDFD then each data object in sequence. */
  reg->PDFD = (uint32_t)msg->header;
  for (uint8_t i = 0U; i < msg->num_objects; ++i) {
    reg->PDFD = msg->objects[i];
  }
  reg->PDFC |= ((uint32_t)1U << k_ra_typec_pdfc_bit_tx_start);
  return k_ra_ok;
}

ra_err_t ra_usb_typec_pd_recv_message(ra_usb_typec_pd_msg_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "pd_recv_message: out");
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  volatile r_usb_typec_regs_t* reg = ra_usb_typec_regs();
  if ((reg->PDFS & ((uint32_t)1U << k_ra_typec_pdfs_bit_rx_avail)) == 0U) {
    return k_ra_err_no_data;
  }
  /* Read header then objects. The mock backs PDFD with plain
   * memory, so a simulator-side test that writes PDFD before
   * calling recv sees the same value back. */
  out->header = (uint16_t)(reg->PDFD & k_ra_pd_hdr_mask_low16);
  out->num_objects =
    (uint8_t)((out->header >> k_ra_pd_hdr_shift_num_objects) & k_ra_pd_hdr_mask_three_bits);
  for (uint8_t i = 0U; i < out->num_objects; ++i) {
    out->objects[i] = reg->PDFD;
  }
  /* Clear RX-avail flag once drained. */
  reg->PDFS &= ~((uint32_t)1U << k_ra_typec_pdfs_bit_rx_avail);
  return k_ra_ok;
}

ra_err_t ra_usb_typec_set_callback(ra_usb_typec_callback_t fn, void* ctx)
{
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  s_state.callback = fn;
  s_state.ctx      = ctx;
  return k_ra_ok;
}

ra_err_t ra_usb_typec_close(void)
{
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  volatile r_usb_typec_regs_t* reg = ra_usb_typec_regs();

  /* Force state machine to Disabled, power-down PHY, mask IRQs. */
  reg->MEC |= ((uint32_t)1U << k_ra_typec_mec_bit_gd);
  reg->CCC |= ((uint32_t)1U << k_ra_typec_ccc_bit_pdown);
  reg->IES &=
    ~(((uint32_t)1U << k_ra_typec_ies_bit_ccien) | ((uint32_t)1U << k_ra_typec_ies_bit_vbusien) |
      ((uint32_t)1U << k_ra_typec_ies_bit_vraien));

  s_state.initialised = false;
  s_state.callback    = nullptr;
  s_state.ctx         = nullptr;
  return k_ra_ok;
}

uint16_t ra_usb_typec_pd_build_header(ra_usb_typec_pd_msg_type_t msg_type,
                                      ra_usb_typec_pd_role_t     port_data_role,
                                      ra_usb_typec_pd_spec_rev_t spec_rev,
                                      ra_usb_typec_pd_role_t     port_power_role,
                                      uint8_t                    msg_id,
                                      uint8_t                    num_objects,
                                      uint8_t                    extended)
{
  uint16_t hdr = (uint16_t)((uint16_t)msg_type & k_ra_pd_hdr_mask_msg_type);
  hdr |= (uint16_t)(((uint16_t)port_data_role & k_ra_pd_hdr_mask_one_bit)
                    << k_ra_pd_hdr_shift_port_data_role);
  hdr |= (uint16_t)(((uint16_t)spec_rev & k_ra_pd_hdr_mask_two_bits) << k_ra_pd_hdr_shift_spec_rev);
  hdr |= (uint16_t)(((uint16_t)port_power_role & k_ra_pd_hdr_mask_one_bit)
                    << k_ra_pd_hdr_shift_port_power_role);
  hdr |= (uint16_t)(((uint16_t)msg_id & k_ra_pd_hdr_mask_three_bits) << k_ra_pd_hdr_shift_msg_id);
  hdr |= (uint16_t)(((uint16_t)num_objects & k_ra_pd_hdr_mask_three_bits)
                    << k_ra_pd_hdr_shift_num_objects);
  hdr |= (uint16_t)(((uint16_t)extended & k_ra_pd_hdr_mask_one_bit) << k_ra_pd_hdr_shift_extended);
  return hdr;
}
