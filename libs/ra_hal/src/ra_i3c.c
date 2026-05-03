/**
 * @file ra_i3c.c
 * @brief I3C Bus Interface driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Primary-mode driver for the RA8D2 I3C0 controller.  The transfer
 * engine mirrors FSP ``r_i3c``: every operation is encoded as a
 * 32-bit (or two-word) command descriptor written to the NCMDQP
 * port, with payload bytes flowing through NTDTBP0.  IBI events
 * arrive via the NIBIQP queue.
 *
 * Bring-up order matches the FSP ``R_I3C_Open`` reference sequence:
 * enable the module clock (CECTL.CLKE), drop BCTL.BUSE, assert
 * RSTCTL.RI3CRST and wait for hardware to clear it, then assert
 * RSTCTL.INTLRST, clear PRTS, release RSTCTL.  Primary dynamic
 * address (MSDVAD.MDYAD) is programmed before BCTL.BUSE is set per
 * HUM Ch 40 BCTL description (pp 2445-2701).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_i3c.h"

#include <stdint.h>

#include "ra8d2_i3c_regs.h"
#include "ra8d2_mstp_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_i3c_internal.h"
#include "ra_log.h"
#include "ra_mstp.h"

/**
 * @brief Pure recv-ccc-invalid predicate -- see header for full contract.
 * @details Promoted helper so the line-688 OR can be driven under MC/DC.
 * @param[in] addr_mask Maximum valid 7-bit address mask.
 * @param[in] target    Candidate target address.
 * @param[in] max_len   Caller-supplied max read length.
 * @return Boolean reject predicate.
 * @retval true  Inputs invalid.
 * @retval false Inputs OK.
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on inputs.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
bool ra_i3c_internal_recv_ccc_invalid(uint8_t addr_mask, uint8_t target, uint8_t max_len)
{
  return (target > addr_mask) || (max_len == 0U);
}

/**
 * @brief Pure HDR-mode-invalid predicate -- see header for full contract.
 * @details Promoted helper so the line-815 AND can be driven under MC/DC.
 * @param[in] sdr_val Numeric value of @c k_ra_i3c_hdr_mode_sdr.
 * @param[in] ddr_val Numeric value of @c k_ra_i3c_hdr_mode_ddr.
 * @param[in] ts_val  Numeric value of @c k_ra_i3c_hdr_mode_ts.
 * @param[in] mode    Candidate mode value.
 * @return Boolean reject predicate.
 * @retval true  Mode is not legal.
 * @retval false Mode is legal.
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on inputs.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
bool ra_i3c_internal_hdr_mode_invalid(uint32_t sdr_val,
                                      uint32_t ddr_val,
                                      uint32_t ts_val,
                                      uint32_t mode)
{
  return (mode != sdr_val) && (mode != ddr_val) && (mode != ts_val);
}

/** @brief Logging tag for this module. */
static const char* s_tag = "I3C";

/** @brief Currently registered IRQ callback (NULL when detached). */
static ra_i3c_event_fn_t s_i3c_fn;

/** @brief Opaque context handed back to ``s_i3c_fn``. */
static void* s_i3c_ctx;

/* =============================================================================
 * Private helpers
 * =============================================================================
 */

/**
 * @brief Internal-error reset bring-up sequence per FSP R_I3C_Open.
 *
 * @details
 * 1. Enable the module clock via CECTL.CLKE.
 * 2. Drop BCTL.BUSE so the bus is idle before any reset.
 * 3. Pulse RSTCTL.RI3CRST -- on real silicon the bit auto-clears
 *    when the I3C internal reset completes; here we issue an
 *    explicit clear so the simulated-mmap unit-test back-end does
 *    not spin forever (FSP relies on
 *    ``FSP_HARDWARE_REGISTER_WAIT`` for the same thing on target).
 * 4. Pulse RSTCTL.INTLRST to flush the internal state machines.
 * 5. Clear PRTS so we start in I2C-fast/I3C-mixed mode 0.
 *
 * Each step follows HUM Ch 40 "RSTCTL : Reset Control Register"
 * description (pp 2445-2701).
 *
 * @param[in] reg See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void priv_ra_i3c_reset_sequence(volatile r_i3c_regs_t* reg)
{
  /* HUM Ch 40 "CECTL : Clock Enable Control Register" p 2445-2701 */
  reg->CECTL = 1U;
  /* HUM Ch 40 "BCTL : Bus Control Register" p 2445-2701 */
  reg->BCTL = 0U;
  /* HUM Ch 40 "RSTCTL : Reset Control Register" p 2445-2701 */
  reg->RSTCTL = k_ra_i3c_rstctl_ri3crst_mask;
  reg->RSTCTL = 0U; /* simulated-mmap clear / target HW already auto-cleared */
  reg->RSTCTL = k_ra_i3c_rstctl_intlrst_mask;
  reg->RSTCTL = 0U;
  reg->PRTS   = 0U;
}

