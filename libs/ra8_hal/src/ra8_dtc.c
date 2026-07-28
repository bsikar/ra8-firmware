/**
 * @file ra8_dtc.c
 * @brief Data Transfer Controller driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Driver for the RA8D2 DTC block. Every register access carries a
 * HUM Ch 18 citation. Shares MSTPA22 with DMAC0 via ra8_mstp's
 * reference counter (HUM 11.2.6 Note 1).
 *
 * FSP-mapping (`r_dtc.c`):
 *   FSP entry-point        | This driver
 *   -----------------------+--------------------------
 *   R_DTC_Open             | ra8_dtc_init      (programmes DTCVBR + MSTP)
 *   R_DTC_Close            | ra8_dtc_deinit
 *   R_DTC_Enable           | ra8_dtc_enable    (DTCST = 1)
 *   R_DTC_Disable          | ra8_dtc_disable   (DTCST = 0)
 *   R_DTC_Reconfigure      | ra8_dtc_reconfigure
 *   R_DTC_Reset            | (call sites edit TI directly + RRS toggle)
 *   R_DTC_InfoGet          | (call sites read TI fields directly)
 *   R_DTC_Reload           | unsupported (FSP returns FSP_ERR_UNSUPPORTED)
 *   R_DTC_CallbackSet      | ra8_dtc_attach_handler (status-bit fan-out)
 *   R_DTC_SoftwareStart    | unsupported (DTC has no SW trigger)
 *   R_DTC_SoftwareStop     | unsupported (DTC has no SW trigger)
 *
 * RRS write-skip handling matches FSP literally: bit 3 of DTCCR is
 * documented as "reserved -- write 1, read 1" (HUM 18.2.1 p 786),
 * so the RRS-disable / RRS-enable values are 0x08 / 0x18 not
 * 0x00 / 0x10.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_dtc.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_cache.h"
#include "ra8_check.h"
#include "ra8_dtc_regs.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_mstp.h"

static const char* s_tag = "DTC";

static ra8_dtc_event_fn_t s_dtc_fn;
static void*              s_dtc_ctx;

/* Retained DTC vector-table base. ra8_dtc_enable() has no vector_base
 * argument, yet the DTC fetches the table from RAM at DTCST=1; keep the
 * base from ra8_dtc_init()/ra8_dtc_reconfigure() so the pre-enable D-cache
 * clean can flush the caller-populated entries out to memory. */
static void* s_dtc_vector_base;

ra8_err_t ra8_dtc_init(void* vector_base)
{
  RA8_CHECK_NULL_PTR(vector_base, s_tag, "vector_base must not be nullptr");

  /* DTC0 + DMAC0 share MSTPA22; ra8_mstp keeps the ref count so
   * a follow-up ra8_dmac_start does not flip the bit again.
   * HUM Ch 11.2.6 "MSTPCRA : Module Stop Control Register A" p 443 */
  const ra8_err_t mst_err = ra8_mstp_enable(k_ra8_mstp_dmac0_dtc0);
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "dtc_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  volatile r_dtc_regs_t* reg = ra8_dtc();
  /* HUM 18.2.1 DTCCR p 786 / 18.2.2 DTCVBR p 787 / 18.2.3 DTCST p 787. On a
   * TrustZone part the secure (and flat-secure) DTC engine fetches its vector
   * table from DTCVBR_SEC (+0x14, HUM 18.2.6 p 789); the plain DTCVBR (+0x04)
   * is the Non-secure alias and a secure write to it is dropped. Program both
   * so the table is found in either world (and so the ra8_emulator DTC model,
   * which shadows DTCVBR, still sees the base). */
  reg->DTCCR        = 0U;
  reg->DTCVBR       = (uint32_t)(uintptr_t)vector_base;
  reg->DTCVBR_SEC   = (uint32_t)(uintptr_t)vector_base;
  reg->DTCST        = 0U;
  s_dtc_vector_base = vector_base; /* flushed to RAM by ra8_dtc_enable() */

  ra8_log_info(s_tag, "dtc_init");
  return k_ra8_ok;
}

ra8_err_t ra8_dtc_deinit(void)
{
  volatile r_dtc_regs_t* reg = ra8_dtc();
  /* HUM 18.2.3 DTCST p 787 / 18.2.1 DTCCR p 786 / 18.2.2 DTCVBR p 787 /
   * 18.2.6 DTCVBR_SEC p 789. Clear both vector bases (see ra8_dtc_init). */
  reg->DTCST        = 0U;
  reg->DTCCR        = 0U;
  reg->DTCVBR       = 0U;
  reg->DTCVBR_SEC   = 0U;
  s_dtc_fn          = nullptr;
  s_dtc_ctx         = nullptr;
  s_dtc_vector_base = nullptr;
  return ra8_mstp_disable(k_ra8_mstp_dmac0_dtc0);
}

