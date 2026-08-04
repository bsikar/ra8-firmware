/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_xspi.c
 * @brief Octal Serial Peripheral Interface (OSPI / xSPI) driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * driver for the RA8D2 OSPI block. Provides a minimal
 * SPI NOR flash driver layered on top of the xSPI manual-command
 * engine (``CDCTL0`` + ``CDBUF[]`` + ``INTS.CMDCMP``).
 *
 * This translation unit owns the block/clock/reset bring-up and the
 * lifecycle + IRQ + power + XIP/DTR/DQS-calibration + software-reset
 * surface:
 *
 * - ``ra8_xspi_init()`` -- select a protocol mode + clear pending IRQs.
 * - ``ra8_xspi_direct_command()`` -- raw CDBUF poke.
 * - ``ra8_xspi_deinit / get_status / clear_status / attach_handler /
 * enter_stop / exit_stop`` lifecycle + IRQ + power surface.
 * - ``ra8_xspi_set_xip_mode / set_dtr_mode / calibrate_dqs / suspend /
 * resume / software_reset`` extended controller surface.
 *
 * The manual-command engine and the JEDEC NOR-flash read / program /
 * erase / status / id operations live in the sibling translation unit
 * ``ra8_xspi_flash.c``; the two manual-command primitives it exports
 * (``ra8_xspi_kick_command`` / ``ra8_xspi_issue_simple_opcode``) are
 * declared in ``ra8_xspi_internal.h`` and reused here by the
 * suspend / resume / software-reset paths.
 *
 * ## CDBUF convention used by this driver
 *
 * Each ``CDBUF`` slot is 4 x 32-bit words wide. This driver writes
 * exclusively to slot 0 and encodes a manual transfer as:
 *
 * - ``CDBUF[0]`` -- opcode byte in the low 8 bits.
 * - ``CDBUF[1]`` -- flash address (3- or 4-byte).
 * - ``CDBUF[2]`` -- write-data low word (bytes 0..3).
 * - ``CDBUF[3]`` -- write-data high word (bytes 4..7).
 *
 * Read responses (status, JEDEC ID, flash-read bytes) land in
 * ``CDBUF[2]`` / ``CDBUF[3]`` after the controller clears TRREQ.
 *
 * Every build emits the identical register sequence; host unit tests
 * round-trip data through the register-level NOR-flash model in
 * ``tests/mocks/ra8_fake_xspi_flash.c``, which services each TRREQ kick
 * from the CMDCMP poll's ``ra8_fake_mmio`` seam consult (#238). Every
 * register write carries a
 * ``HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986``
 * citation comment for the cite checker.
 */

#include "ra8_xspi.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hw_err.h"
#include "ra8_log.h"
#include "ra8_mstp.h"
#include "ra8_ospi_regs.h"
#include "ra8_register_protection.h"
#include "ra8_system_regs.h"
#include "ra8_xspi_internal.h"

/** @brief Logging tag for this driver. */
static const char* s_tag = "XSPI";

/**
 * @enum ra8_xspi_cmd_limits_t
 * @brief Byte-count limits on raw direct-command buffers.
 */
typedef enum : uint8_t {
  k_ra8_xspi_cmd_max_bytes = 16U, /**< A CDBUF slot holds 16 bytes. */
} ra8_xspi_cmd_limits_t;

/**
 * @enum ra8_xspi_chip_select_t
 * @brief Which controller chip-select drives the on-board OSPI flash.
 *
 * @details
 * The xSPI controller fans every manual command out to one of two
 * chip-select devices (CS0 / CS1). Which physical CS strobe pin
 * that maps to is a board fact, not a per-call knob: on the EK-RA8D2 the
 * on-board IS25LX512M's chip-select net (OSPI_FLASH_S_L, P104) is wired
 * to the controller's CS**1** line, and the Renesas FSP EK-RA8D2 OSPI
 * configuration (``configuration.xml`` ``module.driver.ospi_b.channel``
 * = ``channel.1``) confirms the vendor drives the part on device 1. The
 * controller therefore must (a) carry the protocol/timing config in
 * ``LIOCFGCS[1]`` and (b) assert ``CDCTL0.CSSEL = 1`` on every manual
 * command, or the CS0 strobe (an unconnected pin) toggles instead, the
 * flash never sees chip-select, and RDID floats to 0x00FFFFFF on the
 * board pull-ups. HUM Ch 44 p 2986; EK-RA8D2 UM Table 29 p 35.
 */
typedef enum : uint8_t {
  k_ra8_xspi_onboard_cs = 1U, /**< IS25LX512M is on controller CS1. */
} ra8_xspi_chip_select_t;

