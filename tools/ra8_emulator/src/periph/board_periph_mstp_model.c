/**
 * @file board_periph_mstp_model.c
 * @brief Module-stop (MSTP) shadow + address->bit gate table (engine-free half)
 *
 * @details
 * Owns the five @c MSTPCRA..MSTPCRE words the RA8D2 uses to clock-gate every
 * peripheral (R_MSTP at @c 0x4020_3000 -- HUM Ch 11.2.6..11.2.10 p 443-450) and
 * the table that maps a peripheral register address to the module-stop bit that
 * governs it. ::priv_board_mstp_addr_stopped answers the one question the ra8_emulator
 * core asks per MMIO access: "is the peripheral that owns this address currently
 * unclocked?" -- and if so the core reads 0 / drops the write, matching silicon.
 *
 * This translation unit deliberately takes NO Unicorn dependency: it is pure
 * state + arithmetic, so the gate table is unit-tested directly on the host
 * (tests/test_ra8_emulator_mstp_gate.c) rather than only through a full emulation
 * run. The board_periph block glue that needs the engine (register window
 * ownership, reset hook, end-of-run report) is the separate
 * @c board_periph_mstp.c.
 *
 * ## Gate table scope
 *
 * An entry exists for each modelled peripheral instance that (a) has a
 * module-stop bit and (b) is answered by an OWNING board_periph block, so that
 * gating it has an observable effect. Peripherals with no module-stop control
 * (GPIO/PORT, ICU, SYSC), memory controllers (SRAM/MRAM), always-on blocks
 * (WDT), the observe-only snoops (GLCDC, PRCR) and the delicate shared/loop
 * families (DMAC+DTC on MSTPA22/23, the R-Switch Ethernet cluster, the USB
 * self-loop model) are intentionally NOT gated here and answer as before; each
 * is a documented deferral rather than an implicit gap. The bit for every entry
 * is taken from ``ra8_mstp_regs.h`` (``k_ra8_mstp_*``), whose per-bit HUM
 * citations are the source of truth.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "board_periph_mstp_internal.h"
#include "ra8_attributes.h"
#include "ra8_mstp_regs.h"

/** @brief Shadow-array dimensions + bit-field packing constants. */
typedef enum : uint32_t {
  k_mstp_reg_count     = 5U,    /**< MSTPCRA..MSTPCRE.             */
  k_mstp_bytes_per_reg = 4U,    /**< 32-bit registers.             */
  k_mstp_byte_bits     = 8U,    /**< Bits per byte.                */
  k_mstp_byte_mask     = 0xFFU, /**< Low-byte mask.                */
  k_mstp_id_reg_shift  = 8U,    /**< ra8_mstp_t: reg in bits 15:8. */
  k_mstp_id_bit_mask   = 0xFFU, /**< ra8_mstp_t: bit in bits 7:0.  */
} mstp_dim_t;

/**
 * @brief Per-register reset values: all peripheral bits stopped (HUM 11.2.6).
 *
 * @details
 * MSTPCRA keeps the SRAM0..3 bits (0..3) running (0) because the CPU stack
 * lives there; every other bit resets to 1 (stopped). MSTPCRB..E reset fully
 * stopped. This is the same all-stopped baseline ``ra8_mstp_init`` writes, so a
 * peripheral reads 0 until its driver clears its bit.
 */
typedef enum : uint32_t {
  k_mstp_reset_a    = 0xFFFFFFF0U, /**< MSTPCRA: SRAM0-3 (bits 0-3) running. */
  k_mstp_reset_rest = 0xFFFFFFFFU, /**< MSTPCRB..E: all peripherals stopped. */
} mstp_reset_t;

/* =============================================================================
 * Shadow state + observability.
 * =============================================================================
 */

static uint32_t    s_mstpcr[k_mstp_reg_count]; /**< MSTPCRA..E live values.        */
static uint32_t    s_gated_reads;              /**< Reads zeroed (peripheral off). */
static uint32_t    s_gated_writes;             /**< Writes dropped (periph off).   */
static const char* s_last_gated = "-";         /**< Label of last gated access.    */

/* =============================================================================
 * Address -> module-stop-bit gate table.
 *
 * A family covers one strided run of instances, each mapping to the packed
 * ra8_mstp_t bit at the same index; a single-instance peripheral is a family of
 * count 1 whose stride equals its whole register window. The bit values come
 * from ra8_mstp_regs.h -- their HUM Ch/page citations live there.
 * =============================================================================
 */

