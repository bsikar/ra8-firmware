/**
 * @file examples/ek_ra8d2/hw_pending/dfu_bootloader/main.c
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
 *  3. Runs the pure ::ra_dfu_boot_decide policy: trigger set OR neither slot
 *     valid -> stay in DFU; otherwise jump to the valid slot with the higher
 *     sequence number (Slot A on a tie).
 *
 * On a JUMP it points VTOR at the slot base, loads the slot's initial MSP, and
 * branches to its reset vector -- a same-world (Secure) hand-off, since both the
 * bootloader and the application live in Secure MRAM. On DFU it brings up the
 * USBX DFU device (libs/ra_dfu) on USB-FS (J11), programs the INACTIVE slot as
 * the host DFU_DNLOADs it, and soft-resets once the image header is committed so
 * the next boot decision selects the freshly written slot.
 *
 * Brick-safety: the bootloader region is never erased, a freshly written bad
 * slot fails its CRC (so the older valid slot still boots), and SWD re-flash of
 * the bootloader is always available. The DFU path writes only the inactive
 * slot -- no BTFLG, no option-setting, nothing irreversible.
 *
 * @note An app runs in-place from its slot, so it is linked at the slot base
 *       (linker MRAM ORIGIN); see this app's README for the two literal slot
 *       bases and the staging procedure.
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

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_dfu.h"
#include "ra_dfu_device.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"
#include "ra_usb.h"

#ifndef RA_SIMULATOR_MODE
#include "tx_api.h"
#include "ux_api.h"
#include "ux_dcd_ra_usb.h"
#include "ux_device_class_dfu.h"
#include "ux_device_stack.h"

extern void ra_time_on_tick(void);
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
  ra_time_on_tick();
  if (s_tx_kernel_up) {
    _tx_timer_interrupt();
    ux_dcd_ra_usb_irq_reenable();
  }
}
#endif

/* -------------------------------------------------------------------------- */
/* Pinout (FSP-aligned, EK-RA8D2 v1 User's Manual)                            */
/* -------------------------------------------------------------------------- */

/** @brief USBFS VBUS sense pin (P4_07, PSEL = 0x13). */
static const ra_port_pin_t k_blc_pin_fs_vbus =
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_7);

/** @brief USBFS VBUSEN (P5_00) -- GPIO LOW for the device role. */
static const ra_port_pin_t k_blc_pin_fs_vbusen =
  (ra_port_pin_t)(((uint16_t)k_ra_port_5 << 8) | (uint16_t)k_ra_pin_0);

/** @brief USBFS D+ (P8_14). */
static const ra_port_pin_t k_blc_pin_fs_dp =
  (ra_port_pin_t)(((uint16_t)k_ra_port_8 << 8) | (uint16_t)k_ra_pin_14);

/** @brief USBFS D- (P8_15). */
static const ra_port_pin_t k_blc_pin_fs_dm =
  (ra_port_pin_t)(((uint16_t)k_ra_port_8 << 8) | (uint16_t)k_ra_pin_15);

/** @brief J-Link OB CDC TX pin (PD_02 -- SCI8 TX). */
static const ra_port_pin_t k_blc_pin_sci_tx =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);

/** @brief J-Link OB CDC RX pin (PD_03 -- SCI8 RX). */
static const ra_port_pin_t k_blc_pin_sci_rx =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/* -------------------------------------------------------------------------- */
/* Tunables                                                                   */
/* -------------------------------------------------------------------------- */

/**
 * @enum blc_config_t
 * @brief Compile-time settings: thread, pool, console, cadence.
 */
typedef enum : uint32_t {
  k_blc_thread_stack    = 4096U,   /**< Device worker stack (bytes).      */
  k_blc_usbx_pool_bytes = 32768U,  /**< USBX memory pool (bytes).         */
  k_blc_idle_ticks      = 50U,     /**< Worker back-off (1 ms ticks).     */
  k_blc_baud            = 115200U, /**< J-Link OB CDC log baud.           */
  k_blc_sci_channel     = 8U,      /**< SCI8 -> J-Link OB CDC bridge.     */
  k_blc_print_cap       = 160U,    /**< Bound for console-string scans.   */
  k_blc_dev_priority    = 8U,      /**< Device bring-up worker priority.  */
} blc_config_t;

/**
 * @enum blc_hex_t
 * @brief Hex text-formatter sizing constants.
 */
