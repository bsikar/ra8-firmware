/**
 * @file examples/ek_ra8d2/hw_validated/hil/dfu_copy_to_run/payload.c
 * @brief Minimal copy-to-run payload -- one image that runs from the SRAM run base.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * A tiny, self-contained application linked ONCE at the dfu_bootloader's SRAM
 * run base (::k_ra8_dfu_run_base = 0x22020000, see payload.ld). A copy-to-run
 * launcher (::ra8_dfu_launch) copies this image to that base and branches here,
 * so the IDENTICAL `.bin` is bootable from Slot A or Slot B -- the proof that
 * copy-to-run removes the per-slot build (issue #97). It backs both the
 * `dfu_copy_to_run` HIL demo (which embeds and launches it) and the bench
 * staging of `../dfu_bootloader` (which stages it into a real slot).
 *
 * On entry it writes a distinctive sentinel and then advances a heartbeat counter
 * forever, both at a fixed probe word that sits between the bootloader's low
 * .bss and the run base (so neither the bootloader nor this image's copy touches
 * it). A J-Link probe of those two words -- plus a PC inside the SRAM run
 * window -- confirms the image ran via copy-to-run. Interrupts stay masked (the
 * launcher masked them before the hand-off and this payload never re-enables
 * them), so the 2-entry vector table is sufficient.
 *
 * Build with `build_payload.sh` (also emits `payload_image.h` for the demo).
 *
 * @author Brighton Sikarskie
 * @date 2026-06-16
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

/**
 * @enum payload_probe_t
 * @brief Fixed SRAM probe words the bootloader leaves untouched.
 * @details `0x22010000` lies above the bootloader's low-SRAM footprint and below
 *          the run base, so it is owned by neither the bootloader nor this image.
 */
typedef enum : uintptr_t {
  k_payload_probe_sentinel = 0x22010000U, /**< Sentinel word (proof-of-run). */
  k_payload_probe_counter  = 0x22010004U, /**< Heartbeat counter (liveness). */
} payload_probe_t;

/** @brief Sentinel value ("#97 1 code"): this payload ran via copy-to-run. */
typedef enum : uint32_t {
  k_payload_sentinel_value = 0x9710C0DEU, /**< Distinctive proof-of-run value. */
} payload_sentinel_t;

/** @brief Initial-MSP symbol, defined by payload.ld at the top of the run region. */
extern uint32_t g_ra8_ls_stack_top;

/**
 * @brief Payload reset entry: set the sentinel, then advance a heartbeat forever.
 * @return Does not return.
 * @pre A copy-to-run launcher copied this image to ::k_ra8_dfu_run_base and set MSP.
 * @pre Interrupts are masked (the launcher masked them before the hand-off).
 * @post The sentinel word holds ::k_payload_sentinel_value.
 * @post The heartbeat counter advances continuously.
 * @note Bare entry: no C runtime, no .data/.bss (writes go to fixed addresses).
 * @note Non-static so the linker `ENTRY(payload_reset)` resolves (the launcher
 *       actually branches via the vector table, not the ELF entry).
 * @since 0.1.0
 */
[[noreturn]] void payload_reset(void);

void payload_reset(void)
{
  *(volatile uint32_t*)(uintptr_t)k_payload_probe_sentinel = (uint32_t)k_payload_sentinel_value;
  volatile uint32_t* const counter = (volatile uint32_t*)(uintptr_t)k_payload_probe_counter;
  *counter                         = 0U;
  /* Heartbeat: advance forever so a J-Link probe sees liveness. Intentional
   * terminal loop (this is a leaf demo payload, not reusable firmware). */
  for (;;) {
    *counter = *counter + 1U;
  }
}

/**
 * @var s_vectors
 * @brief 2-entry vector table: initial MSP + reset, at the image base.
 * @details The launcher reads [0] as the initial MSP and branches to [1]; the
 *          reset entry keeps its Thumb bit because it is a function address.
 * @since 0.1.0
 */
[[gnu::section(".vectors"), gnu::used]] static const uintptr_t s_vectors[2] = {
  (uintptr_t)&g_ra8_ls_stack_top, /* initial MSP                              */
  (uintptr_t)&payload_reset,      /* reset vector (Thumb bit from the symbol) */
};
