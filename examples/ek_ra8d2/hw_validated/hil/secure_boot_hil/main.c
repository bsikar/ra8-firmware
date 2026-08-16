/**
 * @file examples/ek_ra8d2/hw_validated/hil/secure_boot_hil/main.c
 * @brief On-silicon enforcing secure boot: reject a tampered image, launch the signed one
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The end-to-end proof that the root of trust ENFORCES the boot on this
 * hardware. With RA8_ENABLE_ROOT_OF_TRUST, ::ra8_dfu_launch verifies an image's
 * ECDSA-P256 signature (SHA-256 body digest, via tf-psa-crypto) AND the
 * extra-MRAM anti-rollback floor BEFORE copying it to the SRAM run base and
 * branching -- default-deny on any failure.
 *
 * This app embeds one RoT-signed copy-to-run image (``signed_payload.h``,
 * dfu_copy_to_run's 32-byte payload signed by tools/rot_sign.py at img_version
 * 7) and:
 *   1. Flips one body byte and hands the corrupted copy to ::ra8_dfu_launch --
 *      the signature check must fail, so the launch RETURNS (default-deny) and
 *      nothing is copied or branched. Prints "secure-boot: tampered REJECTED".
 *   2. Hands the genuine signed image to ::ra8_dfu_launch -- signature +
 *      anti-rollback pass, so it copies to k_ra8_dfu_run_base and BRANCHES there
 *      (never returns). The launched payload advances a heartbeat at
 *      0x22010004, so a J-Link mem probe confirms the authentic image actually
 *      executed. Prints "secure-boot: ENFORCING OK" right before the branch.
 *
 * A return from the genuine launch would mean the RoT rejected an authentic
 * image (a bug) -- it prints "secure-boot: LAUNCH FAILED" and halts.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "mbedtls/memory_buffer_alloc.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_dfu.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_psa_crypto.h"
#include "ra8_time.h"
#include "signed_payload.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_sb_baud       = 115200U,  /**< Sb baud.                                      */
  k_sb_heap_bytes = 0x10000U, /**< 64 KiB static heap for tf-psa mbedtls_calloc. */
} sb_const_t;

/** @brief Byte flipped in the tampered copy (first body octet). */
typedef enum : uint8_t {
  k_sb_tamper_bit = 0x01U, /**< Sb tamper bit. */
} sb_bit_t;

static const uint8_t k_sb_msg_start[]     = "secure-boot: start\r\n";
static const uint8_t k_sb_msg_psa_fail[]  = "secure-boot: psa_init FAIL\r\n";
static const uint8_t k_sb_msg_rejected[]  = "secure-boot: tampered REJECTED\r\n";
static const uint8_t k_sb_msg_no_reject[] = "secure-boot: tampered ACCEPTED (bug!)\r\n";
static const uint8_t k_sb_msg_enforcing[] = "secure-boot: ENFORCING OK -- launching authentic\r\n";
static const uint8_t k_sb_msg_launchfail[] =
  "secure-boot: LAUNCH FAILED (authentic rejected -- bug)\r\n";

/** @brief Static heap tf-psa's mbedtls_calloc draws from (no libc heap on target). */
static uint8_t s_sb_heap[k_sb_heap_bytes];

/** @brief Mutable copy of the signed image used to build the tampered case. */
static uint8_t s_sb_tampered[sizeof(g_sb_signed_payload)];

static void sb_write(const uint8_t* msg, size_t len)
{
  (void)ra8_board_uart_console_write(msg, len);
}

static void sb_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

static void sb_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    sb_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    sb_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    sb_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_sb_baud) != k_ra8_ok) {
    sb_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    sb_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    sb_panic_halt();
  }
  sb_write(k_sb_msg_start, (size_t)(sizeof(k_sb_msg_start) - 1U));
  mbedtls_memory_buffer_alloc_init(s_sb_heap, sizeof(s_sb_heap));
  if (ra8_psa_crypto_init() != k_ra8_ok) {
    sb_write(k_sb_msg_psa_fail, (size_t)(sizeof(k_sb_msg_psa_fail) - 1U));
    sb_panic_halt();
  }
}

/**
 * @brief Confirm a one-bit body corruption is rejected by the launch gate.
 * @return true if ::ra8_dfu_launch returned (default-deny) without launching.
 */
static bool sb_tampered_rejected(void)
{
  memcpy(s_sb_tampered, g_sb_signed_payload, sizeof(s_sb_tampered));
  s_sb_tampered[0] ^= (uint8_t)k_sb_tamper_bit;
  /* A rejected image makes ra8_dfu_launch return without copying or branching.
   * If it instead launched the corrupted image, control would never come back
   * here -- so reaching the next line at all proves the default-deny. */
  ra8_dfu_launch((uintptr_t)s_sb_tampered,
                 (uint32_t)k_sb_payload_body_len,
                 (uint32_t)k_ra8_dfu_run_base);
  return true;
}

void main(void)
{
  sb_setup_or_halt();
  ra8_isr_globals_enable();

  /* Step 1: the tampered image must be rejected (launch returns). */
  if (sb_tampered_rejected()) {
    sb_write(k_sb_msg_rejected, (size_t)(sizeof(k_sb_msg_rejected) - 1U));
    (void)ra8_board_led_toggle(k_ra8_board_led1);
  } else {
    sb_write(k_sb_msg_no_reject, (size_t)(sizeof(k_sb_msg_no_reject) - 1U));
    (void)ra8_board_led_toggle(k_ra8_board_led2);
    sb_panic_halt();
  }

  /* Step 2: the genuine signed image must verify + pass anti-rollback, then be
   * copied to the run base and branched to -- ra8_dfu_launch does not return. */
  sb_write(k_sb_msg_enforcing, (size_t)(sizeof(k_sb_msg_enforcing) - 1U));
  ra8_dfu_launch((uintptr_t)g_sb_signed_payload,
                 (uint32_t)k_sb_payload_body_len,
                 (uint32_t)k_ra8_dfu_run_base);

  /* Only reached if the RoT rejected an authentic image -- a bug. */
  sb_write(k_sb_msg_launchfail, (size_t)(sizeof(k_sb_msg_launchfail) - 1U));
  (void)ra8_board_led_toggle(k_ra8_board_led2);
  sb_panic_halt();
}