/**
 * @enum ra8_xspi_reset_delay_t
 * @brief Bounded busy-spin counts for the LIOCTL.RSTCS reset pulse.
 *
 * @details
 * The vendor EK-RA8D2 bring-up (``ospi_flash_issi_is25lx512.c``
 * ``reset_ospi_device``) holds ``LIOCTL.RSTCS`` low for ~50 us and high
 * for ~50 us. With no SysTick dependency in this driver the wait is a
 * bounded register-free busy loop; at ~1 cycle per iteration on the
 * 1 GHz Cortex-M85 ``k_ra8_xspi_reset_spin`` iterations is comfortably
 * over the IS25LX512M tRLRH/tRHSL minima (100 ns each) while staying in
 * the tens-of-microseconds the vendor uses. IS25LX512M datasheet Ch 9.2
 * "Hardware Reset".
 */
typedef enum : uint32_t {
  k_ra8_xspi_reset_spin = 100000U, /**< ~tens of us busy-spin per reset edge. */
} ra8_xspi_reset_delay_t;

/**
 * @var s_xspi_mstp_table
 * @brief Instance-index -> MSTP id lookup. OSPI0 and OSPI1 each
 * have their own MSTPB bit (16/17) per HUM Ch 11.2.7 p 444.
 * Each bit also covers the matching DOTF channel.
 */
static const ra8_mstp_t s_xspi_mstp_table[] = {
  k_ra8_mstp_ospi0,
  k_ra8_mstp_ospi1,
};

/**
 * @brief Bounded wait on OCTACKCR.OCTACKSRDY reaching ``expected``.
 *
 * @details
 * Mirrors ``internal_wait_canfdcksrdy`` in ``ra8_canfd.c`` and
 * ``internal_wait_usbcksrdy`` in ``ra8_cgc.c``. Polls OCTACKCR bit 7
 * (OCTACKSRDY) until it equals ``expected`` or ``k_ra8_xspi_ckcr_spin``
 * iterations elapse. HUM Ch 9.2.45 "OCTACKCR" p 360 documents
 * OCTACKSRDY at bit 7 (R) -- "Possible to Switch" flag.
 *
 * @param[in] expected 1U after SREQ=1; 0U after SREQ=0.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok             SRDY observed equal to ``expected``.
 * @retval k_ra8_err_hw_timeout SRDY never matched within the budget.
 *
 * @pre Caller holds the PRCR-CGC unlock window (PRCR=0xA501).
 * @pre ``expected`` is 0 or 1.
 * @post No register state is modified -- this is a read-only poll.
 * @post On timeout the caller relocks PRCR.
 *
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_wait_octacksrdy(uint8_t expected)
{
  /* SRDY (clock-source ready) is bit 7 of OCTACKCR. */
  /* HUM Ch 9.2.45 "OCTACKCR.OCTACKSRDY" p 360 */
  volatile uint8_t* const ckcr = ra8_sys_octackcr();
  const uint8_t           mask = (uint8_t)(1U << k_ra8_usbckcr_bit_srdy);
  /* Bounded wait through ra8_hw_err.h: on host tests the ra8_fake_mmio
   * seam decides the poll (first-poll success unless a test arms a
   * fault), so the real timeout leg is reachable everywhere. */
  if (expected != 0U) {
    return ra8_hw_wait_flag_set8(ckcr, mask, (uint32_t)k_ra8_xspi_ckcr_spin);
  }
  return ra8_hw_wait_flag_clear8(ckcr, mask, (uint32_t)k_ra8_xspi_ckcr_spin);
}

