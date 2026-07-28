/**
 * @file examples/ek_ra8d2/hw_validated/hil/dfu_bootloader/main.c
 * @brief Real USB-DFU MRAM bootloader: boot the active app slot, or accept a DFU update.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The immutable first 128 KiB of code-MRAM. At every reset it:
 *
 *  1. Reads the no-init SRAM DFU trigger word (one-shot, cleared after read).
 *  2. Validates both application slots (magic + length + software CRC32 over the
 *     image body) and reads their monotonic sequence numbers.
 *  3. Runs the pure ::ra8_dfu_boot_decide policy: trigger set OR neither slot
 *     valid -> stay in DFU; otherwise jump to the valid slot with the higher
 *     sequence number (Slot A on a tie).
 *
 * On a JUMP it copies the slot's image body to a fixed SRAM run base
 * (::k_ra8_dfu_run_base), points VTOR there, loads the image's initial MSP, and
 * branches to its reset vector -- a same-world (Secure) hand-off. Copy-to-run
 * means an app is linked ONCE at the run base and the same image boots from
 * either slot (no per-slot build). On DFU it brings up the
 * USBX DFU device (libs/ra8_dfu) on USB-FS (J11), programs the INACTIVE slot as
 * the host DFU_DNLOADs it, and soft-resets once the image header is committed so
 * the next boot decision selects the freshly written slot.
 *
 * Brick-safety: the bootloader region is never erased, a freshly written bad
 * slot fails its CRC (so the older valid slot still boots), and SWD re-flash of
 * the bootloader is always available. The DFU path writes only the inactive
 * slot -- no BTFLG, no option-setting, nothing irreversible.
 *
 * @note The console is hand-rolled (raw SCI8, not the BSP
 *       ``ra8_board_uart_console`` API) on purpose: this immutable bootloader
 *       keeps its dependency surface minimal and self-contained for
 *       brick-safety, so it does not pull in the board-support library.
 *
 * @note An app is copied to SRAM and run there, so it is linked at the single
 *       ::k_ra8_dfu_run_base (linker ORIGIN), not at a slot base; the identical
 *       image boots from either slot. See this app's README for the run base
 *       and the staging procedure.
 *
 * ## Pinout (USB-FS device + console only)
 *
 * FS device: P4_07 VBUS sense, P5_00 VBUSEN GPIO LOW (device role; the USB host
 * supplies VBUS), P8_14 D+, P8_15 D- (PSEL usb_fs). Console: PD_02/PD_03 SCI8
 * (PSEL sci_async, 115200 to the J-Link OB CDC).
 *
 * @author Brighton Sikarskie
 * @date 2026-06-16
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_dfu.h"
#include "ra8_dfu_device.h"
#include "ra8_err.h"
#include "ra8_gpio_constants.h"
#include "ra8_isr.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_sci.h"
#include "ra8_time.h"
#include "ra8_usb.h"
#ifdef RA8_ENABLE_ROOT_OF_TRUST
#include "mbedtls/memory_buffer_alloc.h"
#include "ra8_psa_crypto.h"
#endif

#ifndef RA8_SIMULATOR_MODE
#include "tx_api.h"
#include "ux_api.h"
#include "ux_dcd_ra8_usb.h"
#include "ux_device_class_dfu.h"
#include "ux_device_stack.h"

extern void _tx_timer_interrupt(void); /**< @brief ThreadX 1 ms tick worker. */

/**
 * @var s_tx_kernel_up
 * @brief Gates ThreadX tick delivery until ::tx_application_define has run.
 * @since 0.1.0
 */
static volatile bool s_tx_kernel_up = false;

void SysTick_Handler(void);
void SysTick_Handler(void)
{
  ra8_time_on_tick();
  if (s_tx_kernel_up) {
    _tx_timer_interrupt();
    ux_dcd_ra8_usb_irq_reenable();
  }
}
#endif

/* -------------------------------------------------------------------------- */
/* Pinout (FSP-aligned, EK-RA8D2 v1 User's Manual) */
/* -------------------------------------------------------------------------- */

