/**
 * @file examples/ek_ra8d2/hw_pending/secure_boot_ns_hil/src/main.c
 * @brief Secure side: authenticate the NS image, then BLXNS -- the TrustZone RoT proof (#172).
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The end-to-end proof that the root of trust ENFORCES the TrustZone Secure->NS
 * boundary on this hardware. ``SystemInit`` -> ``ra8_trustzone_init`` has already
 * carved the SRAM2 NS aperture, programmed the SAU, and copied the (separately
 * flashed) NS image into the SRAM run base 0x3210_0000. This ``main()`` then:
 *
 *   1. Brings up the tf-psa-crypto static heap + the PSA facade.
 *   2. Calls ::ra8_tz_secure_boot_jump_ns, whose ``RA8_ENABLE_ROOT_OF_TRUST`` gate
 *      reads the NS image's ::ra8_ns_rot_header_t (at ns_base + 0x40) to learn the
 *      signed-body length, locates the ::ra8_rot_trailer_t at ns_base + body_len,
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
 * ## Observability (bench)
 *
 * The Secure side owns the security-critical verify decision, so it prints the
 * verdict over the J-Link OB VCOM console (SCI8) for ``uart_scrape``, since a
 * J-Link memprobe is unreliable on this TrustZone app (a debugger ``connect``
 * forces VC_CORERESET, which re-runs and faults the secure boot). The Secure
 * world has full peripheral access, so the console needs no NS/PSAR plumbing.
 * Boot-order breadcrumbs:
 *   - ``sbns: crypto ready``       -- PSA/crypto facade is up (pre-verify).
 *   - ``sbns: verify+enter NS``    -- about to verify + BLXNS into the NS image.
 *   - ``sbns: NS REJECTED err=0x...`` -- the deny path (jump_ns returned; the
 *     hex is ``g_sbns_jump_err``, e.g. 0x502 = ``k_ra8_err_checksum_mismatch``).
 * A GENUINE image prints the first two lines and never returns (BLXNS); a
 * TAMPERED image adds the REJECTED line. NS-side liveness is NOT UART-observable
 * (the minimal NS image owns no console peripheral and delegating SCI8 to NS
 * would need invasive PSAR/SAU changes); the ::g_sbns_ns_alive memprobe symbol
 * remains as the belt-and-suspenders NS-liveness probe.
 *
 * All diagnostics are ALSO latched into Secure ``.bss`` globals a J-Link halt
 * can read when the debug connection does not disturb the boot.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "mbedtls/memory_buffer_alloc.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_psa_crypto.h"
#include "ra8_tz_secure_boot.h"
#include "trustzone_init.h"

/** @brief Static-heap sizing for tf-psa-crypto's mbedtls_calloc (no libc heap). */
typedef enum : uint32_t {
  k_sbns_heap_bytes = 0x10000U, /**< 64 KiB static heap for the ECDSA verify. */
} sbns_const_t;

/**
 * @enum sbns_uart_t
 * @brief Secure-side J-Link OB VCOM (SCI8) console parameters.
 * @details The verdict breadcrumbs are emitted on the stock EK-RA8D2 debug
 *          console so ``uart_scrape`` can read them; 115200 8N1 matches the
 *          sibling HIL apps (e.g. ``fault_div0_hil``).
 */
typedef enum : uint32_t {
  k_sbns_uart_baud = 115200U, /**< SCI8 J-Link OB VCOM console line rate (bps). */
} sbns_uart_t;

/**
 * @enum sbns_hex_t
 * @brief Nibble-decode constants for the denial-code hexadecimal print.
 * @details Used only by ::sbns_console_print_hex32 to render ``g_sbns_jump_err``
 *          as ``0x...`` on the console. No hardware meaning.
 */
typedef enum : uint8_t {
  k_sbns_hex_nibbles    = 8U,  /**< Hex nibbles in a uint32 (32 / 4).           */
  k_sbns_hex_bits       = 4U,  /**< Bits per hex nibble.                        */
  k_sbns_hex_dec_max    = 9U,  /**< Highest nibble emitted as an ASCII '0'-'9'. */
  k_sbns_hex_alpha_base = 10U, /**< First nibble emitted as ASCII 'a'.          */
} sbns_hex_t;

/**
 * @enum sbns_hex_mask_t
 * @brief Bit mask isolating one hexadecimal nibble from a shifted word.
 * @details Separate ``uint32_t`` enum because a mask is not an index/count.
 */