/** @brief SCI0..SCI9 (0x40358000, 0x100 stride) -- MSTPCRB31..22, HUM 11.2.7. */
static const uint16_t s_k_ids_sci[] = {
  (uint16_t)k_ra8_mstp_sci0,
  (uint16_t)k_ra8_mstp_sci1,
  (uint16_t)k_ra8_mstp_sci2,
  (uint16_t)k_ra8_mstp_sci3,
  (uint16_t)k_ra8_mstp_sci4,
  (uint16_t)k_ra8_mstp_sci5,
  (uint16_t)k_ra8_mstp_sci6,
  (uint16_t)k_ra8_mstp_sci7,
  (uint16_t)k_ra8_mstp_sci8,
  (uint16_t)k_ra8_mstp_sci9,
};
/** @brief SPI0..SPI1 (0x4035C000, 0x100 stride) -- MSTPCRB19..18, HUM 11.2.7. */
static const uint16_t s_k_ids_spi[] = {
  (uint16_t)k_ra8_mstp_spi0,
  (uint16_t)k_ra8_mstp_spi1,
};
/** @brief IIC0..IIC2 / RIIC (0x4025E000, 0x100 stride) -- MSTPCRB9..7, HUM 11.2.7. */
static const uint16_t s_k_ids_riic[] = {
  (uint16_t)k_ra8_mstp_iic0,
  (uint16_t)k_ra8_mstp_iic1,
  (uint16_t)k_ra8_mstp_iic2,
};
/** @brief I3C0 (0x4035F000) -- MSTPCRB4, HUM 11.2.7. */
static const uint16_t s_k_ids_i3c[] = {(uint16_t)k_ra8_mstp_i3c};
/** @brief AGT0..AGT1 (0x40221000, 0x100 stride) -- MSTPCRD5..4, HUM 11.2.9. */
static const uint16_t s_k_ids_agt[] = {
  (uint16_t)k_ra8_mstp_agt0,
  (uint16_t)k_ra8_mstp_agt1,
};
/** @brief GPT0..GPT13 (0x40322000, 0x100 stride) -- MSTPCRE31..18, GPT4..9 share
 *  MSTPE27, HUM 11.2.10. */
