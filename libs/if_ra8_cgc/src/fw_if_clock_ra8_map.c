/**
 * @file fw_if_clock_ra8_map.c
 * @brief The RA8 module-to-clock table, and nothing else.
 * @ingroup grp_fw_clock
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Kept apart from the ops in `fw_if_clock_ra8.c` so the mapping is a pure
 * function of its arguments: this translation unit includes no register header
 * and reads no hardware, so a host test can prove every row without a fake
 * peripheral block. The ops file is then thin enough to read in one go.
 *
 * @par Where the rows come from
 * A row exists only where the tree already establishes the pairing:
 *
 *   - UART -> PCLKA, SCIn: `libs/ra8_board_ek_ra8d2/src/ra8_board_ek_ra8d2_comms.c`
 *     reads PCLKA to derive the console baud divisor.
 *   - SPI -> PCLKA, SPIn: `examples/ek_ra8d2/hw_validated/c6/c6_spi_probe/src/main.c`
 *     reads PCLKA for the C6 link's bit rate.
 *   - I2C -> PCLKA, IICn: `examples/ek_ra8d2/hw_validated/hil/iic_b_facade_demo`
 *     and `.../i2c_i3c_combined` both read PCLKA for the bus timing.
 *   - CAN -> PCLKA, CANFDn: `libs/ra8_hal/src/ra8_canfd_timing.c` reads PCLKA
 *     twice to compute nominal and data bit timing.
 *   - SD host -> PCLKA, SDHIn: `examples/ek_ra8d2/hw_validated/hil/sd_font_render`
 *     reads PCLKA for the card clock divider.
 *   - Camera -> PCLKD, CEU: `libs/ra8_board_ek_ra8d2/src/ra8_board_ek_ra8d2_camera.c`
 *     reads PCLKD for the capture-unit timing.
 *   - Core -> CPUCLK0 and memory -> FCLK come from the domain list in
 *     `ra8_cgc.h` itself, which names CPUCLK0 as the Cortex-M85 clock and FCLK
 *     as the flash/MRAM interface clock.
 *
 * Gate-only rows (ADC, DAC, display, Ethernet, crypto) take their module-stop
 * bit from the table in `ra8_mstp_regs.h`, which is transcribed from the
 * hardware manual's own column. Their feed domain is *not* established
 * anywhere in the tree, so they have none here: an application that prints
 * CPUCLK0 in its banner is not evidence about what clocks its GPT.
 *
 * @par What is deliberately absent, and why
 *   - Timer and PWM: the GPT module-stop bits are not a uniform run. GPT0..3
 *     have one bit each, GPT4..GPT9 *share* MSTPE27, and GPT10..13 sit in a
 *     separate block, so no flat instance index addresses them correctly.
 *   - DMA: MSTPA22 and MSTPA23 each gate a DMAC *and* a DTC, and `ra8_mstp.h`
 *     already carries a hardware-manual caveat about releasing them.
 *   - USB: USBFS and USBHS are different blocks, not two instances of one, so
 *     a numeric index cannot choose between them without the board naming
 *     which it wired.
 *   - RTC and watchdog: no module-stop bit exists for either.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "fw_if_clock_ra8.h"

#include <stdbool.h>
#include <stdint.h>

#include "fw_if_clock.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_mstp.h"

/**
 * @brief Instance counts, named so the table reads as hardware rather than
 * as a column of integers.
 */
typedef enum : uint8_t {
  k_internal_instances_none   = 0U,
  k_internal_instances_one    = 1U,
  k_internal_instances_two    = 2U,
  k_internal_instances_three  = 3U,
  k_internal_instances_sci    = 10U,
} internal_instances_t;

/**
 * @struct internal_kind_row_t
 * @brief One row of the per-kind table.
 *
 * @details
 * `gate_base` is instance zero's module-stop id. Every multi-instance run this
 * adapter carries descends by one bit position within a single MSTPCRx word as
 * the instance number rises (SCI0 is MSTPB31 and SCI9 is MSTPB22; IIC0 is
 * MSTPB9 and IIC2 is MSTPB7; SPI, CANFD, SDHI and DAC12 do the same), so
 * instance N's id is `gate_base - N`. That is a property of the rows present,
 * not a general rule about the chip, which is exactly why the runs that break
 * it are absent rather than approximated.
 */
typedef struct internal_kind_row_s {
  ra8_clock_id_t       domain;
  ra8_mstp_t           gate_base;
  internal_instances_t instances;
  bool                 has_domain;
  bool                 has_gate;
} internal_kind_row_t;

/** @brief Placeholder for a row that carries no domain or no gate. */
#define INTERNAL_NO_DOMAIN k_ra8_clock_id_cpuclk0
/** @brief Placeholder for a row that carries no module-stop bit. */
#define INTERNAL_NO_GATE   k_ra8_mstp_sram0

