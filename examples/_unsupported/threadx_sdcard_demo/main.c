/**
 * @file examples/_unsupported/threadx_sdcard_demo/main.c
 * @brief Eclipse ThreadX SD-card boot-sector dump on the EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * One ThreadX thread that exercises the full ``ra_sdcard`` HAL stack:
 *
 *   1. Route the eight SDHI pins (CMD, CLK, DAT0..3, WP, CD) on port 4
 *      to PSEL=k_ra_psel_sdhi (0x12 -- SDHI / MMC).
 *   2. Bring the J-Link OB VCOM console up at 115200 8N1 via
 *      ``ra_board_uart_console_init``.
 *   3. ``ra_sdcard_init`` runs the standard SD Physical Layer
 *      initialization sequence (CMD0 -> CMD7) on SDHI instance 0.
 *   4. ``ra_sdcard_read_blocks(0, ...)`` reads sector 0 (the MBR / boot
 *      sector) into a 512-byte SRAM buffer.
 *   5. The first 16 bytes are formatted as ASCII hex
 *      (``XX XX XX XX...``) and pushed out the console; LED1 toggles
 *      once per pass to make the activity visible.
 *
 * SDHI pin map mirrors ``examples/ereader``: port 4, pins 0..7 routed
 * to the on-chip SDHI block (see ereader main.c for the same map).
 *
 * @par Threads
 *
 * | Name        | Priority | Period         | Action                       |
 * |:------------|:---------|:---------------|:-----------------------------|
 * | ``sdcard``  | 4        | 5000 ms loop   | Read block 0, dump 16 bytes  |
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sdcard.h"

/*
 * The host unit-test build (RA_SIMULATOR_MODE) does not link the
 * ThreadX vendor tree, so ``tx_api.h`` is unreachable when clang-tidy
 * walks this file. Pull it in only on the cross-compile target.
 */
#ifndef RA_SIMULATOR_MODE
#include "tx_api.h"
#endif

/* ---------------------------------------------------------------------------
 * Tunables (typed enums per the project's no-magic-number rule).
 * --------------------------------------------------------------------------- */

/**
 * @brief Stack size, in bytes, for the SD card thread.
 */
typedef enum : uint16_t {
  k_sdcard_thread_stack_bytes = 4096U,
} sdcard_stack_t;

/**
 * @brief Thread priority for the SD card thread.
 */
typedef enum : uint8_t {
  k_sdcard_thread_priority = 4U,
} sdcard_priority_t;

/**
 * @brief Console baud (115200 8N1).
 */
typedef enum : uint32_t {
  k_sdcard_console_baud = 115200U,
} sdcard_baud_t;

/**
 * @brief Loop period in ThreadX ticks (port/threadx/tx_user.h pins to 1 ms).
 */
typedef enum : uint16_t {
  k_sdcard_loop_ticks = 5000U,
} sdcard_period_t;

/**
 * @brief SDHI pin layout constants.
 */
typedef enum : uint8_t {
  k_sdcard_sdhi_pin_count = 8U,  /**< CMD + CLK + DAT0..3 + WP + CD. */
  k_sdcard_sdhi_instance  = 0U,  /**< SDHI instance index.           */
  k_sdcard_dump_bytes     = 16U, /**< Bytes from sector 0 to print.  */
} sdcard_layout_t;

/**
 * @brief ASCII-hex formatting constants.
 */
typedef enum : uint8_t {
  k_sdcard_hex_chars_per_byte  = 3U,    /**< "XX " incl. trailing space. */
  k_sdcard_hex_nibble_shift    = 4U,    /**< High-nibble right-shift.    */
  k_sdcard_hex_nibble_mask     = 0x0FU, /**< Low-nibble mask.            */
  k_sdcard_hex_alpha_threshold = 10U,   /**< 10..15 -> 'A'..'F'.         */
} sdcard_hex_t;

/**
 * @brief SD logical block size (512 bytes for SDHC/SDXC).
 */
typedef enum : uint16_t {
  k_sdcard_block_bytes = 512U,
} sdcard_block_size_t;

/**
 * @brief Pack (port, pin) into a ra_port_pin_t at file scope.
 */
typedef enum : uint8_t {
  k_sdcard_port_shift = 8U,
} sdcard_port_pack_t;

/* ---------------------------------------------------------------------------
 * SDHI pin map (port 4, pins 0..7) -- identical to examples/ereader.
 * --------------------------------------------------------------------------- */

#define SDCARD_PP(port_, pin_)                                                                     \
  ((ra_port_pin_t)(((uint16_t)(port_) << k_sdcard_port_shift) | (uint16_t)(pin_)))

/**
 * @brief SDHI pin list. Each pin is set to ``k_ra_psel_sdhi``.
 *
 * @details
 * RA8D2 datasheet R01DS0493EJ "Pin Functions" -> "SDHI" lists the
 * default mapping at port 4. Same caveat as examples/ereader -- the
 * board-side wiring should be confirmed against EK-RA8D2 v1 docs once
 * they are committed to ``docs/reference/``.
 */