/**
 * @brief Block-level OCTA clock init -- run BEFORE the first MSTP release.
 *
 * @details
 * HUM Ch 11.2.7 "MSTPCRB" Note 3 (p 444) states that MSTPB16 / MSTPB17
 * (the per-instance OSPI module-stop bits) must be written AFTER the
 * OCTACLK is stable. OCTACKCR resets to ``0x01`` (OCTACKSEL = MOCO,
 * OCTACKSREQ = 0, OCTACKSRDY = 0). MOCO is on at reset, but the RA8D2
 * CGC requires an explicit SREQ -> SRDY -> SREQ-clear handshake before
 * the chip raises OCTACKSRDY and declares the clock stable. Without
 * that handshake the OSPI manual-command engine's internal state
 * machine cannot retire ``CDCTL0.TRREQ`` after the first
 * ``CDBUF[0].CDT`` write -- symptom seen on HIL: every
 * ``ra8_xspi_flash_*`` operation surfaces as ``k_ra8_err_hw_timeout``
 * on CMDCMP, and ``flash_journal``'s ``g_fj_match`` /
 * ``g_fj_mismatch`` counters both read 0 across the 5 s memprobe
 * window.
 *
 * Steps (mirror of ``internal_canfd_clock_block_init`` in
 * ``ra8_canfd.c`` + the USBCKCR pattern in ``ra8_cgc.c``, with the
 * OCTACKCR-specific procedure from HUM Ch 9.2.45 p 360):
 *   1. Write OCTACKDIVCR = 0 (/1 -- documented reset value).
 *   2. Set OCTACKCR.OCTACKSREQ = 1 (request switch) while keeping the
 *      reset-default OCTACKSEL = MOCO.
 *   3. Poll until OCTACKSRDY = 1.
 *   4. Re-write OCTACKCR with SREQ=0, source = MOCO -- commits the
 *      (same) source selection.
 *   5. Poll until OCTACKSRDY = 0 (handshake done).
 *
 * This helper is idempotent via a static guard: only the first caller
 * performs the handshake; subsequent ``ra8_xspi_init`` calls (e.g. for
 * instance 1 after instance 0) skip it. MOCO -> /1 keeps OCTACLK at
 * its native MOCO rate (~8 MHz nominal) which is fine for the IS25LX
 * JEDEC-mode bring-up; a later board-specific tune can switch the
 * source to a PLL output by reissuing the same handshake.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok             OCTACLK declared stable; safe to release MSTP.
 * @retval k_ra8_err_hw_timeout SRDY handshake stuck.
 *
 * @pre Single-threaded init context (no other CGC writes in flight).
 * @pre MOCO is running -- chip reset default; ra8_cgc_init does not
 *      explicitly stop MOCO.
 * @post On k_ra8_ok the OCTA block clock is stable; MSTPB16/17 may now
 *       be released.
 * @post On error the OSPI MSTP gate is NOT touched.
 * @post PRCR is re-locked.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_xspi_clock_block_init(void)
{
  static bool s_xspi_clock_inited = false;
  if (s_xspi_clock_inited) {
    return k_ra8_ok;
  }
  ra8_err_t err = k_ra8_ok;
  RA8_PROTECTED_WRITE(k_ra8_prcr_unlock_cgc)
  {
    /* HUM Ch 9.2.40 "OCTACKDIVCR" p 357 -- /1 keeps MOCO at its
     * native rate (~8 MHz nominal). The IS25LX512M JEDEC bring-up
     * runs comfortably below 50 MHz so /1 is conservative. */
    *ra8_sys_octackdivcr() = 0U;

    /* HUM Ch 9.2.45 "OCTACKCR.OCTACKSREQ" p 360 -- assert SREQ with
     * the reset-default source (MOCO, OCTACKSEL = 0001b). */
    const uint8_t sreq_mask = (uint8_t)(1U << k_ra8_usbckcr_bit_sreq);
    const uint8_t src_moco  = 0x01U;
    *ra8_sys_octackcr()     = (uint8_t)(src_moco | sreq_mask);

    /* Step 3: wait for SRDY = 1 (chip acknowledges the request). */
    err = internal_wait_octacksrdy(1U);
    if (err != k_ra8_ok) {
      ra8_log_error(s_tag, "xspi: OCTACKSRDY=1 timeout");
      break;
    }
    /* Step 4: drop SREQ -- commits the (same) source selection. */
    *ra8_sys_octackcr() = src_moco;
    /* Step 5: wait for SRDY = 0 -- handshake done. */
    err = internal_wait_octacksrdy(0U);
    if (err != k_ra8_ok) {
      ra8_log_error(s_tag, "xspi: OCTACKSRDY=0 timeout");
      break;
    }
  }
  if (err == k_ra8_ok) {
    s_xspi_clock_inited = true;
    ra8_log_info(s_tag, "octa block clock stable");
  }
  return err;
}

/**
 * @brief Bounded busy-spin used between the RSTCS reset edges.
 *
 * @details
 * Register-free delay (no SysTick dependency from this HAL driver).
 * ``volatile`` counter so the optimiser cannot elide the loop. Sized by
 * ``k_ra8_xspi_reset_spin`` to span the IS25LX512M tRLRH/tRHSL minima.
 * IS25LX512M datasheet Ch 9.2 "Hardware Reset".
 *
 * @return None.
 * @pre Called only from the single-threaded xSPI init path.
 * @pre No interrupt depends on this delay being preempted.
 * @post At least ``k_ra8_xspi_reset_spin`` iterations have elapsed.
 * @post No registers or shared state are modified.
 * @note Runs on every build; the host burns the same bounded loop so no
 *       code path is compiled out of the coverage build.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_xspi_reset_spin(void)
{
  for (volatile uint32_t i = 0U; i < (uint32_t)k_ra8_xspi_reset_spin; i++) {
    /* spin */
  }
}