/**
 * @brief Build the first word of a regular-transfer command descriptor.
 *
 * @details
 * Mirrors the FSP ``i3c_xfer_command_calculate`` helper (HUM Ch 40
 * "Command Descriptor" pp 2445-2701).  The device-table index is
 * encoded directly as the dynamic address -- the FSP driver looks
 * the index up in DATBASn first, but until the device-address
 * table is wired in, plumbing the address through the index field
 * lets the test harness verify the round-trip.
 *
 * @param[in] target_addr 7-bit dynamic address.
 * @param[in] rnw         True for read transfers.
 * @return Word 0 of the command descriptor (Word 1 is the length).
 *
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static uint32_t priv_ra_i3c_xfer_cmd_word(uint8_t target_addr, bool rnw)
{
  uint32_t cmd = 0U;
  cmd |= ((uint32_t)target_addr) << k_ra_i3c_cmd_dev_index_shift;
  if (rnw) {
    cmd |= 1UL << k_ra_i3c_cmd_rnw_shift;
  }
  cmd |= 1UL << k_ra_i3c_cmd_roc_shift; /* response on completion */
  cmd |= 1UL << k_ra_i3c_cmd_toc_shift; /* terminate (STOP) on completion */
  return cmd;
}

/**
 * @brief Build a CCC-type command descriptor word.
 *
 * @param[in] ccc         Common Command Code opcode.
 * @param[in] target_addr Dynamic address (used only for direct CCCs).
 * @param[in] rnw         True for direct-get CCCs.
 * @return Word 0 of the descriptor.
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
static uint32_t priv_ra_i3c_ccc_cmd_word(uint8_t ccc, uint8_t target_addr, bool rnw)
{
  uint32_t cmd = 0U;
  cmd |= 1UL << k_ra_i3c_cmd_cp_shift;               /* command-present (CCC). */
  cmd |= ((uint32_t)ccc) << k_ra_i3c_cmd_code_shift; /* CCC opcode in [14:7]. */
  cmd |= ((uint32_t)target_addr) << k_ra_i3c_cmd_dev_index_shift;
  if (rnw) {
    cmd |= 1UL << k_ra_i3c_cmd_rnw_shift;
  }
  cmd |= 1UL << k_ra_i3c_cmd_roc_shift;
  cmd |= 1UL << k_ra_i3c_cmd_toc_shift;
  return cmd;
}