/** @brief USBFS VBUS sense pin (P4_07, PSEL = 0x13). */
static const ra8_port_pin_t k_blc_pin_fs_vbus = (ra8_port_pin_t)k_ra8_board_usbfs_pin_vbus;

/** @brief USBFS VBUSEN (P5_00) -- GPIO LOW for the device role. */
static const ra8_port_pin_t k_blc_pin_fs_vbusen = (ra8_port_pin_t)k_ra8_board_usbfs_pin_vbusen;

/** @brief USBFS D+ (P8_14). */
static const ra8_port_pin_t k_blc_pin_fs_dp = (ra8_port_pin_t)k_ra8_board_usbfs_pin_dp;

/** @brief USBFS D- (P8_15). */
static const ra8_port_pin_t k_blc_pin_fs_dm = (ra8_port_pin_t)k_ra8_board_usbfs_pin_dm;

/** @brief J-Link OB CDC TX pin (PD_02 -- SCI8 TX). */
static const ra8_port_pin_t k_blc_pin_sci_tx = (ra8_port_pin_t)k_ra8_board_uart_console_pin_txd;

/** @brief J-Link OB CDC RX pin (PD_03 -- SCI8 RX). */
static const ra8_port_pin_t k_blc_pin_sci_rx = (ra8_port_pin_t)k_ra8_board_uart_console_pin_rxd;

/* -------------------------------------------------------------------------- */
/* Tunables */
/* -------------------------------------------------------------------------- */

/**
 * @enum blc_config_t
 * @brief Compile-time settings: thread, pool, console, cadence.
 */
typedef enum : uint32_t {
  k_blc_thread_stack    = 4096U,   /**< Device worker stack (bytes).     */
  k_blc_usbx_pool_bytes = 32768U,  /**< USBX memory pool (bytes).        */
  k_blc_idle_ticks      = 50U,     /**< Worker back-off (1 ms ticks).    */
  k_blc_baud            = 115200U, /**< J-Link OB CDC log baud.          */
  k_blc_sci_channel     = 8U,      /**< SCI8 -> J-Link OB CDC bridge.    */
  k_blc_print_cap       = 160U,    /**< Bound for console-string scans.  */
  k_blc_dev_priority    = 8U,      /**< Device bring-up worker priority. */
} blc_config_t;

/**
 * @enum blc_hex_t
 * @brief Hex text-formatter sizing constants.
 */
typedef enum : uint8_t {
  k_blc_hex_chars_u32   = 8U,  /**< 32-bit value -> "ABCDEF01".    */
  k_blc_nibble_bits     = 4U,  /**< Bits per hex nibble.           */
  k_blc_hex_digit_split = 10U, /**< Threshold between '0-9'/'A-F'. */
} blc_hex_t;

/** @brief 4-bit nibble mask for the hex formatter. */
typedef enum : uint32_t {
  k_blc_nibble_mask = 0xFU, /**< 4-bit nibble mask. */
} blc_mask_t;

/**
 * @enum blc_scb_t
 * @brief Cortex-M85 System Control Block address used by the DFU-commit reset.
 *
 * @details Accessed by absolute address (the project does not vendor a CMSIS
 * SCB struct). AIRCR is the Secure alias since the bootloader runs Secure. The
 * copy-to-run hand-off's VTOR write lives in ::ra8_dfu_launch.
 */
typedef enum : uintptr_t {
  k_blc_scb_aircr_addr = 0xE000ED0CU, /**< SCB->AIRCR (reset control). */
} blc_scb_t;

/** @brief AIRCR write to request a system reset (VECTKEY 0x05FA | SYSRESETREQ). */
typedef enum : uint32_t {
  k_blc_aircr_sysreset = 0x05FA0004U, /**< 0x05FA<<16 | (1<<2). */
} blc_reset_t;

/**
 * @var g_dfu_trigger
 * @brief No-init SRAM word an application writes (== ::k_ra8_dfu_trigger_magic)
 *        before resetting to request the bootloader's DFU mode.
 * @details Lives in `.noinit` (survives a warm reset, never bss-cleared). The
 * bootloader reads it once and clears it, so the request is one-shot.
 * @warning Garbage on a cold boot; an exact magic match is required, so a stray
 *          value almost never (1 in 2^32) trips a false DFU entry.
 * @since 0.1.0
 */