typedef enum : uint8_t {
  k_blc_hex_chars_u32   = 8U,  /**< 32-bit value -> "ABCDEF01".     */
  k_blc_nibble_bits     = 4U,  /**< Bits per hex nibble.            */
  k_blc_hex_digit_split = 10U, /**< Threshold between '0-9'/'A-F'.  */
} blc_hex_t;

/** @brief 4-bit nibble mask for the hex formatter. */
typedef enum : uint32_t {
  k_blc_nibble_mask = 0xFU, /**< 4-bit nibble mask. */
} blc_mask_t;

/**
 * @enum blc_scb_t
 * @brief Cortex-M85 System Control Block addresses + keys used by the hand-off.
 *
 * @details Accessed by absolute address (the project does not vendor a CMSIS
 * SCB struct). VTOR is the Secure alias since the bootloader runs Secure.
 */
typedef enum : uintptr_t {
  k_blc_scb_vtor_addr  = 0xE000ED08U, /**< SCB->VTOR (Secure vector-table base). */
  k_blc_scb_aircr_addr = 0xE000ED0CU, /**< SCB->AIRCR (reset control).           */
} blc_scb_t;

/** @brief AIRCR write to request a system reset (VECTKEY 0x05FA | SYSRESETREQ). */
typedef enum : uint32_t {
  k_blc_aircr_sysreset = 0x05FA0004U, /**< 0x05FA<<16 | (1<<2). */
} blc_reset_t;

/**
 * @var g_dfu_trigger
 * @brief No-init SRAM word an application writes (== ::k_ra_dfu_trigger_magic)
 *        before resetting to request the bootloader's DFU mode.
 * @details Lives in `.noinit` (survives a warm reset, never bss-cleared). The
 * bootloader reads it once and clears it, so the request is one-shot.
 * @warning Garbage on a cold boot; an exact magic match is required, so a stray
 *          value almost never (1 in 2^32) trips a false DFU entry.
 * @since 0.1.0
 */
static volatile uint32_t g_dfu_trigger __attribute__((section(".noinit")));

#ifndef RA_SIMULATOR_MODE

/* -------------------------------------------------------------------------- */
/* ThreadX worker + USBX pool storage                                         */
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

#endif /* !RA_SIMULATOR_MODE */

/* -------------------------------------------------------------------------- */
/* J-Link probes                                                              */
/* -------------------------------------------------------------------------- */

/** @brief Boot decision outcome (::ra_dfu_action_t), J-Link-readable. */
static volatile uint32_t s_dbg_action = 0xFFFFFFFFU;
/** @brief Slot the DFU path targets (::ra_dfu_slot_t), J-Link-readable. */
static volatile uint32_t s_dbg_target = 0xFFFFFFFFU;
/** @brief Device worker progress: 4 = DFU class attached. */
static volatile uint32_t s_dbg_dev_step;
/** @brief Device bring-up error (0 = none). */
static volatile uint32_t s_dbg_dev_err;

/* -------------------------------------------------------------------------- */
/* Console helpers (SCI8 -> J-Link OB CDC)                                     */
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
 * @return ra_err_t propagated from `ra_sci_write_polling`.
 * @retval k_ra_ok All bytes queued.
 * @pre SCI8 init already ran; @p text is non-NULL.
 * @pre @p text is NUL-terminated within ::k_blc_print_cap bytes.
 * @post The string bytes are in the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t blc_print(const char* text)
{
  return ra_sci_write_polling((uint8_t)k_blc_sci_channel, (const uint8_t*)text, blc_str_len(text));
}