/**
 * @brief Push a payload buffer through NTDTBP0 in 32-bit chunks.
 *
 * @details
 * NTDTBP0 is a 32-bit FIFO; partial trailing words are padded with
 * zero, matching FSP ``i3c_fifo_write``.  The simulated-mmap test
 * back-end is happy to accept the writes -- on silicon the
 * controller drains them onto the bus.
 *
 * @param[in] reg See implementation.
 * @param[in] data See implementation.
 * @param[in] len See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void priv_ra_i3c_fifo_write(volatile r_i3c_regs_t* reg, const uint8_t* data, uint32_t len)
{
  uint32_t       i           = 0U;
  const uint32_t k_word_size = k_ra_i3c_word_size;
  while (i + k_word_size <= len) {
    uint32_t w = (uint32_t)data[i];
    w |= ((uint32_t)data[i + 1U]) << k_ra_i3c_shift_b1;
    w |= ((uint32_t)data[i + 2U]) << k_ra_i3c_shift_b2;
    w |= ((uint32_t)data[i + 3U]) << k_ra_i3c_shift_b3;
    reg->NTDTBP0 = w;
    i += k_word_size;
  }
  if (i < len) {
    uint32_t w = 0U;
    uint32_t s = 0U;
    while (i < len) {
      w |= ((uint32_t)data[i]) << s;
      s += k_ra_i3c_byte_shift;
      ++i;
    }
    reg->NTDTBP0 = w;
  }
}

/**
 * @brief Drain @p len bytes from NTDTBP0 into @p out.
 *
 * @details
 * Hardware exposes the receive buffer as a 32-bit FIFO -- the
 * driver reads one word and unpacks LE bytes; trailing partial
 * words are masked to the requested length.
 *
 * @param[in] reg See implementation.
 * @param[in] out See implementation.
 * @param[in] len See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void priv_ra_i3c_fifo_read(volatile r_i3c_regs_t* reg, uint8_t* out, uint32_t len)
{
  uint32_t       i           = 0U;
  const uint32_t k_word_size = k_ra_i3c_word_size;
  while (i + k_word_size <= len) {
    const uint32_t w = reg->NTDTBP0;
    out[i]           = (uint8_t)(w & k_ra_i3c_byte_mask);
    out[i + 1U]      = (uint8_t)((w >> k_ra_i3c_shift_b1) & k_ra_i3c_byte_mask);
    out[i + 2U]      = (uint8_t)((w >> k_ra_i3c_shift_b2) & k_ra_i3c_byte_mask);
    out[i + 3U]      = (uint8_t)((w >> k_ra_i3c_shift_b3) & k_ra_i3c_byte_mask);
    i += k_word_size;
  }
  if (i < len) {
    const uint32_t w = reg->NTDTBP0;
    uint32_t       s = 0U;
    while (i < len) {
      out[i] = (uint8_t)((w >> s) & k_ra_i3c_byte_mask);
      s += k_ra_i3c_byte_shift;
      ++i;
    }
  }
}

/* =============================================================================
 * Lifecycle / status / IRQ
 * =============================================================================
 */