static const ra_port_pin_t k_sdcard_sdhi_pins[k_sdcard_sdhi_pin_count] = {
  SDCARD_PP(k_ra_port_4, k_ra_pin_0), /* SD_CMD  */
  SDCARD_PP(k_ra_port_4, k_ra_pin_1), /* SD_CLK  */
  SDCARD_PP(k_ra_port_4, k_ra_pin_2), /* SD_DAT0 */
  SDCARD_PP(k_ra_port_4, k_ra_pin_3), /* SD_DAT1 */
  SDCARD_PP(k_ra_port_4, k_ra_pin_4), /* SD_DAT2 */
  SDCARD_PP(k_ra_port_4, k_ra_pin_5), /* SD_DAT3 */
  SDCARD_PP(k_ra_port_4, k_ra_pin_6), /* SD_WP   */
  SDCARD_PP(k_ra_port_4, k_ra_pin_7), /* SD_CD   */
};

/* ---------------------------------------------------------------------------
 * Vector table override and thread storage (cross-build only).
 * --------------------------------------------------------------------------- */

#ifndef RA_SIMULATOR_MODE
/* NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,readability-identifier-naming) -- ThreadX-supplied symbol. */
extern void _tx_timer_interrupt(void);
/* NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,readability-identifier-naming) */

/**
 * @brief Stack for the SD card thread (32-bit aligned per ARMv8-M AAPCS).
 */
static uint8_t s_sdcard_stack[k_sdcard_thread_stack_bytes] __attribute__((aligned(8)));

/**
 * @brief Thread control block. ThreadX zeroes it on tx_thread_create.
 */
static TX_THREAD s_sdcard_thread;

/**
 * @brief Block buffer for one SD sector. File-scope so it does not eat
 *        thread-stack budget.
 */
static uint8_t s_sdcard_block[k_sdcard_block_bytes];

/**
 * @brief SysTick exception handler -- tail-calls the ThreadX timer.
 *
 * @pre Called from exception context (IPSR == 15).
 * @pre ``_tx_initialize_low_level`` has programmed SysTick.
 * @post One ThreadX tick elapsed; PendSV may be pending.
 */
void SysTick_Handler(void);
void SysTick_Handler(void)
{
  _tx_timer_interrupt();
}

/* ---------------------------------------------------------------------------
 * Helpers.
 * --------------------------------------------------------------------------- */

/**
 * @brief Convert a single nibble to its ASCII hex character.
 *
 * @param[in] nibble Low 4 bits of an arbitrary uint8_t.
 *
 * @return ASCII char in '0'..'9' or 'A'..'F'.
 *
 * @pre 0 <= nibble <= 15.
 * @post Returned byte is a printable ASCII hex digit.
 */
static char sdcard_nibble_to_hex(uint8_t nibble)
{
  const uint8_t n = (uint8_t)(nibble & (uint8_t)k_sdcard_hex_nibble_mask);
  if (n < (uint8_t)k_sdcard_hex_alpha_threshold) {
    return (char)('0' + (char)n);
  }
  return (char)('A' + (char)(n - (uint8_t)k_sdcard_hex_alpha_threshold));
}

/**
 * @brief Format ``len`` bytes from ``in`` into "XX XX ..." ASCII.
 *
 * @param[in]  in     Source buffer.
 * @param[in]  len    Number of source bytes to format.
 * @param[out] out    Destination buffer; must hold ``len * 3`` bytes.
 *
 * @pre  in / out non-NULL; out has space for len * 3 bytes.
 * @post out holds an ASCII hex dump (no NUL terminator written).
 */
static void sdcard_hex_dump(const uint8_t* in, uint8_t len, char* out)
{
  for (uint8_t i = 0U; i < len; i++) {
    const uint8_t b = in[i];
    out[i * k_sdcard_hex_chars_per_byte + 0U] =
      sdcard_nibble_to_hex((uint8_t)(b >> k_sdcard_hex_nibble_shift));
    out[i * k_sdcard_hex_chars_per_byte + 1U] =
      sdcard_nibble_to_hex((uint8_t)(b & (uint8_t)k_sdcard_hex_nibble_mask));
    out[i * k_sdcard_hex_chars_per_byte + 2U] = ' ';
  }
}

/**
 * @brief Push a NUL-terminated ASCII string out the J-Link OB VCOM.
 *
 * @param[in] s NUL-terminated string. NULL is a no-op.
 *
 * @pre ra_board_uart_console_init has succeeded.
 * @post Bytes have been handed to SCI3 TDR (or silently dropped on err).
 */
static void sdcard_log(const char* s)
{
  if (s == nullptr) {
    return;
  }
  size_t len = 0U;
  while (s[len] != '\0') {
    len++;
  }
  (void)ra_board_uart_console_write((const uint8_t*)s, len);
}

/**
 * @brief Route the eight SDHI pins to PSEL=k_ra_psel_sdhi.
 *
 * @return Error code from the first failing route call, or k_ra_ok.
 *
 * @pre IOPORT module reachable; single-threaded init context.
 * @post On success the eight SDHI pins are owned by the SDHI block.
 */