[[gnu::section(".noinit")]] static volatile uint32_t g_dfu_trigger;

#ifndef RA8_SIMULATOR_MODE

/* -------------------------------------------------------------------------- */
/* ThreadX worker + USBX pool storage */
/* -------------------------------------------------------------------------- */

/** @brief ThreadX TCB for the USBX device-side worker thread. */
static TX_THREAD s_device_thread;
/** @brief Stack backing storage for ::s_device_thread. */
static UCHAR s_device_stack[k_blc_thread_stack];
/** @brief USBX memory pool (USBX uses ``tx_byte_pool`` internally). */
static UCHAR s_usbx_pool[k_blc_usbx_pool_bytes];

/**
 * @var s_device_framework
 * @brief USBX device + DFU descriptors: a DFU-mode interface (bInterfaceProtocol
 *        0x02, enumerates straight into dfuIDLE) + the DFU functional descriptor
 *        (CAN_DNLOAD | CAN_UPLOAD, wTransferSize 64). DFU runs over EP0.
 * @since 0.1.0
 */
static UCHAR s_device_framework[] = {
  /* Device descriptor (18 bytes). idVendor 0x1209, idProduct 0x0019. */
  0x12U,
  0x01U,
  0x00U,
  0x02U,
  0x00U,
  0x00U,
  0x00U,
  0x40U,
  0x09U,
  0x12U,
  0x19U,
  0x00U,
  0x00U,
  0x01U,
  0x01U,
  0x02U,
  0x03U,
  0x01U,
  /* Configuration descriptor (wTotalLength 0x1B = 27). */
  0x09U,
  0x02U,
  0x1BU,
  0x00U,
  0x01U,
  0x01U,
  0x00U,
  0x80U,
  0x32U,
  /* DFU interface (class 0xFE, subclass 0x01, protocol 0x02 = DFU mode). */
  0x09U,
  0x04U,
  0x00U,
  0x00U,
  0x00U,
  0xFEU,
  0x01U,
  0x02U,
  0x00U,
  /* DFU functional descriptor. bmAttributes 0x07, wTransferSize 64. */
  0x09U,
  0x21U,
  0x07U,
  0xFFU,
  0x00U,
  0x40U,
  0x00U,
  0x10U,
  0x01U,
};

/**
 * @var s_string_framework
 * @brief USBX string descriptor table (vendor / product / serial).
 * @since 0.1.0
 */
static UCHAR s_string_framework[] = {
  /* idx 1: "Brighton Sikarskie". */
  0x09U,
  0x04U,
  0x01U,
  0x12U,
  'B',
  'r',
  'i',
  'g',
  'h',
  't',
  'o',
  'n',
  ' ',
  'S',
  'i',
  'k',
  'a',
  'r',
  's',
  'k',
  'i',
  'e',
  /* idx 2: "RA8D2 DFU". */
  0x09U,
  0x04U,
  0x02U,
  0x09U,
  'R',
  'A',
  '8',
  'D',
  '2',
  ' ',
  'D',
  'F',
  'U',
  /* idx 3: serial "00000019". */
  0x09U,
  0x04U,
  0x03U,
  0x08U,
  '0',
  '0',
  '0',
  '0',
  '0',
  '0',
  '1',
  '9',
};

/** @brief USBX LANGID descriptor 0x0409 (English-US), little-endian byte pair. */
typedef enum : uint8_t {
  k_usb_langid_en_us_lo = 0x09U, /**< LANGID 0x0409 low byte.  */
  k_usb_langid_en_us_hi = 0x04U, /**< LANGID 0x0409 high byte. */
} usb_langid_byte_t;

/** @brief USBX language-id table -- US English. */
static UCHAR s_language_id_framework[] = {k_usb_langid_en_us_lo, k_usb_langid_en_us_hi};

#endif /* !RA8_SIMULATOR_MODE */

/* -------------------------------------------------------------------------- */
/* J-Link probes */
/* -------------------------------------------------------------------------- */