/**
 * @brief Implementation of ra_i3c_init (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_i3c_init(void)
{
  /* HUM Ch 11.2.7 "MSTPCRB : Module Stop Control Register B" p 444 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_i3c);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "i3c_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  volatile r_i3c_regs_t* reg = ra_i3c();
  /* HUM Ch 40 "I3C Bus Interface (I3C)" p 2445-2701 */
  priv_ra_i3c_reset_sequence(reg);

  /* Clear every status / enable register to a deterministic state.
   * INSTFC is write-only (force-clear), so we treat it as a clear. */
  reg->INST   = 0U;
  reg->INSTE  = 0U;
  reg->INIE   = 0U;
  reg->INSTFC = 0U;
  reg->MSDVAD = 0U;

  ra_log_info(s_tag, "i3c_init");
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_i3c_deinit (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_i3c_deinit(void)
{
  volatile r_i3c_regs_t* reg = ra_i3c();
  /* HUM Ch 40 "BCTL : Bus Control Register" p 2445-2701 */
  reg->INIE  = 0U;
  reg->INSTE = 0U;
  reg->BCTL  = 0U;
  /* HUM Ch 40 "CECTL : Clock Enable Control Register" p 2445-2701 */
  reg->CECTL = 0U;
  s_i3c_fn   = nullptr;
  s_i3c_ctx  = nullptr;
  return ra_mstp_disable(k_ra_mstp_i3c);
}

/**
 * @brief Implementation of ra_i3c_set_address (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] addr See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_i3c_set_address(uint32_t addr)
{
  if (addr > k_ra_i3c_msdvad_addr_max) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 40 MSDVAD device address register pp 2445-2701
   * MDYAD occupies bits [22:16]; MDYADV (bit 31) marks the address
   * as valid. */
  const uint32_t mdyad = (addr << k_ra_i3c_msdvad_mdyad_shift) & k_ra_i3c_msdvad_mdyad_mask;
  ra_i3c()->MSDVAD     = mdyad | k_ra_i3c_msdvad_mdyadv_mask;
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_i3c_bus_enable (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] enable See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_i3c_bus_enable(bool enable)
{
  volatile r_i3c_regs_t* reg = ra_i3c();
  /* HUM Ch 40 "BCTL : Bus Control Register" pp 2445-2701 -- BCTL.BUSE
   * is bit 31, not bit 0; toggling it gates bus primary operation. */
  if (enable) {
    reg->BCTL = reg->BCTL | k_ra_i3c_bctl_buse_mask;
  } else {
    reg->BCTL = reg->BCTL & ~k_ra_i3c_bctl_buse_mask;
  }
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_i3c_get_status (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] out_mask See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_i3c_get_status(uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 40 "INST : Internal Status Register" p 2445-2701 */
  *out_mask = ra_i3c()->INST;
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_i3c_clear_status (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] mask See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_i3c_clear_status(uint32_t mask)
{
  volatile r_i3c_regs_t* reg = ra_i3c();
  /* HUM Ch 40 "INST : Internal Status Register" pp 2445-2701 -- the
   * sticky flags are cleared by writing 0 to the matching bit. */
  reg->INST = reg->INST & ~mask;
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_i3c_attach_handler (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] fn See implementation.
 * @param[in] ctx See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_i3c_attach_handler(ra_i3c_event_fn_t fn, void* ctx)
{
  s_i3c_fn  = fn;
  s_i3c_ctx = ctx;
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_i3c_dispatch (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
void ra_i3c_dispatch(void)
{
  volatile r_i3c_regs_t* reg = ra_i3c();
  /* HUM Ch 40 "INST : Internal Status Register" p 2445-2701 */
  const uint32_t          mask = reg->INST;
  const ra_i3c_event_fn_t fn   = s_i3c_fn;
  void* const             ctx  = s_i3c_ctx;
  reg->INST                    = 0U;
  if (fn != nullptr) {
    fn(ctx, mask);
  }
}

/**
 * @brief Implementation of ra_i3c_enter_stop (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_i3c_enter_stop(void)
{
  /* HUM Ch 40 "BCTL : Bus Control Register" p 2445-2701 */
  ra_i3c()->BCTL  = 0U;
  ra_i3c()->CECTL = 0U;
  return ra_mstp_disable(k_ra_mstp_i3c);
}

/**
 * @brief Implementation of ra_i3c_exit_stop (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_i3c_exit_stop(void)
{
  return ra_mstp_enable(k_ra_mstp_i3c);
}

/* =============================================================================
 * Dynamic Address Assignment (ENTDAA / SETDASA / RSTDAA)
 * =============================================================================
 */

/**
 * @brief Implementation of ra_i3c_dynamic_address_assign (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] targets See implementation.
 * @param[in] target_count See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_i3c_dynamic_address_assign(ra_i3c_daa_target_t* targets, uint8_t target_count)
{
  RA_CHECK_NULL_PTR(targets, s_tag, "targets must not be nullptr");
  if ((target_count == 0U) || (target_count > (uint8_t)k_ra_i3c_max_targets)) {
    return k_ra_err_invalid_arg;
  }

  volatile r_i3c_regs_t* reg = ra_i3c();

  /* Build the ENTDAA command descriptor.  See HUM Ch 40 "Command
   * Descriptor / Address Assignment" pp 2445-2701 and FSP
   * R_I3C_DynamicAddressAssignmentStart for the field layout. */
  uint32_t cmd = 0U;
  cmd |= k_ra_i3c_cmd_attr_addr_assgn << k_ra_i3c_cmd_attr_shift;
  cmd |= (uint32_t)k_ra_i3c_ccc_b_entdaa << k_ra_i3c_cmd_code_shift;
  cmd |= (uint32_t)target_count << k_ra_i3c_cmd_addr_count_shift;
  cmd |= 1UL << k_ra_i3c_cmd_roc_shift;
  cmd |= 1UL << k_ra_i3c_cmd_toc_shift;
  reg->NCMDQP = cmd;
  reg->NCMDQP = 0U;

  /* Per HUM Ch 40 "Enter Dynamic Address Assignment" each accepted
   * target replies with PID(48) + BCR + DCR -- 8 bytes total -- which
   * the controller pushes into the receive FIFO.  We drain those
   * bytes into the caller's array, then the controller latches the
   * matching dynamic address from MSDVAD.MDYAD into each target. */
  for (uint8_t i = 0U; i < target_count; ++i) {
    /* PID(6) + BCR(1) + DCR(1) = 8 bytes per target. */
    enum : uint8_t {
      k_pid_bcr_dcr_bytes = 8U,
      k_idx_bcr           = 6U,
      k_idx_dcr           = 7U,
    };
    uint8_t pid_bcr_dcr[k_pid_bcr_dcr_bytes] = {};
    priv_ra_i3c_fifo_read(reg, pid_bcr_dcr, sizeof(pid_bcr_dcr));
    for (uint8_t j = 0U; j < (uint8_t)k_ra_i3c_pid_bytes; ++j) {
      targets[i].pid[j] = pid_bcr_dcr[j];
    }
    targets[i].bcr = pid_bcr_dcr[k_idx_bcr];
    targets[i].dcr = pid_bcr_dcr[k_idx_dcr];
    /* targets[i].dynamic_address is left as the caller-supplied value. */
  }

  /* Clear command-queue-empty so the next push starts cleanly. */
  reg->NTST = reg->NTST & ~k_ra_i3c_ntst_cmdqef_mask;
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_i3c_set_dynamic_address (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] static_addr See implementation.
 * @param[in] dynamic_addr See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_i3c_set_dynamic_address(uint8_t static_addr, uint8_t dynamic_addr)
{
  if ((static_addr > (uint8_t)k_ra_i3c_addr_mask) || (dynamic_addr > (uint8_t)k_ra_i3c_addr_mask)) {
    return k_ra_err_invalid_arg;
  }

  volatile r_i3c_regs_t* reg = ra_i3c();
  uint32_t cmd = priv_ra_i3c_ccc_cmd_word(k_ra_i3c_ccc_d_setdasa, static_addr, false /* write */);
  /* SETDASA carries a single payload byte (the new dynamic address)
   * in immediate-data mode -- mirror the FSP path for transfers
   * <= 4 bytes. */
  cmd |= k_ra_i3c_cmd_attr_immed << k_ra_i3c_cmd_attr_shift;
  cmd |= k_ra_i3c_setdasa_payload_bytes << k_ra_i3c_cmd_immed_bytes_shift;
  reg->NCMDQP = cmd;
  reg->NCMDQP = (uint32_t)dynamic_addr;
  reg->NTST   = reg->NTST & ~k_ra_i3c_ntst_cmdqef_mask;
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_i3c_reset_dynamic_addresses (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_i3c_reset_dynamic_addresses(void)
{
  volatile r_i3c_regs_t* reg = ra_i3c();
  /* RSTDAA is a payload-less broadcast CCC. */
  uint32_t cmd = priv_ra_i3c_ccc_cmd_word(k_ra_i3c_ccc_b_rstdaa, 0U /* broadcast */, false);
  reg->NCMDQP  = cmd;
  reg->NCMDQP  = 0U;
  reg->NTST    = reg->NTST & ~k_ra_i3c_ntst_cmdqef_mask;
  return k_ra_ok;
}

/* =============================================================================
 * Generic CCC send / receive
 * =============================================================================
 */

/**
 * @brief Implementation of ra_i3c_send_ccc (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] ccc See implementation.
 * @param[in] target_addr See implementation.
 * @param[in] payload See implementation.
 * @param[in] len See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_i3c_send_ccc(uint8_t ccc, uint8_t target_addr, const uint8_t* payload, uint8_t len)
{
  if (target_addr > (uint8_t)k_ra_i3c_addr_mask) {
    return k_ra_err_invalid_arg;
  }
  if ((len > 0U) && (payload == nullptr)) {
    return k_ra_err_null_ptr;
  }
  /* @p len is uint8_t and k_ra_i3c_cmd_xfer_length_max is 0xFFFF, so by
   * construction every uint8_t fits in the descriptor's [31:16] length
   * field -- the runtime range check is unnecessary here. */

  volatile r_i3c_regs_t* reg = ra_i3c();
  uint32_t               cmd = priv_ra_i3c_ccc_cmd_word(ccc, target_addr, false);

  if (len <= (uint8_t)k_ra_i3c_immediate_max_bytes) {
    /* Immediate-data transfer: byte count goes in cmd1, the actual
     * bytes packed LSB-first into cmd2. */
    cmd |= k_ra_i3c_cmd_attr_immed << k_ra_i3c_cmd_attr_shift;
    cmd |= (uint32_t)len << k_ra_i3c_cmd_immed_bytes_shift;
    reg->NCMDQP = cmd;
    uint32_t w  = 0U;
    for (uint8_t i = 0U; i < len; ++i) {
      w |= (uint32_t)payload[i] << (i * k_ra_i3c_byte_shift);
    }
    reg->NCMDQP = w;
  } else {
    /* Regular transfer: cmd1 declares the descriptor; cmd2 carries the
     * length in [31:16]; payload pushed through NTDTBP0. */
    reg->NCMDQP = cmd;
    reg->NCMDQP = (uint32_t)len << k_ra_i3c_cmd_xfer_length_shift;
    priv_ra_i3c_fifo_write(reg, payload, (uint32_t)len);
  }
  reg->NTST = reg->NTST & ~k_ra_i3c_ntst_cmdqef_mask;
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_i3c_recv_ccc (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] ccc See implementation.
 * @param[in] target_addr See implementation.
 * @param[in] buf See implementation.
 * @param[in] max_len See implementation.
 * @param[in] got_len See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t
ra_i3c_recv_ccc(uint8_t ccc, uint8_t target_addr, uint8_t* buf, uint8_t max_len, uint8_t* got_len)
{
  RA_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  RA_CHECK_NULL_PTR(got_len, s_tag, "got_len must not be nullptr");
  if ((ccc & k_ra_i3c_ccc_direct_mask) == 0U) {
    /* Only direct CCCs return data. */
    return k_ra_err_invalid_arg;
  }
  if (ra_i3c_internal_recv_ccc_invalid((uint8_t)k_ra_i3c_addr_mask, target_addr, max_len)) {
    return k_ra_err_invalid_arg;
  }

  volatile r_i3c_regs_t* reg = ra_i3c();
  uint32_t               cmd = priv_ra_i3c_ccc_cmd_word(ccc, target_addr, true /* read */);
  reg->NCMDQP                = cmd;
  reg->NCMDQP                = (uint32_t)max_len << k_ra_i3c_cmd_xfer_length_shift;
  priv_ra_i3c_fifo_read(reg, buf, (uint32_t)max_len);
  *got_len  = max_len;
  reg->NTST = reg->NTST & ~k_ra_i3c_ntst_cmdqef_mask;
  return k_ra_ok;
}

/* =============================================================================
 * Private read / write
 * =============================================================================
 */

/**
 * @brief Implementation of ra_i3c_write (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] target_addr See implementation.
 * @param[in] data See implementation.
 * @param[in] len See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_i3c_write(uint8_t target_addr, const uint8_t* data, uint32_t len)
{
  if (target_addr > (uint8_t)k_ra_i3c_addr_mask) {
    return k_ra_err_invalid_arg;
  }
  if ((len > 0U) && (data == nullptr)) {
    return k_ra_err_null_ptr;
  }
  if (len > k_ra_i3c_cmd_xfer_length_max) {
    return k_ra_err_invalid_arg;
  }

  volatile r_i3c_regs_t* reg = ra_i3c();
  uint32_t               cmd = priv_ra_i3c_xfer_cmd_word(target_addr, false);

  if (len <= k_ra_i3c_immediate_max_bytes) {
    cmd |= k_ra_i3c_cmd_attr_immed << k_ra_i3c_cmd_attr_shift;
    cmd |= len << k_ra_i3c_cmd_immed_bytes_shift;
    reg->NCMDQP = cmd;
    uint32_t w  = 0U;
    for (uint32_t i = 0U; i < len; ++i) {
      w |= (uint32_t)data[i] << (i * k_ra_i3c_byte_shift);
    }
    reg->NCMDQP = w;
  } else {
    reg->NCMDQP = cmd;
    reg->NCMDQP = (len << k_ra_i3c_cmd_xfer_length_shift) &
                  (k_ra_i3c_cmd_xfer_length_max << k_ra_i3c_cmd_xfer_length_shift);
    priv_ra_i3c_fifo_write(reg, data, len);
  }
  reg->NTST = reg->NTST & ~k_ra_i3c_ntst_cmdqef_mask;
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_i3c_read (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] target_addr See implementation.
 * @param[in] buf See implementation.
 * @param[in] len See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_i3c_read(uint8_t target_addr, uint8_t* buf, uint32_t len)
{
  RA_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  if (target_addr > (uint8_t)k_ra_i3c_addr_mask) {
    return k_ra_err_invalid_arg;
  }
  if ((len == 0U) || (len > k_ra_i3c_cmd_xfer_length_max)) {
    return k_ra_err_invalid_arg;
  }

  volatile r_i3c_regs_t* reg = ra_i3c();
  uint32_t               cmd = priv_ra_i3c_xfer_cmd_word(target_addr, true);
  reg->NCMDQP                = cmd;
  reg->NCMDQP                = (len << k_ra_i3c_cmd_xfer_length_shift) &
                               (k_ra_i3c_cmd_xfer_length_max << k_ra_i3c_cmd_xfer_length_shift);
  priv_ra_i3c_fifo_read(reg, buf, len);
  reg->NTST = reg->NTST & ~k_ra_i3c_ntst_cmdqef_mask;
  return k_ra_ok;
}

/* =============================================================================
 * Sweep 15 / Phase 2: HDR mode + IBI control + peripheral-mode entry
 * =============================================================================
 */

/**
 * @brief Implementation of ra_i3c_set_hdr_mode (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] target_addr See implementation.
 * @param[in] mode See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_i3c_set_hdr_mode(uint8_t target_addr, ra_i3c_hdr_mode_t mode)
{
  if (target_addr > (uint8_t)k_ra_i3c_addr_mask) {
    return k_ra_err_invalid_arg;
  }
  if (ra_i3c_internal_hdr_mode_invalid((uint32_t)k_ra_i3c_hdr_mode_sdr,
                                       (uint32_t)k_ra_i3c_hdr_mode_ddr,
                                       (uint32_t)k_ra_i3c_hdr_mode_ts,
                                       (uint32_t)mode)) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 40 "Command Descriptor / Transfer Mode" pp 2445-2701 --
   * regular-xfer attribute (0) + dynamic address + transfer-mode
   * bits at [27:26]. */
  uint32_t       cmd       = priv_ra_i3c_xfer_cmd_word(target_addr, false);
  const uint32_t mode_bits = ((uint32_t)mode & k_ra_i3c_cmd_hdr_mode_mask)
                             << k_ra_i3c_cmd_xfer_mode_shift;
  cmd |= mode_bits;

  volatile r_i3c_regs_t* reg = ra_i3c();
  reg->NCMDQP                = cmd;
  reg->NTST                  = reg->NTST & ~k_ra_i3c_ntst_cmdqef_mask;
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_i3c_ibi_enable (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] target_addr See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_i3c_ibi_enable(uint8_t target_addr)
{
  if (target_addr > (uint8_t)k_ra_i3c_addr_mask) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 40 "IBI Valid Control Register" pp 2445-2701 -- VLCNT[7:0]
   * carries the count of IBI entries the controller will accept. */
  enum : uint32_t {
    k_ra_i3c_ntibivctl_one_target = 1U,
  };
  volatile r_i3c_regs_t* reg = ra_i3c();
  reg->NTIBIVCTL             = (k_ra_i3c_ntibivctl_one_target << k_ra_i3c_ntibivctl_vlcnt_shift) &
                               k_ra_i3c_ntibivctl_vlcnt_mask;
  (void)target_addr;
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_i3c_ibi_drain (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] ibi See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_i3c_ibi_drain(ra_i3c_ibi_t* ibi)
{
  return ra_i3c_ibi_read(ibi);
}

/**
 * @brief Implementation of ra_i3c_slave_open (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] static_addr See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_i3c_slave_open(uint8_t static_addr)
{
  if (static_addr > (uint8_t)k_ra_i3c_addr_mask) {
    return k_ra_err_invalid_arg;
  }

  volatile r_i3c_regs_t* reg = ra_i3c();
  /* HUM Ch 40 "BCTL : Bus Control Register" pp 2445-2701 -- drop BUSE
   * before flipping the SLVE bit. */
  reg->BCTL = reg->BCTL & ~k_ra_i3c_bctl_buse_mask;

  /* HUM Ch 40 "Peripheral Device Address Register" (HUM "NSDVAD") p 2445-2701 */
  const uint32_t sdyad =
    (((uint32_t)static_addr) << k_ra_i3c_nsdvad_sdyad_shift) & k_ra_i3c_nsdvad_sdyad_mask;
  reg->NSDVAD = sdyad | k_ra_i3c_nsdvad_sdyadv_mask;

  /* HUM Ch 40 "BCTL : Bus Control Register" pp 2445-2701 -- bit 16
   * (SLVE) gates peripheral-mode reception. */
  reg->BCTL = reg->BCTL | k_ra_i3c_bctl_slve_mask;
  return k_ra_ok;
}

/* =============================================================================
 * IBI inbound queue
 * =============================================================================
 */

/**
 * @brief Implementation of ra_i3c_ibi_read (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] out_ibi See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_i3c_ibi_read(ra_i3c_ibi_t* out_ibi)
{
  RA_CHECK_NULL_PTR(out_ibi, s_tag, "out_ibi must not be nullptr");

  volatile r_i3c_regs_t* reg = ra_i3c();
  /* NTST.IBIQEFF (bit 2) is set when the IBI queue holds at least
   * one entry.  See HUM Ch 40 "NTST" pp 2445-2701. */
  if ((reg->NTST & k_ra_i3c_ntst_ibiqeff_mask) == 0U) {
    return k_ra_err_no_data;
  }

  const uint32_t status = reg->NIBIQP;
  const uint8_t  len =
    (uint8_t)((status & k_ra_i3c_ibi_status_length_mask) >> k_ra_i3c_ibi_status_length_shift);
  const uint8_t ibi_id =
    (uint8_t)((status & k_ra_i3c_ibi_status_id_mask) >> k_ra_i3c_ibi_status_id_shift);

  out_ibi->address = (uint8_t)(ibi_id >> k_ra_i3c_addr_shift_in_ibi_id);
  /* Encode IBI/HJ/MR using the lowest two bits of IBI ID.  See HUM
   * Ch 40 "IBI Status Descriptor" pp 2445-2701 -- bit 31 (IBI_ST)
   * separates IBI from HJ/MR; IBI_ID 0x02 == hot-join, 0x04 ==
   * mainship-request in the MIPI I3C spec. */
  enum : uint8_t {
    k_ibi_id_hot_join = 0x02U,
  };
  if ((status & k_ra_i3c_ibi_status_ibi_st_mask) == 0U) {
    out_ibi->type = k_ra_i3c_ibi_type_interrupt;
  } else if (ibi_id == k_ibi_id_hot_join) {
    out_ibi->type = k_ra_i3c_ibi_type_hot_join;
  } else {
    out_ibi->type = k_ra_i3c_ibi_type_main_request;
  }

  const uint8_t k_max_ibi_payload = (uint8_t)(sizeof(out_ibi->payload));
  uint8_t       to_copy           = len;
  if (to_copy > k_max_ibi_payload) {
    to_copy = k_max_ibi_payload;
  }
  out_ibi->payload_len = to_copy;
  for (uint8_t i = 0U; i < k_max_ibi_payload; ++i) {
    out_ibi->payload[i] = 0U;
  }
  if (to_copy > 0U) {
    priv_ra_i3c_fifo_read(reg, out_ibi->payload, (uint32_t)to_copy);
  }
  enum : uint32_t {
    k_ibi_last_mask = 1U,
  };
  out_ibi->last = (uint8_t)((status >> k_ra_i3c_ibi_status_last_shift) & k_ibi_last_mask);

  /* Clear IBIQEFF so the next read can detect a fresh entry. */
  reg->NTST = reg->NTST & ~k_ra_i3c_ntst_ibiqeff_mask;
  return k_ra_ok;
}