static const uint16_t s_k_ids_gpt[] = {
  (uint16_t)k_ra8_mstp_gpt0,
  (uint16_t)k_ra8_mstp_gpt1,
  (uint16_t)k_ra8_mstp_gpt2,
  (uint16_t)k_ra8_mstp_gpt3,
  (uint16_t)k_ra8_mstp_gpt4_9,
  (uint16_t)k_ra8_mstp_gpt4_9,
  (uint16_t)k_ra8_mstp_gpt4_9,
  (uint16_t)k_ra8_mstp_gpt4_9,
  (uint16_t)k_ra8_mstp_gpt4_9,
  (uint16_t)k_ra8_mstp_gpt4_9,
  (uint16_t)k_ra8_mstp_gpt10,
  (uint16_t)k_ra8_mstp_gpt11,
  (uint16_t)k_ra8_mstp_gpt12,
  (uint16_t)k_ra8_mstp_gpt13,
};
/** @brief ULPT0..ULPT1 (0x40220000, 0x100 stride) -- MSTPCRE9..8, HUM 11.2.10. */
static const uint16_t s_k_ids_ulpt[] = {
  (uint16_t)k_ra8_mstp_ulpt0,
  (uint16_t)k_ra8_mstp_ulpt1,
};
/** @brief DAC_B0..DAC_B1 (0x40233000, 0x100 stride) -- MSTPCRD20..19, HUM 11.2.9. */
static const uint16_t s_k_ids_dac[] = {
  (uint16_t)k_ra8_mstp_dac12_0,
  (uint16_t)k_ra8_mstp_dac12_1,
};
/** @brief SSIE0..SSIE1 (0x4025D000, 0x100 stride) -- MSTPCRC8..7, HUM 11.2.8. */
static const uint16_t s_k_ids_ssie[] = {
  (uint16_t)k_ra8_mstp_ssie0,
  (uint16_t)k_ra8_mstp_ssie1,
};
/** @brief POEG group A..D (0x40212000, 0x100 stride) -- MSTPCRD14..11, HUM 11.2.9. */
static const uint16_t s_k_ids_poeg[] = {
  (uint16_t)k_ra8_mstp_poeg_a,
  (uint16_t)k_ra8_mstp_poeg_b,
  (uint16_t)k_ra8_mstp_poeg_c,
  (uint16_t)k_ra8_mstp_poeg_d,
};
/** @brief CANFD0 (0x40380000) -- MSTPCRC27, HUM 11.2.8. */
static const uint16_t s_k_ids_canfd0[] = {(uint16_t)k_ra8_mstp_canfd0};
/** @brief CANFD1 (0x40382000) -- MSTPCRC26, HUM 11.2.8. */
static const uint16_t s_k_ids_canfd1[] = {(uint16_t)k_ra8_mstp_canfd1};
/** @brief CAC (0x40202400) -- MSTPCRC0, HUM 11.2.8. */
static const uint16_t s_k_ids_cac[] = {(uint16_t)k_ra8_mstp_cac};
/** @brief CRC (0x40310000) -- MSTPCRC1, HUM 11.2.8. */
static const uint16_t s_k_ids_crc[] = {(uint16_t)k_ra8_mstp_crc};
/** @brief DOC (0x40311000) -- MSTPCRC13, HUM 11.2.8. */
static const uint16_t s_k_ids_doc[] = {(uint16_t)k_ra8_mstp_doc};
/** @brief CEU (0x40348000) -- MSTPCRC16, HUM 11.2.8. */
static const uint16_t s_k_ids_ceu[] = {(uint16_t)k_ra8_mstp_ceu};
/** @brief PDM-IF (0x40256000) -- MSTPCRC24, HUM 11.2.8. */
static const uint16_t s_k_ids_pdm[] = {(uint16_t)k_ra8_mstp_pdmif};
/** @brief ADC_B (0x40338000) -- MSTPCRD21, HUM 11.2.9. */
static const uint16_t s_k_ids_adc[] = {(uint16_t)k_ra8_mstp_adc16h};
/** @brief SDHI0 (0x40252000) -- MSTPCRC12, HUM 11.2.8. */
static const uint16_t s_k_ids_sdhi[] = {(uint16_t)k_ra8_mstp_sdhi0};
/** @brief XSPI0 / OSPI0 (0x40268000) -- MSTPCRB16, HUM 11.2.7. */
static const uint16_t s_k_ids_xspi[] = {(uint16_t)k_ra8_mstp_ospi0};
/** @brief DRW / D-AVE 2D (0x40444000) -- MSTPCRC6, HUM 11.2.8. */
static const uint16_t s_k_ids_drw[] = {(uint16_t)k_ra8_mstp_drw};

/**
 * @brief One strided family of peripheral instances sharing a mapping shape.
 *
 * @details @c base + @c stride * i is the window of instance @c i, whose
 * module-stop bit is @c ids[i]; @c count instances are covered. A single
 * peripheral is @c count == 1 with @c stride == its whole register window.
 */
typedef struct {
  uint64_t        base;   /**< Window base of instance 0.                 */
  uint64_t        stride; /**< Bytes between consecutive instances.       */
  uint32_t        count;  /**< Number of gated instances in the family.   */
  const uint16_t* ids;    /**< [count] packed ra8_mstp_t per instance.    */
  const char*     name;   /**< Short label for the dropped-access report. */
} mstp_family_t;