/** @brief J-Link debug-probe "not yet written" sentinel. */
typedef enum : uint32_t {
  k_blc_dbg_unset = 0xFFFFFFFFU, /**< Probe word value before main writes it. */
} blc_dbg_t;

/** @brief Boot decision outcome (::ra8_dfu_action_t), J-Link-readable. */
static volatile uint32_t s_dbg_action = k_blc_dbg_unset;
/** @brief Slot the DFU path targets (::ra8_dfu_slot_t), J-Link-readable. */
static volatile uint32_t s_dbg_target = k_blc_dbg_unset;
/** @brief Device worker progress: 4 = DFU class attached. */
static volatile uint32_t s_dbg_dev_step;
/** @brief Device bring-up error (0 = none). */
static volatile uint32_t s_dbg_dev_err;

/**
 * @var g_blc_dfu_tick
 * @brief HIL liveness counter -- advanced once per DFU device service step.
 *
 * @details
 * Flashed standalone (the HIL scenario), neither application slot is valid, so
 * ::blc_decide deterministically returns ::k_ra8_dfu_action_dfu and control ends
 * up in ::blc_device_worker's service loop. This counter is incremented on every
 * pass of that loop, so a J-Link double-halt mem32 read (scripts/
 * hil_jlink_memprobe.sh, HIL_MODE=jlink_memprobe) sees it advance and proves the
 * bootloader booted, decided "no bootable slot", and brought the USBX DFU device
 * up -- not merely that the chip is alive with PC in MRAM. A copy-to-run jump to
 * a valid slot would leave this counter frozen (PC moves to the SRAM run window);
 * the dfu_copy_to_run app gates that path instead.
 *
 * Global (non-static) and `volatile` so the symbol stays linker-visible for nm
 * and the increment is not optimized away.
 *
 * @note Read externally by J-Link only; firmware never reads it back.
 * @since 0.1.0
 */
volatile uint32_t g_blc_dfu_tick = 0U;

/* -------------------------------------------------------------------------- */
/* Console helpers (SCI8 -> J-Link OB CDC) */
/* -------------------------------------------------------------------------- */

/**
 * @brief Format one nibble (0..15) into an uppercase hex character.
 * @param[in] nibble 4-bit value.
 * @return ASCII '0'..'9' or 'A'..'F'.
 * @retval '0' For a zero nibble.
 * @pre Caller has masked the value to 4 bits.
 * @pre None beyond the mask contract.
 * @post Returned byte is printable hex.
 * @post No state changes.
 * @note Pure function.
 * @since 0.1.0
 */
static uint8_t blc_nibble_to_hex(uint32_t nibble)
{
  if (nibble < k_blc_hex_digit_split) {
    return (uint8_t)((uint8_t)'0' + (uint8_t)nibble);
  }
  return (uint8_t)((uint8_t)'A' + (uint8_t)nibble - (uint8_t)k_blc_hex_digit_split);
}

/**
 * @brief Bounded ASCII string length (cap ::k_blc_print_cap).
 * @param[in] text NUL-terminated string.
 * @return Number of bytes before the NUL, capped.
 * @retval 0 For an empty string.
 * @pre @p text is non-NULL with readable storage.
 * @pre @p text fits the cap.
 * @post No state changes.
 * @post Return value never exceeds ::k_blc_print_cap.
 * @note Bounded scan.
 * @since 0.1.0
 */
static uint32_t blc_str_len(const char* text)
{
  uint32_t len = 0U;
  while (len < (uint32_t)k_blc_print_cap) {
    if (text[len] == '\0') {
      break;
    }
    len++;
  }
  return len;
}