[[nodiscard]] static ra_err_t sdcard_pins_init(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_sdcard_sdhi_pin_count; i++) {
    const ra_err_t err =
      ra_pfs_route_peripheral(k_sdcard_sdhi_pins[i], k_ra_psel_sdhi, "threadx_sdcard.sdhi");
    if (err != k_ra_ok) {
      return err;
    }
  }
  return k_ra_ok;
}

/* ---------------------------------------------------------------------------
 * Thread body.
 * --------------------------------------------------------------------------- */

/**
 * @brief Read sector 0 into ``s_sdcard_block`` and log status / hex dump.
 *
 * @pre ra_sdcard_init has succeeded.
 * @post Either the dump line has been emitted or an error line has.
 */
static void sdcard_one_pass(void)
{
  const ra_err_t err = ra_sdcard_read_blocks(0U, s_sdcard_block, 1U);
  if (err != k_ra_ok) {
    sdcard_log("sdcard: read block 0 failed\r\n");
    return;
  }
  char dump[k_sdcard_dump_bytes * k_sdcard_hex_chars_per_byte + 1U] = {};
  sdcard_hex_dump(s_sdcard_block, (uint8_t)k_sdcard_dump_bytes, dump);
  dump[k_sdcard_dump_bytes * k_sdcard_hex_chars_per_byte] = '\0';
  sdcard_log("sdcard: blk0[0..15] = ");
  sdcard_log(dump);
  sdcard_log("\r\n");
}

/**
 * @brief Thread entry: init the card once, then loop reading sector 0.
 *
 * @param[in] thread_input Unused (ThreadX cookie).
 *
 * @pre Pins routed; console initialized; ThreadX scheduler running.
 * @post Loops forever: read + dump + LED1 toggle + sleep.
 */
static void sdcard_thread_entry(ULONG thread_input)
{
  (void)thread_input;

  const ra_sdcard_cfg_t cfg = {.instance = k_sdcard_sdhi_instance};
  if (ra_sdcard_init(&cfg) != k_ra_ok) {
    sdcard_log("sdcard: init failed (no card / bad pinmux?)\r\n");
    while (1) {
      (void)tx_thread_sleep((ULONG)k_sdcard_loop_ticks);
    }
  }
  sdcard_log("sdcard: card initialized\r\n");

  while (1) {
    sdcard_one_pass();
    (void)ra_board_led_toggle(k_ra_board_led1);
    (void)tx_thread_sleep((ULONG)k_sdcard_loop_ticks);
  }
}

/* ---------------------------------------------------------------------------
 * tx_application_define -- ThreadX calls this once before the first
 * scheduling decision.
 * --------------------------------------------------------------------------- */

/**
 * @brief ThreadX application initialisation callback.
 *
 * @param[in] first_unused_memory Unused -- the demo uses a static stack.
 *
 * @pre Called from ``_tx_initialize_kernel_enter`` with IRQs masked.
 * @post The SD card thread is created and auto-started.
 */
/* NOLINTNEXTLINE(misc-use-internal-linkage) -- exported symbol expected by ThreadX. */
void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;

  const UINT err = tx_thread_create(&s_sdcard_thread,
                                    "sdcard",
                                    sdcard_thread_entry,
                                    0U,
                                    s_sdcard_stack,
                                    (ULONG)k_sdcard_thread_stack_bytes,
                                    (UINT)k_sdcard_thread_priority,
                                    (UINT)k_sdcard_thread_priority,
                                    TX_NO_TIME_SLICE,
                                    TX_AUTO_START);
  if (err != TX_SUCCESS) {
    while (1) {
      __asm__ volatile("wfi");
    }
  }
}
#endif /* !RA_SIMULATOR_MODE */

/* ---------------------------------------------------------------------------
 * main() -- driver init, then drop into the ThreadX scheduler.
 * --------------------------------------------------------------------------- */

/**
 * @brief Application entry. Brings up LED, console, SDHI pins, then ThreadX.
 *
 * @return Never returns -- ``tx_kernel_enter`` does not.
 *
 * @pre Reset_Handler has copied .data + zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 * @post On clean entry the SD card thread runs forever.
 * @post On any HAL init failure the function halts in ``__WFI``.
 *
 * @since 0.1.0
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    while (1) {
      __asm__ volatile("wfi");
    }
  }
  if (ra_board_uart_console_init((uint32_t)k_sdcard_console_baud) != k_ra_ok) {
    while (1) {
      __asm__ volatile("wfi");
    }
  }
#ifndef RA_SIMULATOR_MODE
  if (sdcard_pins_init() != k_ra_ok) {
    while (1) {
      __asm__ volatile("wfi");
    }
  }
  ra_isr_globals_enable();
  tx_kernel_enter();
#endif

  while (1) {
    __asm__ volatile("wfi");
  }
}
#pragma GCC diagnostic pop