/**
 * @brief Print a value as fixed-width (8-digit) uppercase hex.
 * @param[in] value Value to print.
 * @return ra_err_t propagated from the SCI helper.
 * @retval k_ra_ok All bytes queued.
 * @pre SCI8 init already ran.
 * @pre None beyond console readiness.
 * @post One 8-character hex token is in the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t blc_print_hex(uint32_t value)
{
  uint8_t out[k_blc_hex_chars_u32] = {};
  for (uint8_t i = 0U; i < (uint8_t)k_blc_hex_chars_u32; i++) {
    const uint8_t shift = (uint8_t)(((uint8_t)k_blc_hex_chars_u32 - 1U - i) * k_blc_nibble_bits);
    out[i]              = blc_nibble_to_hex((value >> shift) & k_blc_nibble_mask);
  }
  return ra_sci_write_polling((uint8_t)k_blc_sci_channel, out, (uint32_t)k_blc_hex_chars_u32);
}

/* -------------------------------------------------------------------------- */
/* Boot decision + slot hand-off                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief Read the trigger + both slots and resolve the reset-time action.
 *
 * @details Reads and clears the one-shot ::g_dfu_trigger, validates both slots
 * (CRC over the body) and their sequence numbers, then folds them through the
 * pure ::ra_dfu_boot_decide policy. The DFU target (the slot to program) is the
 * INACTIVE slot, or Slot A when neither slot is valid.
 *
 * @param[out] out_target Slot the DFU path should program. Non-NULL.
 *
 * @return The resolved ::ra_dfu_action_t.
 * @retval k_ra_dfu_action_jump_a Boot Slot A.
 * @retval k_ra_dfu_action_jump_b Boot Slot B.
 * @retval k_ra_dfu_action_dfu    Stay in the DFU device.
 *
 * @pre @p out_target is non-NULL.
 * @pre Both slots are readable code-MRAM (always true post-reset).
 * @post ::g_dfu_trigger has been cleared to 0.
 * @post `*out_target` holds the inactive slot (or Slot A).
 *
 * @note Pure reads only -- no ra_flash_open, no programming.
 * @since 0.1.0
 */
static ra_dfu_action_t blc_decide(ra_dfu_slot_t* out_target)
{
  const bool trig = (g_dfu_trigger == (uint32_t)k_ra_dfu_trigger_magic);
  g_dfu_trigger   = 0U; /* one-shot: consume the request */

  const bool a_valid = ra_dfu_slot_valid(k_ra_dfu_slot_a);
  const bool b_valid = ra_dfu_slot_valid(k_ra_dfu_slot_b);
  uint32_t   a_seq   = 0U;
  uint32_t   b_seq   = 0U;
  (void)ra_dfu_slot_seq(k_ra_dfu_slot_a, &a_seq);
  (void)ra_dfu_slot_seq(k_ra_dfu_slot_b, &b_seq);

  const ra_dfu_slot_t active = ra_dfu_select_slot(a_valid, a_seq, b_valid, b_seq);
  *out_target = (active == k_ra_dfu_slot_none) ? k_ra_dfu_slot_a : ra_dfu_other_slot(active);

  return ra_dfu_boot_decide(trig, a_valid, a_seq, b_valid, b_seq);
}

/**
 * @brief Hand off to an application slot (same-world Secure jump). Never returns.
 *
 * @details Loads the slot's initial MSP and reset vector from its vector table
 * at @p base, points the Secure VTOR at @p base, and branches. Both bootloader
 * and application are Secure, so this is an ordinary branch (the reset vector's
 * Thumb bit is kept) -- not a BLXNS. Interrupts are masked across the switch;
 * the application re-enables them after its own bring-up. The MSP/VTOR change
 * and the branch run as one inline-asm block so the compiler never touches the
 * abandoned stack between switching SP and branching.
 *
 * @param[in] base Slot base address (== the application's vector table).
 *
 * @return Does not return (control passes to the application).
 *
 * @pre @p base is a 64 KiB-aligned slot base whose image passed CRC validation.
 * @pre The slot's vector[1] (reset) carries the Thumb bit, per the C ABI.
 * @post Control is in the application; the bootloader frame is gone.
 *
 * @note Not thread-safe; the final hand-off of a single-threaded boot.
 * @since 0.1.0
 */
[[noreturn]] static void blc_jump(uintptr_t base)
{
  const uint32_t initial_sp  = ((const volatile uint32_t*)base)[0];
  const uint32_t reset_entry = ((const volatile uint32_t*)base)[1];

  __asm__ volatile("cpsid i" ::: "memory");
  *(volatile uint32_t*)(uintptr_t)k_blc_scb_vtor_addr = (uint32_t)base;
  __asm__ volatile("dsb 0xF\n isb 0xF\n" ::: "memory");
  __asm__ volatile("msr msp, %0\n"
                   "bx %1\n"
                   :
                   : "r"(initial_sp), "r"(reset_entry)
                   : "memory");
  __builtin_unreachable();
}