ra8_err_t ra8_dtc_enable(void)
{
  /* Cache coherency (M85 L1 D-cache): the DTC fetches its vector table --
   * and, one indirection on, each per-source TI block -- straight from RAM.
   * Vector-table entries the CPU wrote between ra8_dtc_init() and here may
   * still sit in dirty cache lines the engine cannot see, so clean (write
   * back) the table region before DTCST=1. Clean is non-destructive and an
   * architectural no-op when the D-cache is off (every current app), so it
   * is added unconditionally. The TI blocks and the source/destination data
   * buffers are NOT reachable from this driver: the DTC is direction-blind
   * (like the DMAC), so the owning driver cleans the TI/source and
   * invalidates the destination. */
  if (s_dtc_vector_base != nullptr) {
    (void)ra8_cache_dcache_clean_by_addr(s_dtc_vector_base, (uint32_t)k_ra8_dtc_vector_table_align);
  }
  /* HUM 18.2.3 DTCST p 787. */
  ra8_dtc()->DTCST = k_ra8_dtcst_dtcst_msk;
  return k_ra8_ok;
}

ra8_err_t ra8_dtc_disable(void)
{
  /* HUM 18.2.3 DTCST p 787. */
  ra8_dtc()->DTCST = 0U;
  return k_ra8_ok;
}

ra8_err_t ra8_dtc_reconfigure(void* vector_base)
{
  RA8_CHECK_NULL_PTR(vector_base, s_tag, "vector_base must not be nullptr");
  volatile r_dtc_regs_t* reg = ra8_dtc();
  /* HUM 18.2.3 DTCST p 787 / 18.2.2 DTCVBR p 787 / 18.2.1 DTCCR p 786.
   * Mirrors FSP r_dtc_set_info(): drop RRS before touching the table,
   * rewrite the base, then re-enable RRS so the read-skip cache picks
   * up the fresh entries. Bit 3 of DTCCR is reserved-write-1. */
  reg->DTCST        = 0U;
  reg->DTCVBR       = (uint32_t)(uintptr_t)vector_base;
  reg->DTCVBR_SEC   = (uint32_t)(uintptr_t)vector_base;
  s_dtc_vector_base = vector_base;
  /* Cache coherency (M85 L1 D-cache): the RRS toggle below drops the DTC's
   * internal read-skip cache and forces it to re-read its descriptors from
   * RAM. Clean the CPU's vector-table edits out to memory first so the
   * re-read sees them, not stale dirty lines. Non-destructive and a no-op
   * when the D-cache is off. The indirectly-referenced TI blocks live one
   * pointer-hop away and are flushed by the owning driver (DTC is
   * direction-blind, like the DMAC). */
  (void)ra8_cache_dcache_clean_by_addr(vector_base, (uint32_t)k_ra8_dtc_vector_table_align);
  reg->DTCCR = k_ra8_dtccr_rrs_disable;
  reg->DTCCR = k_ra8_dtccr_rrs_enable;
  return k_ra8_ok;
}

ra8_err_t ra8_dtc_get_status(uint16_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM 18.2.4 DTCSTS p 788. */
  *out_mask = ra8_dtc()->DTCSTS;
  return k_ra8_ok;
}

ra8_err_t ra8_dtc_clear_status(uint16_t mask)
{
  volatile r_dtc_regs_t* reg = ra8_dtc();
  /* HUM 18.2.4 DTCSTS p 788. */
  reg->DTCSTS = (uint16_t)(reg->DTCSTS & ~mask);
  return k_ra8_ok;
}

ra8_err_t ra8_dtc_attach_handler(ra8_dtc_event_fn_t fn, void* ctx)
{
  s_dtc_fn  = fn;
  s_dtc_ctx = ctx;
  return k_ra8_ok;
}

RA8_ISR_SAFE
void ra8_dtc_dispatch(void)
{
  volatile r_dtc_regs_t* reg = ra8_dtc();
  /* HUM 18.2.4 DTCSTS p 788. */
  const uint16_t           mask = reg->DTCSTS;
  const ra8_dtc_event_fn_t fn   = s_dtc_fn;
  void* const              ctx  = s_dtc_ctx;
  reg->DTCSTS                   = 0U;
  if (fn != nullptr) {
    fn(ctx, mask);
  }
}

ra8_err_t ra8_dtc_enter_stop(void)
{
  /* HUM 18.2.3 DTCST p 787. */
  ra8_dtc()->DTCST = 0U;
  return ra8_mstp_disable(k_ra8_mstp_dmac0_dtc0);
}

ra8_err_t ra8_dtc_exit_stop(void)
{
  return ra8_mstp_enable(k_ra8_mstp_dmac0_dtc0);
}