/**
 * @brief Print a NUL-terminated ASCII string over SCI8 (polled).
 * @param[in] text String to print (CR/LF included by the caller).
 * @return ra8_err_t propagated from `ra8_sci_write_polling`.
 * @retval k_ra8_ok All bytes queued.
 * @pre SCI8 init already ran; @p text is non-NULL.
 * @pre @p text is NUL-terminated within ::k_blc_print_cap bytes.
 * @post The string bytes are in the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t blc_print(const char* text)
{
  return ra8_sci_write_polling((uint8_t)k_blc_sci_channel, (const uint8_t*)text, blc_str_len(text));
}

/**
 * @brief Print a value as fixed-width (8-digit) uppercase hex.
 * @param[in] value Value to print.
 * @return ra8_err_t propagated from the SCI helper.
 * @retval k_ra8_ok All bytes queued.
 * @pre SCI8 init already ran.
 * @pre None beyond console readiness.
 * @post One 8-character hex token is in the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t blc_print_hex(uint32_t value)
{
  uint8_t out[k_blc_hex_chars_u32] = {};
  for (uint8_t i = 0U; i < (uint8_t)k_blc_hex_chars_u32; i++) {
    const uint8_t shift = (uint8_t)(((uint8_t)k_blc_hex_chars_u32 - 1U - i) * k_blc_nibble_bits);
    out[i]              = blc_nibble_to_hex((value >> shift) & k_blc_nibble_mask);
  }
  return ra8_sci_write_polling((uint8_t)k_blc_sci_channel, out, (uint32_t)k_blc_hex_chars_u32);
}

/* -------------------------------------------------------------------------- */
/* Boot decision + slot hand-off */
/* -------------------------------------------------------------------------- */

/**
 * @brief Read the trigger + both slots and resolve the reset-time action.
 *
 * @details Reads and clears the one-shot ::g_dfu_trigger, validates both slots
 * (CRC over the body) and their sequence numbers, then folds them through the
 * pure ::ra8_dfu_boot_decide policy. The DFU target (the slot to program) is the
 * INACTIVE slot, or Slot A when neither slot is valid.
 *
 * @param[out] out_target Slot the DFU path should program. Non-NULL.
 *
 * @return The resolved ::ra8_dfu_action_t.
 * @retval k_ra8_dfu_action_jump_a Boot Slot A.
 * @retval k_ra8_dfu_action_jump_b Boot Slot B.
 * @retval k_ra8_dfu_action_dfu    Stay in the DFU device.
 *
 * @pre @p out_target is non-NULL.
 * @pre Both slots are readable code-MRAM (always true post-reset).
 * @post ::g_dfu_trigger has been cleared to 0.
 * @post `*out_target` holds the inactive slot (or Slot A).
 *
 * @note Pure reads only -- no ra8_flash_open, no programming.
 * @since 0.1.0
 */
static ra8_dfu_action_t blc_decide(ra8_dfu_slot_t* out_target)
{
  if (out_target == nullptr) {
    return k_ra8_dfu_action_dfu; /* safe fallback: never jump on a bad caller */
  }
  const bool trig = (g_dfu_trigger == (uint32_t)k_ra8_dfu_trigger_magic);
  g_dfu_trigger   = 0U; /* one-shot: consume the request */

  const bool a_valid = ra8_dfu_slot_valid(k_ra8_dfu_slot_a);
  const bool b_valid = ra8_dfu_slot_valid(k_ra8_dfu_slot_b);
  uint32_t   a_seq   = 0U;
  uint32_t   b_seq   = 0U;
  /* A failed read leaves seq at 0 (treated as oldest) -- a safe default that
   * never wins the higher-seq tiebreak, so the discarded return is intentional. */
  (void)ra8_dfu_slot_seq(k_ra8_dfu_slot_a, &a_seq);
  (void)ra8_dfu_slot_seq(k_ra8_dfu_slot_b, &b_seq);

  const ra8_dfu_slot_t active = ra8_dfu_select_slot(a_valid, a_seq, b_valid, b_seq);
  *out_target = (active == k_ra8_dfu_slot_none) ? k_ra8_dfu_slot_a : ra8_dfu_other_slot(active);

  return ra8_dfu_boot_decide(trig, a_valid, a_seq, b_valid, b_seq);
}

