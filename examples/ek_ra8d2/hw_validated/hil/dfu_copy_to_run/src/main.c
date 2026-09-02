/**
 * @file examples/ek_ra8d2/hw_validated/hil/dfu_copy_to_run/src/main.c
 * @brief Copy-to-run HIL demo: launch one embedded image at the SRAM run base.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The unattended, self-contained proof of the dfu_bootloader's **copy-to-run**
 * scheme (issue #97): an image linked ONCE at the SRAM run base
 * (::k_ra8_dfu_run_base) runs from wherever it is staged, with no per-slot build.
 *
 * This app embeds that image (`payload_image.h`, generated from `payload.c` by
 * `examples/ek_ra8d2/hw_validated/hil/dfu_copy_to_run/scripts/build_payload.sh`)
 * and hands it to the shared ::ra8_dfu_launch -- the exact
 * launcher the bootloader uses for a validated slot. ra8_dfu_launch copies the
 * image to ::k_ra8_dfu_run_base and branches there; the payload then spins in the
 * SRAM run window forever (writing a sentinel + heartbeat at a fixed probe word).
 *
 * ## How HIL verifies it (alive gate)
 *
 * `hil.conf` is `HIL_MODE=alive`. After the hand-off the PC lives in the SRAM
 * copy-to-run window (0x22020000+) -- a region only reachable BY copying-to-run
 * and branching there, since this app's own code is in MRAM. So "PC in the SRAM
 * run window, CycleCnt advancing, CFSR/HFSR clean, not in a fault spinner" is a
 * specific proof that copy-to-run executed. If the launch's run-target check
 * fails (it never should: a fixed run base + a valid length), control returns
 * and parks in ::internal_dcr_panic_halt, which the alive gate flags as a fault spinner.
 *
 * Unlike the bootloader's alive gate (which can also pass via the DFU-device
 * fallback when no slot is valid), this app ALWAYS copies-to-run, so its alive
 * pass is unambiguous.
 *
 * @author Brighton Sikarskie
 * @date 2026-06-16
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "payload_image.h"
#include "ra8_attributes.h"
#include "ra8_boot_entry.h"
#include "ra8_dfu.h"

/**
 * @enum dcr_vt_t
 * @brief Sanity-check masks for the embedded image's vector table.
 */
typedef enum : uint32_t {
  k_dcr_sram_mask = 0xFF000000U, /**< Top byte of an SRAM address.           */
  k_dcr_sram_base = 0x22000000U, /**< SRAM window base (initial-SP check).   */
  k_dcr_thumb_bit = 0x00000001U, /**< Reset vector must carry the Thumb bit. */
} dcr_vt_t;

/**
 * @enum dcr_probe_t
 * @brief Fixed SRAM probe-word addresses the copied-to-run payload writes.
 *
 * @details Mirror of payload.c's `k_payload_probe_sentinel` /
 * `k_payload_probe_counter`. Once ::ra8_dfu_launch branches into it, the payload
 * (running from the SRAM run window) writes a proof-of-run sentinel and a
 * free-running heartbeat at these two fixed words. They sit above this app's
 * low-SRAM `.bss`/`.data` and below the run base (0x22020000), so neither this
 * app nor the payload image overwrites them. linker_script.ld pins
 * ::g_dcr_run_sentinel / ::g_dcr_run_heartbeat here so the J-Link memprobe gate
 * (HIL_MODE=jlink_memprobe) can resolve the heartbeat by name.
 */
typedef enum : uintptr_t {
  k_dcr_probe_sentinel_addr = 0x22010000U, /**< Sentinel word address.  */
  k_dcr_probe_counter_addr  = 0x22010004U, /**< Heartbeat word address. */
} dcr_probe_t;

/**
 * @var g_dcr_run_sentinel
 * @brief Memprobe anchor for the payload's proof-of-run sentinel (0x22010000).
 * @details A reserved 4-byte word pinned by linker_script.ld to
 *          ::k_dcr_probe_sentinel_addr. This app never touches it -- the
 *          copied-to-run payload writes `k_payload_sentinel_value` here. It
 *          exists only so `nm` can resolve a named symbol at the sentinel word.
 * @warning Written exclusively by the SRAM-resident payload, never by this app.
 * @since 0.1.0
 */