#ifndef RA_SIMULATOR_MODE

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
  *(volatile uint32_t*)(uintptr_t)k_blc_scb_aircr_addr = (uint32_t)k_blc_aircr_sysreset;
  __asm__ volatile("dsb 0xF" ::: "memory");
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief DFU device worker: bring the DFU class up, drain commits, reset on done.
 * @param[in] arg ThreadX entry argument (unused).
 * @return Never returns.
 * @pre ::ra_dfu_device_set_target selected the inactive slot (main did it).
 * @pre USB-FS pins + clock are up (main did both).
 * @post The FS device is attached in DFU mode; commits trigger a system reset.
 * @post On any bring-up failure the thread parks (::s_dbg_dev_err set).
 * @note The DFU class runs its own thread; this worker only services it.
 * @since 0.1.0
 */
static VOID blc_device_worker(ULONG arg)
{
  (void)arg;

  const ra_err_t e = ra_dfu_device_start(k_ra_usb_speed_fs,
                                         s_usbx_pool,
                                         (uint32_t)sizeof(s_usbx_pool),
                                         s_device_framework,
                                         (uint32_t)sizeof(s_device_framework),
                                         s_string_framework,
                                         (uint32_t)sizeof(s_string_framework),
                                         s_language_id_framework,
                                         (uint32_t)sizeof(s_language_id_framework));
  if (e != k_ra_ok) {
    s_dbg_dev_err = (uint32_t)e;
    (void)blc_print("dfu-bootloader: FAIL device bring-up\r\n");
    return;
  }
  s_dbg_dev_step = 4U;
  (void)blc_print("dfu-bootloader: DFU device up on USB-FS, awaiting DFU_DNLOAD...\r\n");

  while (1) {
    (void)ra_dfu_device_worker_step();
    if (ra_dfu_device_committed() && (ra_dfu_device_last_error() == k_ra_ok)) {
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
#endif /* !RA_SIMULATOR_MODE */

/* -------------------------------------------------------------------------- */
/* Startup                                                                    */
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
  if (ra_pfs_route_peripheral(k_blc_pin_fs_vbus, k_ra_psel_usb_fs, "blc.fs_vbus") != k_ra_ok) {
    blc_panic_halt();
  }
  if (ra_gpio_output_init(k_blc_pin_fs_vbusen, k_ra_level_low) != k_ra_ok) {
    blc_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_blc_pin_fs_dp, k_ra_psel_usb_fs, "blc.fs_dp") != k_ra_ok) {
    blc_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_blc_pin_fs_dm, k_ra_psel_usb_fs, "blc.fs_dm") != k_ra_ok) {
    blc_panic_halt();
  }
}

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
  if (ra_cgc_init() != k_ra_ok) {
    blc_panic_halt();
  }
  if (ra_cgc_usbfs_clock_enable() != k_ra_ok) {
    blc_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    blc_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    blc_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    blc_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_blc_pin_sci_tx, k_ra_psel_sci_async, "blc.txd8") != k_ra_ok) {
    blc_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_blc_pin_sci_rx, k_ra_psel_sci_async, "blc.rxd8") != k_ra_ok) {
    blc_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_blc_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_blc_sci_channel, &sci_cfg) != k_ra_ok) {
    blc_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    blc_panic_halt();
  }
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
  ra_isr_globals_enable();

  (void)blc_print("\r\nra8d2 dfu-bootloader (immutable, 128K @ 0x02000000)\r\n");

  ra_dfu_slot_t         target = k_ra_dfu_slot_a;
  const ra_dfu_action_t action = blc_decide(&target);
  s_dbg_action                 = (uint32_t)action;
  s_dbg_target                 = (uint32_t)target;

  if (action == k_ra_dfu_action_jump_a) {
    (void)blc_print("dfu-bootloader: jumping to Slot A @0x");
    (void)blc_print_hex((uint32_t)k_ra_dfu_slot_a_base);
    (void)blc_print("\r\n");
    blc_jump((uintptr_t)k_ra_dfu_slot_a_base);
  }
  if (action == k_ra_dfu_action_jump_b) {
    (void)blc_print("dfu-bootloader: jumping to Slot B @0x");
    (void)blc_print_hex((uint32_t)k_ra_dfu_slot_b_base);
    (void)blc_print("\r\n");
    blc_jump((uintptr_t)k_ra_dfu_slot_b_base);
  }

  /* k_ra_dfu_action_dfu: no valid slot (or a trigger) -- accept a DFU update
   * into the inactive slot over USB-FS. */
  (void)blc_print("dfu-bootloader: no bootable slot -- entering DFU\r\n");
#ifndef RA_SIMULATOR_MODE
  ra_dfu_device_set_target(target);
  tx_kernel_enter();
#endif

  blc_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