/* Indexed by fw_clock_module_kind_t. Absent kinds keep zero instances, which
 * fw_clock_ra8_resolve reports as not-found. */
static const internal_kind_row_t k_internal_kinds[K_FW_CLOCK_MODULE_KIND_COUNT] = {
    [k_fw_clock_module_none] = {INTERNAL_NO_DOMAIN, INTERNAL_NO_GATE,
                                k_internal_instances_none, false, false},
    [k_fw_clock_module_core] = {k_ra8_clock_id_cpuclk0, INTERNAL_NO_GATE,
                                k_internal_instances_one, true, false},
    [k_fw_clock_module_uart] = {k_ra8_clock_id_pclka, k_ra8_mstp_sci0,
                                k_internal_instances_sci, true, true},
    [k_fw_clock_module_spi] = {k_ra8_clock_id_pclka, k_ra8_mstp_spi0,
                               k_internal_instances_two, true, true},
    [k_fw_clock_module_i2c] = {k_ra8_clock_id_pclka, k_ra8_mstp_iic0,
                               k_internal_instances_three, true, true},
    [k_fw_clock_module_can] = {k_ra8_clock_id_pclka, k_ra8_mstp_canfd0,
                               k_internal_instances_two, true, true},
    [k_fw_clock_module_timer] = {INTERNAL_NO_DOMAIN, INTERNAL_NO_GATE,
                                 k_internal_instances_none, false, false},
    [k_fw_clock_module_pwm] = {INTERNAL_NO_DOMAIN, INTERNAL_NO_GATE,
                               k_internal_instances_none, false, false},
    [k_fw_clock_module_adc] = {INTERNAL_NO_DOMAIN, k_ra8_mstp_adc16h,
                               k_internal_instances_one, false, true},
    [k_fw_clock_module_dac] = {INTERNAL_NO_DOMAIN, k_ra8_mstp_dac12_0,
                               k_internal_instances_two, false, true},
    [k_fw_clock_module_dma] = {INTERNAL_NO_DOMAIN, INTERNAL_NO_GATE,
                               k_internal_instances_none, false, false},
    [k_fw_clock_module_display] = {INTERNAL_NO_DOMAIN, k_ra8_mstp_glcdc,
                                   k_internal_instances_one, false, true},
    [k_fw_clock_module_camera] = {k_ra8_clock_id_pclkd, k_ra8_mstp_ceu,
                                  k_internal_instances_one, true, true},
    [k_fw_clock_module_usb] = {INTERNAL_NO_DOMAIN, INTERNAL_NO_GATE,
                               k_internal_instances_none, false, false},
    [k_fw_clock_module_ethernet] = {INTERNAL_NO_DOMAIN, k_ra8_mstp_eswm,
                                    k_internal_instances_one, false, true},
    [k_fw_clock_module_sdhost] = {k_ra8_clock_id_pclka, k_ra8_mstp_sdhi0,
                                  k_internal_instances_two, true, true},
    [k_fw_clock_module_crypto] = {INTERNAL_NO_DOMAIN, k_ra8_mstp_rsip,
                                  k_internal_instances_one, false, true},
    [k_fw_clock_module_rtc] = {INTERNAL_NO_DOMAIN, INTERNAL_NO_GATE,
                               k_internal_instances_none, false, false},
    [k_fw_clock_module_watchdog] = {INTERNAL_NO_DOMAIN, INTERNAL_NO_GATE,
                                    k_internal_instances_none, false, false},
    [k_fw_clock_module_memory] = {k_ra8_clock_id_fclk, INTERNAL_NO_GATE,
                                  k_internal_instances_one, true, false},
};

ra8_err_t fw_clock_ra8_resolve(fw_clock_module_t module, fw_clock_ra8_row_t *out_row)
{
  if (out_row == nullptr) {
    return k_ra8_err_invalid_arg;
  }

  out_row->domain     = INTERNAL_NO_DOMAIN;
  out_row->gate       = INTERNAL_NO_GATE;
  out_row->has_domain = false;
  out_row->has_gate   = false;

  if ((uint8_t)module.kind >= (uint8_t)K_FW_CLOCK_MODULE_KIND_COUNT) {
    return k_ra8_err_invalid_arg;
  }

  const internal_kind_row_t *const row = &k_internal_kinds[(uint8_t)module.kind];

  if (module.index >= (uint8_t)row->instances) {
    return k_ra8_err_not_found;
  }

  out_row->has_domain = row->has_domain;
  out_row->has_gate   = row->has_gate;

  if (row->has_domain) {
    out_row->domain = row->domain;
  }
  if (row->has_gate) {
    /* Descending within one MSTPCRx word; see internal_kind_row_t. */
    out_row->gate = (ra8_mstp_t)((uint32_t)row->gate_base - (uint32_t)module.index);
  }

  return k_ra8_ok;
}
