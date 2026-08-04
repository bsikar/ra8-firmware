/**
 * @file ra8_dfu_launch.c
 * @brief Copy-to-run launch: copy an image to the SRAM run base and branch to it.
 *
 * @par Tag
 * [Ring 4 / Service] {World: S}
 *
 * @details
 * The shared copy-to-run hand-off behind the `dfu_bootloader` (which launches a
 * validated slot body) and the `dfu_copy_to_run` HIL demo (which launches an
 * embedded image). It copies `img_len` bytes from `src` to the fixed SRAM run
 * base (::k_ra8_dfu_run_base), then points the Secure VTOR there, loads the
 * image's initial MSP, and branches to its reset vector -- a same-world (Secure)
 * branch (the reset vector keeps its Thumb bit). Because every payload is linked
 * at the one run base, the same image runs wherever it was staged.
 *
 * The destination is the trusted ::k_ra8_dfu_run_base constant, never a
 * caller-supplied `entry`, so a corrupted `entry` cannot redirect the copy: it
 * only fails the ::ra8_dfu_run_target_valid cross-check and the launch returns.
 *
 * Authenticity is enforced BEFORE any copy: the signed image carries a
 * ::ra8_rot_trailer_t after its body, and ::ra8_rot_verify_image must approve the
 * SHA-256 + ECDSA-P256 signature against the provisioned root public key. The
 * historical CRC32 (checked by the caller via ::ra8_dfu_hdr_valid) remains as an
 * integrity pre-check, but the signature is the authority -- on any signature /
 * hash / trailer failure this function default-denies: it copies nothing,
 * branches nowhere, and simply returns to the caller's fallback path.
 *
 * The I/D caches are disabled on the EK-RA8D2 (libs/ra8_board_ek_ra8d2/boot/
 * system_init.c keeps them off), so the write-then-execute is made coherent by a
 * DSB after the copy plus a DSB/ISB before the branch -- no cache maintenance.
 * If caches are ever enabled there, a clean-DCache + invalidate-ICache must be
 * added before the branch.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_attributes.h"
#include "ra8_dfu.h"
#include "ra8_scb.h"
#ifdef RA8_ENABLE_ROOT_OF_TRUST
#include "ra8_dfu_antirollback.h"
#include "ra8_rot.h"
#endif

#ifdef RA8_ENABLE_ROOT_OF_TRUST
/**
 * @brief Decide whether a staged image is allowed to launch.
 *
 * @details
 * The two default-deny gates that must both pass before a single byte is copied
 * to the run base. They live together because they share the trailer they read.
 *
 * The root-of-trust gate authenticates the signed image (SHA-256 + ECDSA-P256)
 * against the provisioned root public key. The signed image is
 * ``[ body (img_len) ] [ ra8_rot_trailer_t ]``, so the trailer sits immediately
 * after the body; a missing trailer, a malformed trailer, a tampered body or an
 * invalid signature all deny.
 *
 * The anti-rollback gate then refuses a version older than the highest ever
 * accepted. A store READ FAULT denies as well as a genuine downgrade. A fresh
 * or unprovisioned device reads its erased monotonic counter as 0, so it
 * ACCEPTS its first image -- which becomes the new floor -- and is NOT bricked.
 *
 * @param[in] src     Address of the staged image body.
 * @param[in] img_len Length of the image body in bytes, excluding the trailer.
 *
 * @return Whether the image passed both gates.
 * @retval true  The signature verified and the version is not a downgrade.
 * @retval false A gate failed; the caller must not copy and must not branch.
 *
 * @pre ``src`` is non-zero and ::ra8_dfu_run_target_valid has already approved
 *      the ``entry`` / ``img_len`` pair.
 * @pre The root public key is provisioned, and the monotonic counter is either
 *      provisioned or reads as erased on a fresh device.
 * @post No image bytes have been copied and no vector table has been relocated.
 * @post No durable counter has been advanced.
 *
 * @note Not thread-safe; boot-path only.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_launch_authorized(uintptr_t src, uint32_t img_len)
{
  const ra8_rot_trailer_t* trailer = ra8_rot_trailer_after((const void*)src, img_len);
  if (ra8_rot_verify_image((const uint8_t*)src, img_len, trailer) != k_ra8_ok) {
    return false; /* DEFAULT-DENY: unauthenticated image */
  }
  if (ra8_rot_antirollback_verify(ra8_rot_antirollback_default_store(), trailer->img_version) !=
      k_ra8_ok) {
    return false; /* DEFAULT-DENY: downgrade, or an unreadable counter */
  }
  return true;
}
#endif /* RA8_ENABLE_ROOT_OF_TRUST */

void ra8_dfu_launch(uintptr_t src, uint32_t img_len, uint32_t entry)
{
  if (src == 0U) {
    return; /* nothing to copy */
  }
  if (!ra8_dfu_run_target_valid(entry, img_len)) {
    return; /* entry not the run base, or bad length -> caller drops to fallback */
  }

#ifdef RA8_ENABLE_ROOT_OF_TRUST
  /* Opt-in (see ra8_rot.h). With the flag OFF both gates are absent and the
   * launch proceeds on the CRC32 integrity check alone. */
  if (!internal_launch_authorized(src, img_len)) {
    return; /* DEFAULT-DENY -> caller's fallback path, nothing copied */
  }
#endif /* RA8_ENABLE_ROOT_OF_TRUST */

#ifndef RA8_OFF_TARGET
  const volatile uint32_t* s     = (const volatile uint32_t*)src;
  volatile uint32_t*       d     = (volatile uint32_t*)(uintptr_t)k_ra8_dfu_run_base;
  const uint32_t           words = img_len / (uint32_t)sizeof(uint32_t);

  __asm__ volatile("cpsid i" ::: "memory");
  /* Bounded by img_len (<= k_ra8_dfu_img_max via ra8_dfu_run_target_valid): a
   * statically bounded copy -- NASA Rule 2 compliant. */
  for (uint32_t i = 0U; i < words; ++i) {
    d[i] = s[i];
  }
  __asm__ volatile("dsb 0xF" ::: "memory"); /* image stores reach SRAM before fetch */

  const uint32_t initial_sp  = d[0];
  const uint32_t reset_entry = d[1];

  /* Point the Secure VTOR at the run base via the shared ra8_scb primitive,
   * then fence: the vector fetch on the coming branch must see the new base. */
  ra8_scb_set_vtor((uintptr_t)k_ra8_dfu_run_base);
  __asm__ volatile("dsb 0xF\n isb 0xF\n" ::: "memory");
  __asm__ volatile("msr msp, %0\n"
                   "bx %1\n"
                   :
                   : "r"(initial_sp), "r"(reset_entry)
                   : "memory");
  __builtin_unreachable();
#endif /* !RA8_OFF_TARGET */
}