typedef enum : uint32_t {
  k_sbns_hex_nibble_mask = 0xFU, /**< Low-nibble mask for a uint32 shift result. */
} sbns_hex_mask_t;

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
 * @brief The ``ra8_err_t`` ::ra8_tz_secure_boot_jump_ns returned on a denial.
 * @details Captured only on the tampered path (a genuine boot BLXNS-es and never
 *          returns). Expected value: ``k_ra8_err_checksum_mismatch`` for a flipped
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
 * @var s_sbns_console_up
 * @brief True once the Secure J-Link OB VCOM console is initialised.
 * @details Set by ::sbns_console_bringup on success; the ::sbns_console_say
 *          helpers no-op while it is false so a dead console can never gate the
 *          security-critical verify.
 * @note File-private; boot context only, single-threaded.
 * @warning Do not set from outside ::sbns_console_bringup.
 * @since 0.1.0
 */
static bool s_sbns_console_up = false;

/** @brief UART breadcrumb: crypto/PSA facade is up (printed pre-verify). */
static const uint8_t k_sbns_msg_crypto_ready[] = "sbns: crypto ready\r\n";

/** @brief UART breadcrumb: about to verify + BLXNS into the NS image. */
static const uint8_t k_sbns_msg_verify_enter[] = "sbns: verify+enter NS\r\n";

/** @brief UART verdict prefix: RoT denied the NS image (followed by the hex code). */
static const uint8_t k_sbns_msg_reject[] = "sbns: NS REJECTED err=0x";

/** @brief UART line terminator appended after the denial error code. */
static const uint8_t k_sbns_msg_crlf[] = "\r\n";

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

/**
 * @brief Bring up the Secure J-Link OB VCOM console (best-effort).
 *
 * @details
 * Clears the module-stop refcounts (``ra8_mstp_init``) and initialises SCI8 on
 * PD02/PD03 (``ra8_board_uart_console_init``) at ::k_sbns_uart_baud. ``ra8_cgc_init``
 * already ran in ``SystemInit``, so PCLKA is post-PLL and the BRR divisor is
 * valid. This is BEST-EFFORT: on any failure ::s_sbns_console_up stays false and
 * the ::sbns_console_say helpers silently no-op, so a dead console never blocks
 * the security-critical verify. No hardware crypto uses the modules gated here,
 * so running it after ``ra8_psa_crypto_init`` leaves the verify unaffected.
 *
 * @pre ``ra8_cgc_init`` has run (PCLKA is post-PLL) -- done in ``SystemInit``.
 * @pre Secure state, single-threaded boot context.
 * @post ::s_sbns_console_up is true iff both MSTP and console init returned ok.
 * @post No global state other than ::s_sbns_console_up + the SCI8/PFS registers
 *       is modified.
 * @note Not thread-safe; call once during boot.
 * @since 0.1.0
 */
static void sbns_console_bringup(void)
{
  if (ra8_mstp_init() != k_ra8_ok) {
    return;
  }
  if (ra8_board_uart_console_init((uint32_t)k_sbns_uart_baud) != k_ra8_ok) {
    return;
  }
  s_sbns_console_up = true;
}

/**
 * @brief Emit a byte run on the Secure console (no-op until it is up).
 *
 * @details Thin best-effort wrapper: guards on ::s_sbns_console_up and a NULL
 *          buffer, then forwards to the polled console write, discarding its
 *          status (console loss is acceptable for a breadcrumb; the HIL gate
 *          re-checks the output on the wire).
 *
 * @param[in] msg Bytes to transmit (not NUL-inspected); ignored when NULL.
 * @param[in] len Number of bytes in ``msg``.
 *
 * @pre ``msg`` points at ``len`` readable bytes when non-NULL.
 * @pre The console is up (else the call is a no-op).
 * @post ``len`` bytes were pushed to the console, or nothing on a guard miss.
 * @post No other state modified.
 * @note Not thread-safe (single-threaded boot).
 * @since 0.1.0
 */