/** @brief Peripheral-family register-window bases (RA8D2 peripheral memory map). */
typedef enum : uint64_t {
  k_mstp_base_sci    = 0x40358000UL, /**< SCI0 (SCI family base).   */
  k_mstp_base_spi    = 0x4035C000UL, /**< SPI0 (SPI_B family base). */
  k_mstp_base_riic   = 0x4025E000UL, /**< IIC0 (RIIC family base).  */
  k_mstp_base_i3c    = 0x4035F000UL, /**< I3C0.                     */
  k_mstp_base_agt    = 0x40221000UL, /**< AGT0 (AGT family base).   */
  k_mstp_base_gpt    = 0x40322000UL, /**< GPT0 (GPT family base).   */
  k_mstp_base_ulpt   = 0x40220000UL, /**< ULPT0 (ULPT family base). */
  k_mstp_base_dac    = 0x40233000UL, /**< DAC_B0 (DAC family base). */
  k_mstp_base_ssie   = 0x4025D000UL, /**< SSIE0 (SSIE family base). */
  k_mstp_base_poeg   = 0x40212000UL, /**< POEG group A (POEG base). */
  k_mstp_base_canfd0 = 0x40380000UL, /**< CANFD0.                   */
  k_mstp_base_canfd1 = 0x40382000UL, /**< CANFD1.                   */
  k_mstp_base_cac    = 0x40202400UL, /**< CAC.                      */
  k_mstp_base_crc    = 0x40310000UL, /**< CRC.                      */
  k_mstp_base_doc    = 0x40311000UL, /**< DOC.                      */
  k_mstp_base_ceu    = 0x40348000UL, /**< CEU.                      */
  k_mstp_base_pdm    = 0x40256000UL, /**< PDM-IF.                   */
  k_mstp_base_adc    = 0x40338000UL, /**< ADC_B.                    */
  k_mstp_base_sdhi   = 0x40252000UL, /**< SDHI0.                    */
  k_mstp_base_xspi   = 0x40268000UL, /**< XSPI0.                    */
  k_mstp_base_drw    = 0x40444000UL, /**< DRW / D-AVE 2D.           */
} mstp_family_base_t;

/** @brief Per-family instance stride / single-instance window span (bytes). */
typedef enum : uint64_t {
  k_mstp_chan_stride = 0x100UL,  /**< Channel stride (also CEU window). */
  k_mstp_span_i3c    = 0x214UL,  /**< I3C window (through BCST).        */
  k_mstp_span_canfd  = 0x1920UL, /**< CANFD channel window.             */
  k_mstp_span_cac    = 0x10UL,   /**< CAC register window.              */
  k_mstp_span_crc    = 0x20UL,   /**< CRC register window.              */
  k_mstp_span_doc    = 0x20UL,   /**< DOC register window.              */
  k_mstp_span_pdm    = 0x400UL,  /**< PDM common + channel banks.       */
  k_mstp_span_adc    = 0x2224UL, /**< ADC_B register window.            */
  k_mstp_span_sdhi   = 0x200UL,  /**< SDHI0 register window.            */
  k_mstp_span_xspi   = 0x200UL,  /**< XSPI0 register window.            */
  k_mstp_span_drw    = 0x104UL,  /**< DRW register window.              */
} mstp_family_span_t;

/** @brief Instance counts per family (channels with a module-stop bit). */
typedef enum : uint32_t {
  k_mstp_count_1  = 1U,  /**< Single-instance peripheral.  */
  k_mstp_count_2  = 2U,  /**< SPI/AGT/ULPT/DAC/SSIE (two). */
  k_mstp_count_3  = 3U,  /**< RIIC (three channels).       */
  k_mstp_count_4  = 4U,  /**< POEG (four groups).          */
  k_mstp_count_10 = 10U, /**< SCI (ten channels).          */
  k_mstp_count_14 = 14U, /**< GPT (fourteen channels).     */
} mstp_family_count_t;