/**
 * @brief Copy a validated slot's image to the SRAM run base and launch it.
 *
 * @details
 * Reads the slot header for its `img_len` / `entry`, then delegates to the
 * shared ::ra8_dfu_launch, which cross-checks the run target, copies the image
 * body from the slot to ::k_ra8_dfu_run_base in SRAM, and branches to it. Because
 * an app is linked ONCE at the run base, the same image boots from either slot.
 * ::ra8_dfu_launch returns here only when the run target fails validation (a
 * corrupted `entry`/length), in which case `main` drops to DFU.
 *
 * @param[in] slot Slot to boot (A or B); already passed ::ra8_dfu_slot_valid.
 *
 * @return void -- returns to the caller ONLY when the run target is invalid
 *         (so `main` drops to DFU); on a valid image it does not return.
 *
 * @pre @p slot is A or B and passed CRC validation in ::blc_decide.
 * @pre The image was linked at ::k_ra8_dfu_run_base (its header `entry` records it).
 * @post On a valid image, control is in the application at the run base.
 * @post On an invalid run target, no copy/jump happens and control returns.
 *
 * @note Not thread-safe; the final hand-off of a single-threaded boot.
 * @since 0.1.0
 */
static void blc_boot_slot(ra8_dfu_slot_t slot)
{
  ra8_dfu_img_hdr_t hdr = {};
  if (ra8_dfu_read_header(slot, &hdr) != k_ra8_ok) {
    return; /* unreadable header -> let main fall through to DFU */
  }
  ra8_dfu_launch(ra8_dfu_slot_base(slot), hdr.img_len, hdr.entry);
  /* Returns only if ra8_dfu_run_target_valid rejected the image -> main -> DFU. */
}

#ifndef RA8_SIMULATOR_MODE

/**
 * @brief Request a system reset via AIRCR.SYSRESETREQ. Never returns.
 * @return Does not return.
 * @pre Called after a committed DFU image, to re-run the boot decision.
 * @pre Caller accepts that volatile state is lost.
 * @post The core resets and re-enters Reset_Handler.
 * @note Spins in WFI until the reset lands.
 * @since 0.1.0
 */
