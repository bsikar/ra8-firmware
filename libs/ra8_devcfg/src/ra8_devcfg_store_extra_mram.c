/**
 * @file ra8_devcfg_store_extra_mram.c
 * @brief Production ::ra8_devcfg_store_t binding over the extra-MRAM window.
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details
 * The durable backend for ``ra8_devcfg``: reads dereference the extra-MRAM
 * (data-flash) window at ``k_ra8_flash_extra_start + offset`` and writes go
 * through ``ra8_flash_extra_mram_write`` in ::k_ra8_devcfg_page_bytes program
 * pages. The window is untouched by any DFU slot program or erase, so the
 * record survives an A/B update and a rollback alike.
 *
 * Under ``RA8_SIMULATOR_MODE`` (the host unit-test build) both accessors
 * address a RAM shadow instead: the flash MACI registers are modelled by the
 * simulator but the extra-MRAM *data* side is not, so the shadow lets the host
 * exercise the identical read / page-loop / offset control flow without MMIO.
 * Silicon and ra8_emulator take the ``#else`` branch and drive the real window.
 *
 * @note Blank (never-programmed) extra-MRAM reads back as 0xFF with valid ECC
 *       and does NOT bus-fault on the corrected window (#315); no fault-catch
 *       probe is needed, and 0xFF fails the record magic so a virgin unit
 *       resolves cleanly to UNPROVISIONED.
 * @note The window is one-time-programmable (HUM Ch 59.7.4.5); a commit
 *       programs a fresh copy slot rather than rewriting one in place.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_devcfg.h"
#include "ra8_err.h"

#ifndef RA8_SIMULATOR_MODE
#include "ra8_flash_core.h"
#include "ra8_flash_regs.h"
#endif

/**
 * @var s_tag
 * @brief Logging tag for the extra-MRAM backend.
 * @warning Do not modify.
 * @since 0.1.0
 */
static const char* const s_tag = "DEVCFG_XM";

/**
 * @enum ra8_devcfg_xm_span_t
 * @brief End of the devcfg region: copy 1 offset plus one reserved slot.
 * @details The highest byte any store access may touch. Both the RAM shadow
 *          size and the silicon bounds guard derive from it, so the two cannot
 *          disagree about the region extent.
 */
typedef enum : uint32_t {
  k_ra8_devcfg_xm_span = /**< One past the last devcfg region byte. */
  (uint32_t)k_ra8_devcfg_copy1_off + (uint32_t)k_ra8_devcfg_slot_bytes,
} ra8_devcfg_xm_span_t;

#ifdef RA8_SIMULATOR_MODE

/**
 * @enum ra8_devcfg_xm_sim_t
 * @brief Host-shadow constants.
 */
typedef enum : uint8_t {
  k_ra8_devcfg_xm_blank = 0xFFU, /**< Value an unprogrammed byte reads back as. */
} ra8_devcfg_xm_sim_t;

/**
 * @var s_shadow
 * @brief Host-test RAM shadow of the extra-MRAM devcfg region.
 * @details Filled with the blank sentinel on first access (see
 *          ::internal_devcfg_xm_shadow) so an unwritten read is
 *          indistinguishable from virgin silicon. Test seam only; never
 *          compiled into a silicon image.
 * @warning File-private; exercised only through the default store.
 * @since 0.1.0
 */
static uint8_t s_shadow[k_ra8_devcfg_xm_span] = {};

/**
 * @var s_shadow_ready
 * @brief Whether ::s_shadow has been filled with the blank sentinel yet.
 * @warning File-private; host build only.
 * @since 0.1.0
 */
static bool s_shadow_ready = false;

/**
 * @brief Return the RAM shadow, blank-filling it on the first call.
 *
 * @details Deferred fill (rather than a range-designated initialiser) keeps the
 *          declaration portable while still presenting a virgin 0xFF window to
 *          the first read.
 *
 * @return Pointer to the process-lifetime shadow; never NULL.
 * @retval non-NULL The blank-filled RAM shadow.
 *
 * @pre  Host (``RA8_SIMULATOR_MODE``) build only.
 * @pre  Called only from the extra-MRAM read / write backends.
 * @post The shadow is blank-filled exactly once.
 * @post Subsequent calls return the same pointer without re-filling.
 *
 * @note Not thread-safe; host test seam only.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t* internal_devcfg_xm_shadow(void)
{
  if (!s_shadow_ready) {
    (void)memset(s_shadow, (int)k_ra8_devcfg_xm_blank, sizeof s_shadow);
    s_shadow_ready = true;
  }
  return s_shadow;
}

#endif /* RA8_SIMULATOR_MODE */