/**
 * @brief Pulse the controller-driven flash RESET (LIOCTL.RSTCS) low->high.
 *
 * @details
 * Mirror of the vendor EK-RA8D2 bring-up (``ospi_b_ep.c`` ``ospi_b_init``
 * and ``ospi_flash_issi_is25lx512.c`` ``reset_ospi_device``): after the
 * controller protocol/timing config is in place, drive ``LIOCTL.RSTCS``
 * low, wait, then high, wait. This issues a hardware reset to the
 * selected chip-select's flash through the controller's RESET output,
 * returning a device that a prior loader may have left in OPI/DOPI back
 * to its power-on 1S SPI protocol. WPCS is held high (write-protect
 * deasserted) throughout. HUM Ch 44 p 2986.
 *
 * @param[in] reg xSPI register block (already gated open by the caller).
 *
 * @return None.
 * @pre ``reg != nullptr`` and the xSPI MSTP gate is open.
 * @pre The link-IO protocol/timing config has already been written.
 * @post The on-board flash has seen a RESET low->high pulse and RSTCS is
 *       left deasserted (1).
 * @post WPCS remains high (write-protect deasserted).
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_xspi_reset_device(volatile r_xspi_regs_t* reg)
{
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* WPCS held high (write-protect deasserted) the whole time; only
   * RSTCS toggles. */
  reg->LIOCTL = k_ra8_xspi_lioctl_mask_wpcs; /* RSTCS = 0 (asserted, reset) */
  internal_xspi_reset_spin();
  reg->LIOCTL = k_ra8_xspi_lioctl_mask_wpcs | k_ra8_xspi_lioctl_mask_rstcs; /* RSTCS = 1 */
  internal_xspi_reset_spin();
}

/**
 * @brief Apply the manual-command controller config for the on-board flash.
 *
 * @details
 * Wakes the wrapper, idles the common/XiP config, writes the link-IO
 * protocol mode into the on-board chip-select's ``LIOCFGCS`` slot, and
 * points the manual-command engine at that CS via ``CDCTL0.CSSEL``.
 * ``BMCTL0`` is forced disabled so the AHB system-bus path cannot race the
 * manual-command engine (FSP gates this via ``r_ospi_b_xip(false)``);
 * leaving it enabled lets the controller NAK ``CDCTL0.TRREQ`` and time out
 * CMDCMP. ``CMCTLCH[0/1]`` are zeroed so XIPEN is not left armed. The CS is
 * ``k_ra8_xspi_onboard_cs`` (CS1): the EK-RA8D2 IS25LX512M is wired there
 * (FSP OSPI example ``channel == 1``); an earlier CS0 default strobed an
 * unconnected pin and floated RDID to 0x00FFFFFF (issue #44).
 * HUM Ch 44 p 2986.
 *
 * @param[in] reg  xSPI register block (already gated open by the caller).
 * @param[in] mode Link-IO protocol/latency word for ``LIOCFGCS``.
 *
 * @return None.
 * @pre ``reg != nullptr`` and the xSPI MSTP gate is open.
 * @pre OCTACLK is stable (clock-block handshake done).
 * @post ``LIOCFGCS[k_ra8_xspi_onboard_cs] == mode`` and ``CDCTL0.CSSEL``
 *       selects that CS; latent interrupt flags are cleared.
 * @post ``BMCTL0`` is disabled (manual-command path owns the bus).
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_xspi_apply_config(volatile r_xspi_regs_t* reg, ra8_xspi_lio_mode_t mode)
{
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  reg->BMCTL0                          = (uint32_t)k_ra8_xspi_bmctl0_disabled;
  reg->CMCTLCH[0]                      = 0U;
  reg->CMCTLCH[1]                      = 0U;
  reg->WRAPCFG                         = 0U;
  reg->COMCFG                          = 0U;
  reg->LIOCFGCS[k_ra8_xspi_onboard_cs] = (uint32_t)mode;
  reg->CDCTL0 =
    ((uint32_t)k_ra8_xspi_onboard_cs << k_ra8_xspi_cdctl0_bit_cssel) & k_ra8_xspi_cdctl0_mask_cssel;
  reg->INTC = k_ra8_xspi_ints_mask_all;
}

ra8_err_t ra8_xspi_init(uint8_t instance, ra8_xspi_lio_mode_t mode)
{
  volatile r_xspi_regs_t* reg = ra8_xspi(instance);
  RA8_CHECK_NULL_PTR(reg, s_tag, "instance out of range");
  if (instance >= (uint8_t)(sizeof(s_xspi_mstp_table) / sizeof(s_xspi_mstp_table[0]))) {
    return k_ra8_err_invalid_arg;
  }

  /* HUM Ch 11.2.7 "MSTPCRB" Note 3 p 444 -- MSTPB16/17 must be written
   * AFTER OCTACLK is stable. Run the block-level OCTACKCR handshake
   * first; the helper is idempotent so a second ra8_xspi_init call
   * (e.g. for instance 1) skips it. */
  const ra8_err_t clk_err = internal_xspi_clock_block_init();
  if (clk_err != k_ra8_ok) {
    return clk_err;
  }

  /* HUM Ch 11.2.7 "MSTPCRB : Module Stop Control Register B", p 444 */
  const ra8_err_t mst_err = ra8_mstp_enable(s_xspi_mstp_table[instance]);
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "xspi_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  internal_xspi_apply_config(reg, mode);
  internal_xspi_reset_device(reg);

  ra8_log_info_val(s_tag, "xspi_init inst", (uint32_t)instance);
  return k_ra8_ok;
}