[[noreturn]] static void blc_system_reset(void)
{
  __asm__ volatile("dsb 0xF" ::: "memory");
  /* HUM Ch 6.1 Table 6.1 p 244 "Software reset (AIRCR.SYSRESETREQ)"; the
   * Cortex-M85 AIRCR itself is defined in ARMv8-M ARM B3.2.6 at 0xE000ED0C. */
  *(volatile uint32_t*)(uintptr_t)k_blc_scb_aircr_addr = (uint32_t)k_blc_aircr_sysreset;
  __asm__ volatile("dsb 0xF" ::: "memory");
  /* NASA Rule 2 exemption: spin until the reset lands (a few cycles); the
   * function is [[noreturn]] and never iterates unboundedly in practice. */
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief DFU device worker: bring the DFU class up, drain commits, reset on done.
 * @param[in] arg ThreadX entry argument (unused).
 * @return Never returns.
 * @pre ::ra8_dfu_device_set_target selected the inactive slot (main did it).
 * @pre USB-FS pins + clock are up (main did both).
 * @post The FS device is attached in DFU mode; commits trigger a system reset.
 * @post On any bring-up failure the thread parks (::s_dbg_dev_err set).
 * @note The DFU class runs its own thread; this worker only services it.
 * @since 0.1.0
 */
static VOID blc_device_worker(ULONG arg)
{
  (void)arg;

  const ra8_err_t e = ra8_dfu_device_start(k_ra8_usb_speed_fs,
                                           s_usbx_pool,
                                           (uint32_t)sizeof(s_usbx_pool),
                                           s_device_framework,
                                           (uint32_t)sizeof(s_device_framework),
                                           s_string_framework,
                                           (uint32_t)sizeof(s_string_framework),
                                           s_language_id_framework,
                                           (uint32_t)sizeof(s_language_id_framework));
  if (e != k_ra8_ok) {
    s_dbg_dev_err = (uint32_t)e;
    (void)blc_print("dfu-bootloader: FAIL device bring-up\r\n");
    return;
  }
  s_dbg_dev_step = 4U;
  (void)blc_print("dfu-bootloader: DFU device up on USB-FS, awaiting DFU_DNLOAD...\r\n");

  /* NASA Rule 2 exemption: deliberate RTOS service loop. It exits only via
   * blc_system_reset() once an image commits (that call does not return). */
  while (1) {
    (void)ra8_dfu_device_worker_step();
    g_blc_dfu_tick += 1U; /* HIL liveness: proves the DFU service loop iterates */
    if (ra8_dfu_device_committed() && (ra8_dfu_device_last_error() == k_ra8_ok)) {
      (void)blc_print("dfu-bootloader: image committed -- resetting into new slot\r\n");
      blc_system_reset();
    }
    tx_thread_sleep((ULONG)k_blc_idle_ticks);
  }
}

/**
 * @brief ThreadX application-define hook. Spawns the single DFU device worker.
 * @param[in] first_unused_memory Sentinel (unused; static stacks).
 * @return void.
 * @pre Called from ``tx_kernel_enter`` after scheduler init.
 * @pre The DFU target slot was selected before ``tx_kernel_enter``.
 * @post One auto-start worker is queued; ``s_tx_kernel_up`` is true.
 * @note Called once at boot; not thread-safe.
 * @since 0.1.0
 */
VOID tx_application_define(VOID* first_unused_memory)
{
  (void)first_unused_memory;
  s_tx_kernel_up = true;
  (void)tx_thread_create(&s_device_thread,
                         "dfu_device",
                         blc_device_worker,
                         0UL,
                         s_device_stack,
                         (ULONG)k_blc_thread_stack,
                         (UINT)k_blc_dev_priority,
                         (UINT)k_blc_dev_priority,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
}
#endif /* !RA8_SIMULATOR_MODE */

/* -------------------------------------------------------------------------- */
/* Startup */
/* -------------------------------------------------------------------------- */

/**
 * @brief Halt forever in WFI -- panic stop on init failure.
 * @return void.
 * @pre Called only after a fatal boot error.
 * @pre Interrupts may be in any state.
 * @post CPU is parked.
 * @post No further code runs.
 * @note Not reachable post-boot.
 * @since 0.1.0
 */
[[noreturn]] static void blc_panic_halt(void)
{
  /* NASA Rule 2 exemption: terminal panic spin -- intentionally never exits. */
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Route the USB-FS device pins (P4_07/P5_00/P8_14/P8_15).
 * @return void.
 * @pre IOPORT is reachable.
 * @pre Called once from ::blc_setup_or_halt.
 * @post FS carries the device role (VBUSEN LOW; the host supplies VBUS).
 * @post Panic-halts on any routing failure.
 * @note Panic-halts on any routing failure.
 * @since 0.1.0
 */
static void blc_route_usb_or_halt(void)
{
  if (ra8_pfs_route_peripheral(k_blc_pin_fs_vbus, k_ra8_psel_usb_fs, "blc.fs_vbus") != k_ra8_ok) {
    blc_panic_halt();
  }
  if (ra8_gpio_output_init(k_blc_pin_fs_vbusen, k_ra8_level_low) != k_ra8_ok) {
    blc_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_blc_pin_fs_dp, k_ra8_psel_usb_fs, "blc.fs_dp") != k_ra8_ok) {
    blc_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_blc_pin_fs_dm, k_ra8_psel_usb_fs, "blc.fs_dm") != k_ra8_ok) {
    blc_panic_halt();
  }
}

#ifdef RA8_ENABLE_ROOT_OF_TRUST
/**
 * @enum blc_rot_const_t
 * @brief Root-of-trust setup constant.
 */
typedef enum : uint32_t {
  k_blc_psa_heap_bytes = 0x10000U, /**< 64 KiB tf-psa-crypto buffer-alloc arena. */
} blc_rot_const_t;

/**
 * @var s_blc_psa_heap
 * @brief Static arena backing MBEDTLS_MEMORY_BUFFER_ALLOC for the RoT verify.
 * @details tf-psa-crypto's ECDSA-P256 verify draws its working buffers from this
 *          fixed arena instead of a real heap (NASA Rule 3: no dynamic memory
 *          after init). Sized to match secure_boot_hil's proven verify path.
 * @warning Not thread-safe; consumed only on the single-threaded boot path.
 * @since 0.1.0
 */
static uint8_t s_blc_psa_heap[k_blc_psa_heap_bytes];
#endif

/**
 * @brief Bring CGC + USB-FS clock + SysTick + SCI8 + LEDs + pins up.
 * @return void.
 * @pre Reset_Handler finished C runtime init.
 * @pre SystemInit has run.
 * @post Console works; the USB-FS pins and clock are live.
 * @post Panic-halts on any failure.
 * @note Called once from main.
 * @since 0.1.0
 */
static void blc_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    blc_panic_halt();
  }
  if (ra8_cgc_usbfs_clock_enable() != k_ra8_ok) {
    blc_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    blc_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, &pclka_hz) != k_ra8_ok) {
    blc_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    blc_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_blc_pin_sci_tx, k_ra8_psel_sci_async, "blc.txd8") != k_ra8_ok) {
    blc_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_blc_pin_sci_rx, k_ra8_psel_sci_async, "blc.rxd8") != k_ra8_ok) {
    blc_panic_halt();
  }
  const ra8_sci_cfg_t sci_cfg = {
    .baud      = k_blc_baud,
    .data_bits = k_ra8_sci_data_8,
    .parity    = k_ra8_sci_parity_none,
    .stop_bits = k_ra8_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra8_sci_init((uint8_t)k_blc_sci_channel, &sci_cfg) != k_ra8_ok) {
    blc_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    blc_panic_halt();
  }