/** @brief The gate table: every module-stop-gated modelled peripheral instance. */
static const mstp_family_t s_k_families[] = {
  {(uint64_t)k_mstp_base_sci,
   (uint64_t)k_mstp_chan_stride,
   (uint32_t)k_mstp_count_10,
   s_k_ids_sci,
   "SCI"},
  {(uint64_t)k_mstp_base_spi,
   (uint64_t)k_mstp_chan_stride,
   (uint32_t)k_mstp_count_2,
   s_k_ids_spi,
   "SPI_B"},
  {(uint64_t)k_mstp_base_riic,
   (uint64_t)k_mstp_chan_stride,
   (uint32_t)k_mstp_count_3,
   s_k_ids_riic,
   "RIIC"},
  {(uint64_t)k_mstp_base_i3c,
   (uint64_t)k_mstp_span_i3c,
   (uint32_t)k_mstp_count_1,
   s_k_ids_i3c,
   "I3C"},
  {(uint64_t)k_mstp_base_agt,
   (uint64_t)k_mstp_chan_stride,
   (uint32_t)k_mstp_count_2,
   s_k_ids_agt,
   "AGT"},
  {(uint64_t)k_mstp_base_gpt,
   (uint64_t)k_mstp_chan_stride,
   (uint32_t)k_mstp_count_14,
   s_k_ids_gpt,
   "GPT"},
  {(uint64_t)k_mstp_base_ulpt,
   (uint64_t)k_mstp_chan_stride,
   (uint32_t)k_mstp_count_2,
   s_k_ids_ulpt,
   "ULPT"},
  {(uint64_t)k_mstp_base_dac,
   (uint64_t)k_mstp_chan_stride,
   (uint32_t)k_mstp_count_2,
   s_k_ids_dac,
   "DAC_B"},
  {(uint64_t)k_mstp_base_ssie,
   (uint64_t)k_mstp_chan_stride,
   (uint32_t)k_mstp_count_2,
   s_k_ids_ssie,
   "SSIE"},
  {(uint64_t)k_mstp_base_poeg,
   (uint64_t)k_mstp_chan_stride,
   (uint32_t)k_mstp_count_4,
   s_k_ids_poeg,
   "POEG"},
  {(uint64_t)k_mstp_base_canfd0,
   (uint64_t)k_mstp_span_canfd,
   (uint32_t)k_mstp_count_1,
   s_k_ids_canfd0,
   "CANFD0"},
  {(uint64_t)k_mstp_base_canfd1,
   (uint64_t)k_mstp_span_canfd,
   (uint32_t)k_mstp_count_1,
   s_k_ids_canfd1,
   "CANFD1"},
  {(uint64_t)k_mstp_base_cac,
   (uint64_t)k_mstp_span_cac,
   (uint32_t)k_mstp_count_1,
   s_k_ids_cac,
   "CAC"},
  {(uint64_t)k_mstp_base_crc,
   (uint64_t)k_mstp_span_crc,
   (uint32_t)k_mstp_count_1,
   s_k_ids_crc,
   "CRC"},
  {(uint64_t)k_mstp_base_doc,
   (uint64_t)k_mstp_span_doc,
   (uint32_t)k_mstp_count_1,
   s_k_ids_doc,
   "DOC"},
  {(uint64_t)k_mstp_base_ceu,
   (uint64_t)k_mstp_chan_stride,
   (uint32_t)k_mstp_count_1,
   s_k_ids_ceu,
   "CEU"},
  {(uint64_t)k_mstp_base_pdm,
   (uint64_t)k_mstp_span_pdm,
   (uint32_t)k_mstp_count_1,
   s_k_ids_pdm,
   "PDM-IF"},
  {(uint64_t)k_mstp_base_adc,
   (uint64_t)k_mstp_span_adc,
   (uint32_t)k_mstp_count_1,
   s_k_ids_adc,
   "ADC_B"},
  {(uint64_t)k_mstp_base_sdhi,
   (uint64_t)k_mstp_span_sdhi,
   (uint32_t)k_mstp_count_1,
   s_k_ids_sdhi,
   "SDHI0"},
  {(uint64_t)k_mstp_base_xspi,
   (uint64_t)k_mstp_span_xspi,
   (uint32_t)k_mstp_count_1,
   s_k_ids_xspi,
   "XSPI0"},
  {(uint64_t)k_mstp_base_drw,
   (uint64_t)k_mstp_span_drw,
   (uint32_t)k_mstp_count_1,
   s_k_ids_drw,
   "DRW"},
};

/* =============================================================================
 * Internal helpers.
 * =============================================================================
 */

/**
 * @brief Resolve @p addr to the family + instance index that owns it.
 *
 * @param[in]  addr    Absolute peripheral address.
 * @param[out] out_idx Instance index within the family on a hit.
 * @return The owning family, or NULL when @p addr is not gated.
 * @since 0.1.0
 */
RA8_INTERNAL
static const mstp_family_t* internal_mstp_family_for_addr(uint64_t addr, uint32_t* out_idx)
{
  for (uint32_t f = 0U; f < (sizeof(s_k_families) / sizeof(s_k_families[0])); f++) {
    const mstp_family_t* fam  = &s_k_families[f];
    const uint64_t       span = fam->stride * (uint64_t)fam->count;
    if ((addr >= fam->base) && (addr < (fam->base + span))) {
      *out_idx = (uint32_t)((addr - fam->base) / fam->stride);
      return fam;
    }
  }
  return nullptr;
}