static void sbns_console_say(const uint8_t* msg, uint32_t len)
{
  if (!s_sbns_console_up) {
    return;
  }
  if (msg == nullptr) {
    return;
  }
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/**
 * @brief Print a uint32 as hexadecimal on the console, no leading zeros.
 *
 * @details
 * Renders ``value`` MSB-first into an 8-nibble scratch buffer, then emits from
 * the first non-zero nibble (keeping at least one digit, so 0 prints "0"). Used
 * to append ``g_sbns_jump_err`` after the ``err=0x`` prefix.
 *
 * @param[in] value 32-bit value to render (a denial ``ra8_err_t`` code here).
 *
 * @pre The console is up (else the call is a no-op).
 * @pre ``value`` is any uint32 (full range supported).
 * @post The hexadecimal text was pushed to the console, or nothing when down.
 * @post No other state modified.
 * @note Not thread-safe (single-threaded boot).
 * @since 0.1.0
 */
static void sbns_console_print_hex32(uint32_t value)
{
  if (!s_sbns_console_up) {
    return;
  }
  uint8_t buf[k_sbns_hex_nibbles];
  for (uint8_t i = 0U; i < (uint8_t)k_sbns_hex_nibbles; i++) {
    const uint32_t shift =
      (uint32_t)((uint8_t)k_sbns_hex_nibbles - 1U - i) * (uint32_t)k_sbns_hex_bits;
    const uint8_t nib = (uint8_t)((value >> shift) & (uint32_t)k_sbns_hex_nibble_mask);
    buf[i]            = (nib <= (uint8_t)k_sbns_hex_dec_max)
                          ? (uint8_t)('0' + nib)
                          : (uint8_t)('a' + (nib - (uint8_t)k_sbns_hex_alpha_base));
  }
  uint8_t first = 0U;
  while (first < (uint8_t)((uint8_t)k_sbns_hex_nibbles - 1U)) {
    if (buf[first] != (uint8_t)'0') {
      break;
    }
    first++;
  }
  (void)ra8_board_uart_console_write(&buf[first], (size_t)((uint8_t)k_sbns_hex_nibbles - first));
}

/**
 * @brief Print the ``sbns: NS REJECTED err=0x...`` verdict line for a denial.
 *
 * @details Composes the fixed prefix, the ``err`` code in hexadecimal, and a
 *          CRLF. Called only on the default-deny path (jump_ns returned).
 *
 * @param[in] err The ``ra8_err_t`` denial code from ::ra8_tz_secure_boot_jump_ns.
 *
 * @pre Reached from the deny path (the NS image was rejected).
 * @pre The console is up (else the call is a no-op).
 * @post The full REJECTED line was pushed to the console, or nothing when down.
 * @post No other state modified.
 * @note Not thread-safe (single-threaded terminal path).
 * @since 0.1.0
 */
static void sbns_console_say_reject(ra8_err_t err)
{
  sbns_console_say(k_sbns_msg_reject, (uint32_t)(sizeof(k_sbns_msg_reject) - 1U));
  sbns_console_print_hex32((uint32_t)err);
  sbns_console_say(k_sbns_msg_crlf, (uint32_t)(sizeof(k_sbns_msg_crlf) - 1U));
}

void main(void)
{
  /* Bring up the crypto heap + PSA facade the root-of-trust verify needs. The
   * SAU + NS-image copy already ran in ra8_trustzone_init (SystemInit). */
  mbedtls_memory_buffer_alloc_init(s_sbns_heap, sizeof(s_sbns_heap));
  const ra8_err_t psa_err = ra8_psa_crypto_init();
  if ((psa_err != k_ra8_ok) && (psa_err != k_ra8_err_exists)) {
    g_sbns_step = k_sbns_step_psa_fail;
    sbns_park();
  }
  g_sbns_step = k_sbns_step_crypto;

  /* Bring up the Secure UART console and announce the crypto milestone so the
   * boot verdict is scrape-able on the bench. Best-effort: a dead console must
   * never gate the security decision, so the prints no-op if it failed. */
  sbns_console_bringup();
  sbns_console_say(k_sbns_msg_crypto_ready, (uint32_t)(sizeof(k_sbns_msg_crypto_ready) - 1U));

  /* Authenticate + jump. The RA8_ENABLE_ROOT_OF_TRUST gate inside jump_ns reads
   * the NS RoT header for the body length, verifies SHA-256 + ECDSA-P256, and
   * BLXNS-es ONLY on success. A genuine image never returns here. */
  g_sbns_step = k_sbns_step_armed;
  sbns_console_say(k_sbns_msg_verify_enter, (uint32_t)(sizeof(k_sbns_msg_verify_enter) - 1U));
  const ra8_err_t jump_err =
    ra8_tz_secure_boot_jump_ns((const uint32_t*)(uintptr_t)k_sbns_ns_run_base);

  /* Reached only when the gate DENIED the NS image (tampered / unsigned): the
   * default-deny path. Latch + print the outcome for the bench and halt -- the
   * NS world never ran, so g_sbns_ns_alive stays 0. */
  g_sbns_jump_err = (uint32_t)jump_err;
  g_sbns_denied   = 1U;
  g_sbns_step     = k_sbns_step_denied;
  sbns_console_say_reject(jump_err);
  sbns_park();
}