ra8_err_t ra8_xspi_direct_command(uint8_t instance, const uint8_t* cmd_buf, uint8_t len)
{
  RA8_CHECK_NULL_PTR(cmd_buf, s_tag, "cmd_buf must not be nullptr");
  if (len > k_ra8_xspi_cmd_max_bytes) {
    return k_ra8_err_invalid_size;
  }
  volatile r_xspi_regs_t* reg = ra8_xspi(instance);
  if (reg == nullptr) {
    return k_ra8_err_out_of_range;
  }

  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* Pack the caller's bytes into CDBUF slot 0 as little-endian
   * words. Slot 0 is the full 16-byte CDBUF window (4 u32), so
   * raw callers can use any of the four indices. */
  uint32_t word = 0U;
  for (uint8_t i = 0U; i < len; i++) {
    word |= ((uint32_t)cmd_buf[i]) << ((i % 4U) * 8U);
    if ((i % 4U) == 3U) {
      reg->CDBUF[i / 4U] = word;
      word               = 0U;
    }
  }
  if ((len % 4U) != 0U) {
    reg->CDBUF[len / 4U] = word;
  }

  return k_ra8_ok;
}

/* =============================================================================
 * lifecycle + IRQ + power transition
 * =============================================================================
 */

/**
 * @struct ra8_xspi_state_t
 * @brief Per-instance callback state.
 */
typedef struct {
  ra8_xspi_event_fn_t fn;  /**< User-supplied completion callback. */
  void*               ctx; /**< Opaque context pointer for ``fn``. */
} ra8_xspi_state_t;

/** @brief Per-instance callback state (s_ prefix for file-static). */
static ra8_xspi_state_t s_xspi_state[k_ra8_xspi_instance_count];

ra8_err_t ra8_xspi_deinit(uint8_t instance)
{
  volatile r_xspi_regs_t* reg = ra8_xspi(instance);
  RA8_CHECK_NULL_PTR(reg, s_tag, "instance out of range");

  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  reg->LIOCFGCS[0] = 0U;
  reg->INTE        = 0U;
  reg->INTC        = k_ra8_xspi_ints_mask_all;

  s_xspi_state[instance].fn  = nullptr;
  s_xspi_state[instance].ctx = nullptr;
  return ra8_mstp_disable(s_xspi_mstp_table[instance]);
}

ra8_err_t ra8_xspi_get_status(uint8_t instance, uint32_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  volatile r_xspi_regs_t* reg = ra8_xspi(instance);
  RA8_CHECK_NULL_PTR(reg, s_tag, "instance out of range");
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  *out_mask = reg->COMSTT;
  return k_ra8_ok;
}

ra8_err_t ra8_xspi_clear_status(uint8_t instance, uint32_t mask)
{
  volatile r_xspi_regs_t* reg = ra8_xspi(instance);
  RA8_CHECK_NULL_PTR(reg, s_tag, "instance out of range");
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  reg->INTC = mask;
  return k_ra8_ok;
}

ra8_err_t ra8_xspi_attach_handler(uint8_t instance, ra8_xspi_event_fn_t fn, void* ctx)
{
  if (instance >= k_ra8_xspi_instance_count) {
    return k_ra8_err_invalid_arg;
  }
  s_xspi_state[instance].fn  = fn;
  s_xspi_state[instance].ctx = ctx;
  return k_ra8_ok;
}

RA8_ISR_SAFE
void ra8_xspi_dispatch(uint8_t instance)
{
  if (instance >= k_ra8_xspi_instance_count) {
    return;
  }
  volatile r_xspi_regs_t* reg = ra8_xspi(instance);
  if (reg == nullptr) { /* GCOVR_EXCL_BR_LINE -- instance bounded above */
    return;             /* GCOVR_EXCL_LINE                              */
  }
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* Snapshot INTS + COMSTT, clear every pending interrupt flag,
   * then hand the INTS mask off to the user callback so it can
   * decide whether the transfer was successful or errored. */
  const uint32_t mask = reg->INTS;
  reg->INTC           = k_ra8_xspi_ints_mask_all;

  const ra8_xspi_event_fn_t fn  = s_xspi_state[instance].fn;
  void* const               ctx = s_xspi_state[instance].ctx;
  if (fn != nullptr) {
    fn(ctx, mask);
  }
}

