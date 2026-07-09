/**
 * @file examples/ek_ra8d2/hw_pending/secure_boot_ns_hil/main.c
 * @brief Secure side: authenticate the NS image, then BLXNS -- the TrustZone RoT proof (#172).
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The end-to-end proof that the root of trust ENFORCES the TrustZone Secure->NS
 * boundary on this hardware. ``SystemInit`` -> ``ra_trustzone_init`` has already
 * carved the SRAM2 NS aperture, programmed the SAU, and copied the (separately
 * flashed) NS image into the SRAM run base 0x3210_0000. This ``main()`` then:
 *
 *   1. Brings up the tf-psa-crypto static heap + the PSA facade.
 *   2. Calls ::ra_tz_secure_boot_jump_ns, whose ``RA_ENABLE_ROOT_OF_TRUST`` gate
 *      reads the NS image's ::ra_ns_rot_header_t (at ns_base + 0x40) to learn the
 *      signed-body length, locates the ::ra_rot_trailer_t at ns_base + body_len,
 *      re-computes SHA-256 + verifies the ECDSA-P256 signature, and only then
 *      arms VTOR_NS + BLXNS.
 *
 * Two flashable artifacts drive the two outcomes (identical Secure half):
 *   - **Genuine** signed NS image -> verify passes -> BLXNS -> the NS reset
 *     handler advances ::g_sbns_ns_alive forever (a J-Link memprobe sees it
 *     climb). ``main()`` never returns (BLXNS left Secure state).
 *   - **Tampered** NS image (one flipped body byte) -> digest mismatch ->
 *     default-deny -> ``jump_ns`` RETURNS an error -> ``main()`` latches
 *     ::g_sbns_denied = 1 + ::g_sbns_jump_err and parks. ::g_sbns_ns_alive
 *     stays 0 (the NS world never ran).
 *
 * All diagnostics are Secure ``.bss`` globals a J-Link halt can read.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "mbedtls/memory_buffer_alloc.h"
#include "ra_err.h"
#include "ra_psa_crypto.h"
#include "ra_tz_secure_boot.h"
#include "trustzone_init.h"

/** @brief Static-heap sizing for tf-psa-crypto's mbedtls_calloc (no libc heap). */
typedef enum : uint32_t {
  k_sbns_heap_bytes = 0x10000U, /**< 64 KiB static heap for the ECDSA verify. */
} sbns_const_t;

/**
 * @enum sbns_step_t
 * @brief Boot-progress breadcrumbs latched into ::g_sbns_step.
 * @details Read via J-Link to localise where the Secure boot wedged if the NS
 *          world never comes alive.
 */
typedef enum : uint32_t {
  k_sbns_step_idle     = 0U, /**< Pre-main sentinel.                    */
  k_sbns_step_crypto   = 1U, /**< Heap + PSA facade initialised.        */
  k_sbns_step_armed    = 2U, /**< About to call jump_ns (verify+BLXNS). */
  k_sbns_step_denied   = 3U, /**< jump_ns returned -> NS image denied.  */
  k_sbns_step_psa_fail = 9U, /**< PSA init failed (crypto unavailable). */
} sbns_step_t;

/**
 * @var g_sbns_step
 * @brief Secure boot progress breadcrumb (::sbns_step_t).
 * @details Advanced at each Secure-side milestone. On a genuine boot it freezes
 *          at ::k_sbns_step_armed (BLXNS never returns); on a tampered boot it
 *          reaches ::k_sbns_step_denied.
 * @note Read externally by J-Link only.
 * @warning Do not modify from outside ``main()``.
 * @since 0.1.0
 */
volatile uint32_t g_sbns_step = k_sbns_step_idle;

/**
 * @var g_sbns_denied
 * @brief Set to 1 when the root-of-trust gate DENIED the NS image (tampered).
 * @details Stays 0 on a genuine boot (BLXNS never returns to set it). A J-Link
 *          read of 1 confirms default-deny fired and the NS world never ran.
 * @note Read externally by J-Link only.
 * @warning Do not modify from outside ``main()``.
 * @since 0.1.0
 */
volatile uint32_t g_sbns_denied;

/**
 * @var g_sbns_jump_err
 * @brief The ``ra_err_t`` ::ra_tz_secure_boot_jump_ns returned on a denial.
 * @details Captured only on the tampered path (a genuine boot BLXNS-es and never
 *          returns). Expected value: ``k_ra_err_checksum_mismatch`` for a flipped
 *          body byte (the digest pre-check fails).
 * @note Read externally by J-Link only.
 * @warning Do not modify from outside ``main()``.
 * @since 0.1.0
 */
volatile uint32_t g_sbns_jump_err;

/**
 * @var s_sbns_heap
 * @brief Static heap tf-psa-crypto's mbedtls_calloc draws from.
 * @details Secure-side ``.bss`` (below the SRAM2 NS boundary, so it stays
 *          Secure). Handed to ``mbedtls_memory_buffer_alloc_init`` before the
 *          first PSA call.
 * @note File-private; the allocator owns it after init.
 * @warning Do not access directly after ``mbedtls_memory_buffer_alloc_init``.
 * @since 0.1.0
 */
static uint8_t s_sbns_heap[k_sbns_heap_bytes];

/**
 * @brief Park the Secure core in WFI forever.
 * @details Terminal halt used on every non-BLXNS exit so a J-Link halt leaves
 *          the diagnostic globals frozen.
 * @return Never returns.
 * @pre Reached from a terminal boot outcome.
 * @pre The diagnostic globals reflect the reason.
 * @post The core spins in WFI.
 * @post No further work is done.
 * @note Single-threaded.
 * @since 0.1.0
 */
[[noreturn]] static void sbns_park(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  /* Bring up the crypto heap + PSA facade the root-of-trust verify needs. The
   * SAU + NS-image copy already ran in ra_trustzone_init (SystemInit). */
  mbedtls_memory_buffer_alloc_init(s_sbns_heap, sizeof(s_sbns_heap));
  const ra_err_t psa_err = ra_psa_crypto_init();
  if ((psa_err != k_ra_ok) && (psa_err != k_ra_err_exists)) {
    g_sbns_step = k_sbns_step_psa_fail;
    sbns_park();
  }
  g_sbns_step = k_sbns_step_crypto;

  /* Authenticate + jump. The RA_ENABLE_ROOT_OF_TRUST gate inside jump_ns reads
   * the NS RoT header for the body length, verifies SHA-256 + ECDSA-P256, and
   * BLXNS-es ONLY on success. A genuine image never returns here. */
  g_sbns_step = k_sbns_step_armed;
  const ra_err_t jump_err =
    ra_tz_secure_boot_jump_ns((const uint32_t*)(uintptr_t)k_sbns_ns_run_base);

  /* Reached only when the gate DENIED the NS image (tampered / unsigned): the
   * default-deny path. Latch the outcome for the bench and halt -- the NS world
   * never ran, so g_sbns_ns_alive stays 0. */
  g_sbns_jump_err = (uint32_t)jump_err;
  g_sbns_denied   = 1U;
  g_sbns_step     = k_sbns_step_denied;
  sbns_park();
  return 0;
}
#pragma GCC diagnostic pop