#ifdef RA8_ENABLE_ROOT_OF_TRUST
  /* Bring up the tf-psa-crypto backend for the root-of-trust launch gate: a
   * static buffer-alloc arena (no malloc) then psa_crypto_init. A failure here
   * means the bootloader cannot authenticate any slot, so halt rather than boot
   * an unverified image. */
  mbedtls_memory_buffer_alloc_init(s_blc_psa_heap, sizeof(s_blc_psa_heap));
  if (ra8_psa_crypto_init() != k_ra8_ok) {
    blc_panic_halt();
  }
#endif
  blc_route_usb_or_halt();
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry: decide, then boot a slot or enter the DFU device.
 * @return Never returns (jumps to an app or stays in ``tx_kernel_enter``).
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR, FPU, priority grouping.
 * @post Control passes to the active app, or to the ThreadX-hosted DFU device.
 * @post On any HAL init failure the function halts in WFI.
 * @note Single entry point; not re-entrant.
 * @since 0.1.0
 */
int32_t main(void)
{
  blc_setup_or_halt();
  ra8_isr_globals_enable();

  (void)blc_print("\r\nra8d2 dfu-bootloader (immutable, 128K @ 0x02000000)\r\n");

  ra8_dfu_slot_t         target = k_ra8_dfu_slot_a;
  const ra8_dfu_action_t action = blc_decide(&target);
  s_dbg_action                  = (uint32_t)action;
  s_dbg_target                  = (uint32_t)target;

  if (action == k_ra8_dfu_action_jump_a) {
    (void)blc_print("dfu-bootloader: Slot A -> copy-to-run @0x");
    (void)blc_print_hex((uint32_t)k_ra8_dfu_run_base);
    (void)blc_print("\r\n");
    blc_boot_slot(k_ra8_dfu_slot_a); /* returns only if the run target is invalid */
  }
  if (action == k_ra8_dfu_action_jump_b) {
    (void)blc_print("dfu-bootloader: Slot B -> copy-to-run @0x");
    (void)blc_print_hex((uint32_t)k_ra8_dfu_run_base);
    (void)blc_print("\r\n");
    blc_boot_slot(k_ra8_dfu_slot_b); /* returns only if the run target is invalid */
  }

  /* k_ra8_dfu_action_dfu (or a slot whose run target failed validation): no
   * bootable slot -- accept a DFU update into the inactive slot over USB-FS. */
  (void)blc_print("dfu-bootloader: no bootable slot -- entering DFU\r\n");
#ifndef RA8_SIMULATOR_MODE
  ra8_dfu_device_set_target(target);
  tx_kernel_enter();
#endif

  blc_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