ra8_err_t ra8_xspi_enter_stop(uint8_t instance)
{
  if (instance >= k_ra8_xspi_instance_count) {
    return k_ra8_err_invalid_arg;
  }
  return ra8_mstp_disable(s_xspi_mstp_table[instance]);
}

ra8_err_t ra8_xspi_exit_stop(uint8_t instance)
{
  if (instance >= k_ra8_xspi_instance_count) {
    return k_ra8_err_invalid_arg;
  }
  return ra8_mstp_enable(s_xspi_mstp_table[instance]);
}

ra8_err_t ra8_xspi_xip_enter(uint8_t instance, uint8_t enter_code, uint8_t exit_code)
{
  volatile r_xspi_regs_t* reg = ra8_xspi(instance);
  RA8_CHECK_NULL_PTR(reg, s_tag, "instance out of range");

  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* FSP r_ospi_b_xip(true) flow:
   *   1. Stage XIP enter/exit codes in CMCTLCH for both channels.
   *   2. Map the target window read-only via BMCTL0 = 0x55.
   *   3. Set CMCTLCH.XIPEN to arm execute-in-place.
   * The first read on the memory-mapped window then transmits the
   * enter code. We omit the bus-bridge prefetch dance because the
   * driver does not enable prefetch by default. */
  const uint32_t code_word = ((uint32_t)enter_code << k_ra8_xspi_cmctlch_xipencode_pos) |
                             ((uint32_t)exit_code << k_ra8_xspi_cmctlch_xipexcode_pos);

  reg->BMCTL0     = k_ra8_xspi_bmctl0_read_only;
  reg->CMCTLCH[0] = code_word | k_ra8_xspi_cmctlch_xipen_mask;
  reg->CMCTLCH[1] = code_word | k_ra8_xspi_cmctlch_xipen_mask;
  return k_ra8_ok;
}

ra8_err_t ra8_xspi_xip_exit(uint8_t instance)
{
  volatile r_xspi_regs_t* reg = ra8_xspi(instance);
  RA8_CHECK_NULL_PTR(reg, s_tag, "instance out of range");

  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* FSP r_ospi_b_xip(false) flow: clear XIPEN, drop the codes,
   * and put BMCTL0 back to read/write so direct-command transfers
   * are no longer blocked by the memory-mapped path. */
  reg->CMCTLCH[0] = 0U;
  reg->CMCTLCH[1] = 0U;
  reg->BMCTL0     = k_ra8_xspi_bmctl0_read_write;
  return k_ra8_ok;
}

/* =============================================================================
 * Sweep 6 extensions: XIP mode select, DTR, DQS calibration, suspend/resume
 * =============================================================================
 */

/**
 * @enum ra8_xspi_jedec_extra_t
 * @brief Extra JEDEC opcodes used by suspend/resume control.
 */
typedef enum : uint8_t {
  k_ra8_spi_flash_op_suspend   = 0x75U, /**< 0x75 erase/program suspend.             */
  k_ra8_spi_flash_op_resume    = 0x7AU, /**< 0x7A erase/program resume.              */
  k_ra8_spi_flash_op_reset_en  = 0x66U, /**< 0x66 RSTEN  -- IS25LX512M Ch 8.20 p 39. */
  k_ra8_spi_flash_op_reset_dev = 0x99U, /**< 0x99 RST    -- IS25LX512M Ch 8.21 p 39. */
} ra8_xspi_jedec_extra_t;

/**
 * @enum ra8_xspi_reset_cmd_bytes_t
 * @brief Allowed ``cmd_bytes`` values for ``ra8_xspi_software_reset``.
 *
 * @details
 * 1-byte opcodes are used in 1S-1S-1S extended SPI mode, 2-byte
 * opcodes (``opcode | (~opcode << 8)``) in 8D-8D-8D OPI/DDR mode.
 * Cite: IS25LX512M datasheet Ch 7.3 "Operating Protocols" p 27.
 */
typedef enum : uint8_t {
  k_ra8_xspi_reset_cmd_bytes_1s = 1U, /**< 1-byte opcode for 1S-1S-1S.      */
  k_ra8_xspi_reset_cmd_bytes_8d = 2U, /**< 2-byte opcode pair for 8D-8D-8D. */
} ra8_xspi_reset_cmd_bytes_t;

/**
 * @enum ra8_xspi_addr_bytes_t
 * @brief Allowed address-byte widths for ``ra8_xspi_set_xip_mode``.
 */