/**
 * @brief Extra-MRAM read backend (::ra8_devcfg_read_fn_t).
 *
 * @details On silicon, copies ``len`` bytes out of the memory-mapped
 *          extra-MRAM window; a blank word reads back as 0xFF without faulting
 *          (#315). Under simulation, copies from the RAM shadow.
 *
 * @param[in]  offset Byte offset into the devcfg region.
 * @param[out] dst    Destination; non-NULL, at least ``len`` bytes.
 * @param[in]  len    Bytes to read.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok             ``len`` bytes copied.
 * @retval k_ra8_err_null_ptr   ``dst`` was NULL.
 * @retval k_ra8_err_out_of_range ``offset + len`` leaves the devcfg region.
 *
 * @pre  ``dst`` has room for ``len`` bytes.
 * @pre  ``offset + len`` lies inside the devcfg region.
 * @post On success ``dst[0 .. len)`` holds the window contents.
 * @post No backing store is programmed.
 *
 * @note Not thread-safe; boot / provisioning path only.
 * @since 0.1.0
 */
RA8_INTERNAL
[[nodiscard]] static ra8_err_t internal_devcfg_xm_read(uint32_t offset, uint8_t* dst, uint32_t len)
{
  RA8_CHECK_NULL_PTR(dst, s_tag, "xm read: dst null");
  if ((offset + len) > (uint32_t)k_ra8_devcfg_xm_span) {
    return k_ra8_err_out_of_range;
  }
#ifdef RA8_SIMULATOR_MODE
  (void)memcpy(dst, &internal_devcfg_xm_shadow()[offset], (size_t)len);
#else
  /* HUM Ch 59.1 "Address Map" p 3543 -- the extra-MRAM window is directly
   * memory-mapped for CPU reads; a virgin word returns 0xFFFFFFFF with valid
   * ECC and no BusFault (#315). */
  const volatile uint8_t* src =
    (const volatile uint8_t*)(uintptr_t)((uint32_t)k_ra8_flash_extra_start + offset);
  for (uint32_t i = 0U; i < len; i++) {
    dst[i] = src[i];
  }
#endif
  return k_ra8_ok;
}

/**
 * @brief Extra-MRAM write backend (::ra8_devcfg_write_fn_t).
 *
 * @details On silicon, programs ``len`` bytes through
 *          ``ra8_flash_extra_mram_write`` in ::k_ra8_devcfg_page_bytes pages
 *          (HUM Ch 59.7.4.5 "Program Command" Table 59.15 p 3592). Under
 *          simulation, copies into the RAM shadow.
 *
 * @param[in] offset Byte offset into the devcfg region.
 * @param[in] src    Source; non-NULL, at least ``len`` bytes.
 * @param[in] len    Bytes to write.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok             ``len`` bytes programmed.
 * @retval k_ra8_err_null_ptr   ``src`` was NULL.
 * @retval k_ra8_err_out_of_range ``offset + len`` leaves the devcfg region.
 * @retval other                Forwarded from ``ra8_flash_extra_mram_write``.
 *
 * @pre  ``src`` has ``len`` readable bytes; on silicon ``ra8_flash_init`` ran.
 * @pre  ``offset + len`` lies inside the devcfg region.
 * @post On success the window holds ``src[0 .. len)``.
 * @post On failure the region may be partially programmed.
 *
 * @note Not thread-safe; boot / provisioning path only.
 * @since 0.1.0
 */
RA8_INTERNAL
[[nodiscard]] static ra8_err_t
internal_devcfg_xm_write(uint32_t offset, const uint8_t* src, uint32_t len)
{
  RA8_CHECK_NULL_PTR(src, s_tag, "xm write: src null");
  if ((offset + len) > (uint32_t)k_ra8_devcfg_xm_span) {
    return k_ra8_err_out_of_range;
  }
#ifdef RA8_SIMULATOR_MODE
  (void)memcpy(&internal_devcfg_xm_shadow()[offset], src, (size_t)len);
  return k_ra8_ok;
#else
  /* Bounded by len/page_bytes (<= record_len/page_bytes = 4) chunks; each
   * chunk stays inside one 32-byte program page. */
  uint32_t done = 0U;
  while (done < len) {
    uint32_t chunk = len - done;
    if (chunk > (uint32_t)k_ra8_devcfg_page_bytes) {
      chunk = (uint32_t)k_ra8_devcfg_page_bytes;
    }
    const uint32_t  addr = (uint32_t)k_ra8_flash_extra_start + offset + done;
    const ra8_err_t err  = ra8_flash_extra_mram_write(addr, &src[done], chunk);
    RA8_RETURN_ON_ERROR(err, s_tag, "xm write: page program failed");
    done += chunk;
  }
  return k_ra8_ok;
#endif
}

/**
 * @var s_default_store
 * @brief The process-lifetime extra-MRAM-backed store.
 * @details Wires the read / write backends above; returned by
 *          ::ra8_devcfg_default_store.
 * @warning File-private; exposed only through the accessor.
 * @since 0.1.0
 */
static const ra8_devcfg_store_t s_default_store = {
  .read  = internal_devcfg_xm_read,
  .write = internal_devcfg_xm_write,
};

const ra8_devcfg_store_t* ra8_devcfg_default_store(void)
{
  return &s_default_store;
}