/**
 * @brief True iff the module-stop bit @p id (packed ra8_mstp_t) reads set.
 * @details True iff the module-stop bit @p id (packed ra8_mstp_t) reads set; this step is contained within the board periph module-stop model model and uses bounded caller or module-owned storage.
 * @param[in] id Id input used by the operation.
 * @return The module-stop bit set result produced by the board periph module-stop model model.
 * @retval true The module-stop bit set condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for module-stop bit set. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph module-stop model model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_mstp_bit_set(uint16_t id)
{
  const uint32_t reg = ((uint32_t)id >> (uint32_t)k_mstp_id_reg_shift) & (uint32_t)k_mstp_byte_mask;
  const uint32_t bit = (uint32_t)id & (uint32_t)k_mstp_id_bit_mask;
  if (reg >= (uint32_t)k_mstp_reg_count) {
    return false; /* defensive: a malformed id gates nothing. */
  }
  return (s_mstpcr[reg] & ((uint32_t)1U << bit)) != 0U;
}

/* =============================================================================
 * Public (module-private) API.
 * =============================================================================
 */

RA8_PRIV void priv_board_mstp_reset(void)
{
  s_mstpcr[0] = (uint32_t)k_mstp_reset_a;
  for (uint32_t r = 1U; r < (uint32_t)k_mstp_reg_count; r++) {
    s_mstpcr[r] = (uint32_t)k_mstp_reset_rest;
  }
  s_gated_reads  = 0U;
  s_gated_writes = 0U;
  s_last_gated   = "-";
}

RA8_PRIV void priv_board_mstp_apply_write(uint64_t off, unsigned size, uint32_t value)
{
  for (unsigned i = 0U; i < size; i++) {
    const uint64_t b = off + (uint64_t)i;
    if (b >= (uint64_t)k_board_mstp_win_span) {
      break;
    }
    const uint32_t reg = (uint32_t)(b / (uint64_t)k_mstp_bytes_per_reg);
    const uint32_t sh = (uint32_t)(b % (uint64_t)k_mstp_bytes_per_reg) * (uint32_t)k_mstp_byte_bits;
    const uint32_t vb =
      (value >> ((uint32_t)i * (uint32_t)k_mstp_byte_bits)) & (uint32_t)k_mstp_byte_mask;
    s_mstpcr[reg] = (s_mstpcr[reg] & ~((uint32_t)k_mstp_byte_mask << sh)) | (vb << sh);
  }
}

RA8_PRIV uint32_t priv_board_mstp_read_reg(uint64_t off, unsigned size)
{
  uint32_t v = 0U;
  for (unsigned i = 0U; i < size; i++) {
    const uint64_t b = off + (uint64_t)i;
    if (b >= (uint64_t)k_board_mstp_win_span) {
      continue;
    }
    const uint32_t reg = (uint32_t)(b / (uint64_t)k_mstp_bytes_per_reg);
    const uint32_t sh = (uint32_t)(b % (uint64_t)k_mstp_bytes_per_reg) * (uint32_t)k_mstp_byte_bits;
    const uint32_t vb = (s_mstpcr[reg] >> sh) & (uint32_t)k_mstp_byte_mask;
    v |= vb << ((uint32_t)i * (uint32_t)k_mstp_byte_bits);
  }
  return v;
}

RA8_PRIV bool priv_board_mstp_addr_stopped(uint64_t addr)
{
  uint32_t             idx = 0U;
  const mstp_family_t* fam = internal_mstp_family_for_addr(addr, &idx);
  if (fam == nullptr) {
    return false; /* not gated: unmodelled or a peripheral with no module-stop. */
  }
  return internal_mstp_bit_set(fam->ids[idx]);
}

RA8_PRIV void priv_board_mstp_note_gated_access(uint64_t addr, bool is_write)
{
  uint32_t             idx = 0U;
  const mstp_family_t* fam = internal_mstp_family_for_addr(addr, &idx);
  if (fam != nullptr) {
    s_last_gated = fam->name;
  }
  if (is_write) {
    s_gated_writes++;
  } else {
    s_gated_reads++;
  }
}

RA8_PRIV uint32_t priv_board_mstp_gated_read_count(void)
{
  return s_gated_reads;
}

RA8_PRIV uint32_t priv_board_mstp_gated_write_count(void)
{
  return s_gated_writes;
}

RA8_PRIV const char* priv_board_mstp_last_gated_name(void)
{
  return s_last_gated;
}