[[gnu::section(".dfu_probe.sentinel"), gnu::used]] volatile uint32_t g_dcr_run_sentinel;

/**
 * @var g_dcr_run_heartbeat
 * @brief Memprobe liveness counter the copied-to-run payload advances (0x22010004).
 * @details A reserved 4-byte word pinned by linker_script.ld to
 *          ::k_dcr_probe_counter_addr. The payload increments the word at this
 *          address forever once copy-to-run branches into it, so
 *          hil_jlink_memprobe.sh sees it advance -- proof the image ran via
 *          copy-to-run (the only writer of this word). This app reserves the
 *          word but never writes it.
 * @warning Written exclusively by the SRAM-resident payload, never by this app.
 * @since 0.1.0
 */
[[gnu::section(".dfu_probe.heartbeat"), gnu::used]] volatile uint32_t g_dcr_run_heartbeat;

/**
 * @brief Park forever in WFI -- copy-to-run did not happen (alive gate fails).
 * @return Does not return.
 * @pre Reached only when ::ra8_dfu_launch returned (run-target check failed).
 * @pre Interrupts are in any state.
 * @post The CPU spins; the alive gate sees a fault-spinner PC and fails.
 * @post No further app code runs.
 * @note Named `*panic_halt` so the HIL alive gate's fault-spinner check matches.
 * @since 0.1.0
 */
[[noreturn]] RA8_INTERNAL static void internal_dcr_panic_halt(void)
{
  /* NASA Rule 2 exemption: terminal panic spin -- intentionally never exits. */
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Entry: launch the embedded copy-to-run image; never returns on success.
 * @pre Reset_Handler copied .data and zeroed .bss; SystemInit ran.
 * @pre The embedded image's first two words are a valid MSP + Thumb reset vector.
 * @post On success, control is at the image's reset vector in the SRAM run window.
 * @post On a failed run-target check, control parks in ::internal_dcr_panic_halt.
 * @note Single entry point; not re-entrant.
 * @since 0.1.0
 */
void main(void)
{
  static_assert(sizeof(s_payload_image) >= (2U * sizeof(uint32_t)),
                "payload image must contain at least the 2-word vector table");

  /* HIL contract: the memprobe gate reads g_dcr_run_heartbeat by name and asserts
   * it advances. That holds only if the linker pinned the probe words to the
   * addresses the payload writes (payload.c k_payload_probe_*). Trip the fault
   * path loudly (and freeze the heartbeat) if a future edit breaks either pin. */
  if ((uintptr_t)&g_dcr_run_sentinel != (uintptr_t)k_dcr_probe_sentinel_addr) {
    internal_dcr_panic_halt();
  }
  if ((uintptr_t)&g_dcr_run_heartbeat != (uintptr_t)k_dcr_probe_counter_addr) {
    internal_dcr_panic_halt();
  }

  uint32_t initial_sp  = 0U;
  uint32_t reset_entry = 0U;
  memcpy(&initial_sp, &s_payload_image[0], sizeof(initial_sp));
  memcpy(&reset_entry, &s_payload_image[sizeof(initial_sp)], sizeof(reset_entry));
  /* Preconditions (NASA Rule 5): the embedded image must carry a plausible SRAM
   * stack pointer and a Thumb reset vector before we hand off to it. */
  if ((initial_sp & (uint32_t)k_dcr_sram_mask) != (uint32_t)k_dcr_sram_base) {
    internal_dcr_panic_halt();
  }
  if ((reset_entry & (uint32_t)k_dcr_thumb_bit) == 0U) {
    internal_dcr_panic_halt();
  }

  /* Copy-to-run: ra8_dfu_launch copies the image to k_ra8_dfu_run_base and branches
   * there. It returns only if the run-target check fails (it should not). */
  ra8_dfu_launch((uintptr_t)s_payload_image,
                 (uint32_t)sizeof(s_payload_image),
                 (uint32_t)k_ra8_dfu_run_base);

  internal_dcr_panic_halt();
}