typedef enum : uint8_t {
  k_ra8_xspi_addr_bytes_3 = 3U, /**< 24-bit JEDEC address. */
  k_ra8_xspi_addr_bytes_4 = 4U, /**< 32-bit JEDEC address. */
} ra8_xspi_addr_bytes_t;

/**
 * @enum ra8_xspi_calib_spin_t
 * @brief Bounded spin budget for the auto-calibration handshake.
 */
typedef enum : uint32_t {
  k_ra8_xspi_calib_spin = 1024U, /**< CCCTL0.CAEN poll budget. */
} ra8_xspi_calib_spin_t;

ra8_err_t ra8_xspi_set_xip_mode(uint8_t instance, bool enable, uint8_t read_cmd, uint8_t addr_bytes)
{
  volatile r_xspi_regs_t* reg = ra8_xspi(instance);
  RA8_CHECK_NULL_PTR(reg, s_tag, "instance out of range");
  if ((addr_bytes != k_ra8_xspi_addr_bytes_3) && (addr_bytes != k_ra8_xspi_addr_bytes_4)) {
    return k_ra8_err_invalid_arg;
  }

  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* CMCFGCS slot 0 = (mode, read-cmd-word, write-cmd-word, addr-word).
   * For XIP we only need read + addr; write opcode stays zero. */
  const uint8_t base                                    = 0U; /* slot 0 base index */
  reg->CMCFGCS[base + k_ra8_xspi_cmcfgcs_word_read_cmd] = (uint32_t)read_cmd
                                                          << k_ra8_xspi_cmcfgcs_pos_cmd;
  reg->CMCFGCS[base + k_ra8_xspi_cmcfgcs_word_addr]     = (uint32_t)addr_bytes
                                                          << k_ra8_xspi_cmcfgcs_pos_addr_size;

  if (enable) {
    /* Mirror FSP r_ospi_b_xip(true): map read-only and arm XIPEN. */
    reg->BMCTL0     = k_ra8_xspi_bmctl0_read_only;
    reg->CMCTLCH[0] = k_ra8_xspi_cmctlch_xipen_mask;
    reg->CMCTLCH[1] = k_ra8_xspi_cmctlch_xipen_mask;
  } else {
    /* FSP r_ospi_b_xip(false): clear XIPEN and re-open the bus. */
    reg->CMCTLCH[0] = 0U;
    reg->CMCTLCH[1] = 0U;
    reg->BMCTL0     = k_ra8_xspi_bmctl0_read_write;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_xspi_set_dtr_mode(uint8_t instance, bool enable)
{
  volatile r_xspi_regs_t* reg = ra8_xspi(instance);
  RA8_CHECK_NULL_PTR(reg, s_tag, "instance out of range");

  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  uint32_t v = reg->LIOCFGCS[0];
  if (enable) {
    v |= k_ra8_xspi_liocfgcs_mask_ddren;
  } else {
    v &= ~k_ra8_xspi_liocfgcs_mask_ddren;
  }
  reg->LIOCFGCS[0] = v;
  return k_ra8_ok;
}

ra8_err_t ra8_xspi_calibrate_dqs(uint8_t instance)
{
  volatile r_xspi_regs_t* reg = ra8_xspi(instance);
  RA8_CHECK_NULL_PTR(reg, s_tag, "instance out of range");

  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* Mirror FSP R_OSPI_B_AutoCalibrate: arm CAEN and wait for the
   * controller to clear it once the phase-scan completes. The full
   * preamble-pattern + CARDCMD descriptor is owned by board-level
   * code in higher-level callers. On host tests the bounded wait
   * consults the ra8_fake_mmio seam (first-poll success unless a test
   * arms a fault); the CAEN bit itself stays set in host RAM because
   * only the real controller clears it. */
  reg->CCCTLCS[0] |= k_ra8_xspi_ccctl0_mask_caen;
  return ra8_hw_wait_flag_clear32(&reg->CCCTLCS[0],
                                  (uint32_t)k_ra8_xspi_ccctl0_mask_caen,
                                  (uint32_t)k_ra8_xspi_calib_spin);
}

ra8_err_t ra8_xspi_suspend(uint8_t instance)
{
  volatile r_xspi_regs_t* reg = ra8_xspi(instance);
  RA8_CHECK_NULL_PTR(reg, s_tag, "instance out of range");
  return ra8_xspi_issue_simple_opcode(reg, k_ra8_spi_flash_op_suspend);
}

ra8_err_t ra8_xspi_resume(uint8_t instance)
{
  volatile r_xspi_regs_t* reg = ra8_xspi(instance);
  RA8_CHECK_NULL_PTR(reg, s_tag, "instance out of range");
  return ra8_xspi_issue_simple_opcode(reg, k_ra8_spi_flash_op_resume);
}

/**
 * @brief Issue a single RSTEN-or-RST opcode in either 1S or 8D form.
 *
 * @details
 * In 1S-1S-1S mode the opcode is a single byte placed at CDT.CMD bits
 * [31..16] with CMDSIZE=1. In 8D-8D-8D mode the chip expects the
 * opcode followed by its bitwise complement so the controller has to
 * ship two bytes; we encode that as ``opcode | (~opcode << 8)`` in
 * the same CMD field with CMDSIZE=2. The CDT.CMD field is 16 bits
 * wide ([31..16]) so both forms fit cleanly. No address phase, no
 * data phase. IS25LX512M datasheet Ch 7.3 p 27 ("Operating Protocols")
 * + Ch 8.20/8.21 p 39 (RSTEN/RST opcodes); HUM Ch 44 p 2986 for the
 * CDT layout.
 *
 * @param[in] reg       xSPI register block.
 * @param[in] opcode    JEDEC reset opcode (0x66 RSTEN or 0x99 RST).
 * @param[in] cmd_bytes 1 (1S) or 2 (8D); chosen by the caller based
 *                      on the protocol mode the chip *might* be in.
 *
 * @return ``k_ra8_ok`` on success, ``k_ra8_err_hw_timeout`` on CMDCMP
 *         timeout.
 * @retval k_ra8_ok Operation completed successfully.
 * @retval other Non-zero error code from the underlying operation.
 * @pre Module/state preconditions hold (see function body).
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 * @post Documented side effects are visible on success.
 * @note Not thread-safe; the caller must serialise concurrent access.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_issue_reset_opcode(volatile r_xspi_regs_t* reg, uint8_t opcode, uint8_t cmd_bytes)
{
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* Build the CMD half-word for either 1S (just the opcode) or 8D
   * (opcode + complement). The complement form is what 8D-mode SPI
   * NOR devices require so they can distinguish "real opcode" from
   * "garbage on the bus". */
  /* CMD is transmitted MSB-first from CDT[31:16]; a 1-byte opcode must
   * be left-justified to CDT[31:24] (cmd_word << 8), a 2-byte 8D pair
   * fills the full [31:16] field. Same rule as internal_make_cdt. */
  uint16_t cmd_word = (uint16_t)(opcode << 8U);
  if (cmd_bytes == (uint8_t)k_ra8_xspi_reset_cmd_bytes_8d) {
    cmd_word = (uint16_t)(opcode | (((uint16_t)(uint8_t)~opcode) << 8U));
  }
  reg->CDBUF[k_ra8_xspi_cdbuf_idx_cdt] =
    (((uint32_t)cmd_bytes & k_ra8_xspi_cdt_mask_cmdsize) << k_ra8_xspi_cdt_pos_cmdsize) |
    (((uint32_t)k_ra8_xspi_cdt_addsize_0 & k_ra8_xspi_cdt_mask_addsize)
     << k_ra8_xspi_cdt_pos_addsize) |
    (((uint32_t)k_ra8_xspi_cdt_trtype_write & k_ra8_xspi_cdt_mask_trtype)
     << k_ra8_xspi_cdt_pos_trtype) |
    (((uint32_t)cmd_word) << k_ra8_xspi_cdt_pos_cmd);
  reg->CDBUF[k_ra8_xspi_cdbuf_idx_addr]  = 0U;
  reg->CDBUF[k_ra8_xspi_cdbuf_idx_data0] = 0U;
  reg->CDBUF[k_ra8_xspi_cdbuf_idx_data1] = 0U;
  return ra8_xspi_kick_command(reg);
}

ra8_err_t ra8_xspi_software_reset(uint8_t instance, uint8_t cmd_bytes)
{
  volatile r_xspi_regs_t* reg = ra8_xspi(instance);
  RA8_CHECK_NULL_PTR(reg, s_tag, "instance out of range");
  if ((cmd_bytes != (uint8_t)k_ra8_xspi_reset_cmd_bytes_1s) &&
      (cmd_bytes != (uint8_t)k_ra8_xspi_reset_cmd_bytes_8d)) {
    return k_ra8_err_invalid_arg;
  }

  /* IS25LX512M Ch 8.20-8.21 p 39: RSTEN must be the immediately
   * preceding command before RST or the device ignores RST. */
  const ra8_err_t en = internal_issue_reset_opcode(reg, k_ra8_spi_flash_op_reset_en, cmd_bytes);
  if (en != k_ra8_ok) {
    return en;
  }
  const ra8_err_t rst = internal_issue_reset_opcode(reg, k_ra8_spi_flash_op_reset_dev, cmd_bytes);
  if (rst != k_ra8_ok) {
    return rst;
  }
  return k_ra8_ok;
}
