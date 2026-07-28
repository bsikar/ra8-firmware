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

#include "ra8_dfu.h"
#ifdef RA8_ENABLE_ROOT_OF_TRUST
#include "ra8_dfu_antirollback.h"
#include "ra8_rot.h"
#endif

/**
 * @enum ra8_dfu_launch_scb_t
 * @brief Cortex-M85 System Control Block address used by the hand-off.
 * @details VTOR is the Secure alias; the bootloader and apps run Secure. The
 *          Renesas HUM defers the Cortex-M85 core (SCB) registers to the ARMv8-M
 *          ARM (B3.2.2 "VTOR, Vector Table Offset Register").
 */
typedef enum : uintptr_t {
  k_ra8_dfu_launch_vtor_addr = 0xE000ED08U, /**< SCB->VTOR (Secure). */
} ra8_dfu_launch_scb_t;

void ra8_dfu_launch(uintptr_t src, uint32_t img_len, uint32_t entry)
{
  if (src == 0U) {
    return; /* nothing to copy */
  }
  if (!ra8_dfu_run_target_valid(entry, img_len)) {
    return; /* entry not the run base, or bad length -> caller drops to fallback */
  }

#ifdef RA8_ENABLE_ROOT_OF_TRUST
  /* Root-of-trust gate (opt-in, see ra8_rot.h): authenticate the signed image
   * (SHA-256 + ECDSA-P256) BEFORE copying it to the run base. Default-deny --
   * any failure (missing trailer, malformed trailer, tampered body, or an
   * invalid signature) returns to the caller's fallback path without copying or
   * branching. The signed image is `[ body (img_len) ] [ ra8_rot_trailer_t ]`,
   * so the trailer sits immediately after the body. With the flag OFF this gate
   * is absent and the launch proceeds on the CRC32 integrity check alone. */
  const ra8_rot_trailer_t* trailer = ra8_rot_trailer_after((const void*)src, img_len);
  if (ra8_rot_verify_image((const uint8_t*)src, img_len, trailer) != k_ra8_ok) {
    return; /* DEFAULT-DENY: unauthenticated image -> do not launch */
  }

  /* Anti-rollback gate (downgrade protection): the image is now authentic, so
   * its trailer is readable. Refuse a version older than the highest ever
   * accepted (see ra8_dfu_antirollback.h). DEFAULT-DENY on a downgrade OR a store
   * READ FAULT -- either returns to the caller's fallback path without copying or
   * branching. A fresh/unprovisioned device reads its erased monotonic counter as
   * 0, so it ACCEPTS its first image (which becomes the new floor) and is NOT
   * bricked; only a genuine downgrade or an unreadable counter denies launch. */
  if (ra8_rot_antirollback_verify(ra8_rot_antirollback_default_store(), trailer->img_version) !=
      k_ra8_ok) {
    return; /* DEFAULT-DENY: downgrade / unprovisioned counter -> do not launch */
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

  *(volatile uint32_t*)(uintptr_t)k_ra8_dfu_launch_vtor_addr = (uint32_t)k_ra8_dfu_run_base;
  __asm__ volatile("dsb 0xF\n isb 0xF\n" ::: "memory");
  __asm__ volatile("msr msp, %0\n"
                   "bx %1\n"
                   :
                   : "r"(initial_sp), "r"(reset_entry)
                   : "memory");
  __builtin_unreachable();
#endif /* !RA8_OFF_TARGET */
}
