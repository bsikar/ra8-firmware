/**
 * @file cpu1_main.c
 * @brief Secondary Cortex-M33 hold-page supervisor for ra8d2-ereader
 * @ingroup grp_apps
 *
 * @par Tag
 * [App / Product] {World: NS}
 *
 * @details
 * Executes out of `MRAM_CPU1` (`0x020C0000`) with `.data`/`.bss` and stack in
 * `SRAM_CPU1` (`0x22190000..0x2219FFFF`). While the primary Cortex-M85 is
 * parked in HOCO-stopped Sleep mode, M33:
 *   1. Holds the displayed framebuffer metadata in shared SRAM3 (`0x22180000`).
 *   2. Blinks `LED1` (`P600` via NS alias `0x504000C0`) as the active-core cue.
 *   3. Polls `SW1` (`P009`) for page-turn wake, `SW2` (`P008`) for low-power
 *      disable wake, and optional `auto_turn_polls` for automated soak gates.
 *   4. Bumps `turn_req`, writes `wake_reason`, and pokes `IPC0` channel 0 IRQ0
 *      via ``ra8_ipc_send_event`` (`0x500200C0`) to wake CPU0 cleanly.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "er_lowpower.h"
#include "ra8_attributes.h"
#include "ra8_hw_intrinsics.h"
#include "ra8_ipc.h"
#include "ra8_sau.h"

extern uint32_t g_ra8_ls_cpu1_stack_top;
extern uint32_t g_ra8_ls_cpu1_data_start;
extern uint32_t g_ra8_ls_cpu1_data_end;
extern uint32_t g_ra8_ls_cpu1_data_load;
extern uint32_t g_ra8_ls_cpu1_bss_start;
extern uint32_t g_ra8_ls_cpu1_bss_end;

/** @brief Cortex-M33 CPU1 reset entry placed in `.cpu1_vectors[1]`. */
[[noreturn]] void cpu1_reset_handler(void);

/** @brief Non-Secure peripheral and SAU region bases used on CPU1. */
typedef enum : uintptr_t {
  k_c1_sau_mram_base   = 0x020C0000UL, /**< MRAM_CPU1 (256 KiB).             */
  k_c1_sau_sram3_base  = 0x22180000UL, /**< Shared mailbox + SRAM_CPU1.      */
  k_c1_sau_periph_base = 0x40000000UL, /**< Secure/NS peripheral span.       */
  k_c1_sau_sdram_base  = 0x68000000UL, /**< External 64 MiB SDRAM.           */
  k_c1_port0_pcntr2_ns = 0x50400004UL, /**< PORT0 PCNTR2 NS alias (SW1/SW2). */
  k_c1_port6_pcntr1_ns = 0x504000C0UL, /**< PORT6 PCNTR1 NS alias (LED1).    */
} c1_addr_t;

/** @brief Region sizes, bitmasks, and NASA Rule 2 dwell bounds on CPU1. */
typedef enum : uint32_t {
  k_c1_sau_mram_size   = 0x00040000UL, /**< 256 KiB CPU1 flash.             */
  k_c1_sau_sram3_size  = 0x00020000UL, /**< 128 KiB SRAM3 bank.             */
  k_c1_sau_periph_size = 0x20000000UL, /**< 512 MiB peripheral window.      */
  k_c1_sau_sdram_size  = 0x04000000UL, /**< 64 MiB SDRAM bank.              */
  k_c1_led1_pdr_out    = 0x00000001UL, /**< PCNTR1 PDR bit 0 = output.      */
  k_c1_led1_podr_hi    = 0x00010000UL, /**< PCNTR1 PODR bit 0 = LED1 ON.    */
  k_c1_sw1_bit_shift   = 9U,           /**< SW1 = P009 active-low PIDR[9].  */
  k_c1_sw2_bit_shift   = 8U,           /**< SW2 = P008 active-low PIDR[8].  */
  k_c1_poll_dwell_nops = 4096U,        /**< Bounded busy dwell per poll.    */
  k_c1_magic_wait_cap  = 100000U,      /**< Bounded wait for mailbox magic. */
  k_c1_ipc_ch_tx       = 0U,           /**< IPC0 channel 0 (M33 -> M85).    */
  k_c1_ipc_ch_rx       = 2U,           /**< IPC1 channel 0 (M85 -> M33).    */
} c1_const_t;

/**
 * @brief Program CPU1's 4-region SAU so flash, SRAM3, peripherals, and SDRAM pass.
 *
 * @pre Running in CPU1 startup context before accessing peripherals.
 * @post SAU regions 0..3 cover MRAM_CPU1, SRAM3, peripherals, and SDRAM.
 * @note Single-threaded CPU1 init only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_c1_init_sau(void)
{
  static const ra8_sau_region_t k_sau_regions[] = {
    [0] = {.base = (uintptr_t)k_c1_sau_mram_base,
           .size = (uint32_t)k_c1_sau_mram_size,
           .attr = k_ra8_sau_attr_ns},
    [1] = {.base = (uintptr_t)k_c1_sau_sram3_base,
           .size = (uint32_t)k_c1_sau_sram3_size,
           .attr = k_ra8_sau_attr_ns},
    [2] = {.base = (uintptr_t)k_c1_sau_periph_base,
           .size = (uint32_t)k_c1_sau_periph_size,
           .attr = k_ra8_sau_attr_ns},
    [3] = {.base = (uintptr_t)k_c1_sau_sdram_base,
           .size = (uint32_t)k_c1_sau_sdram_size,
           .attr = k_ra8_sau_attr_ns},
  };
  static const ra8_sau_cfg_t k_sau_cfg = {
    .regions      = k_sau_regions,
    .region_count = (uint8_t)(sizeof(k_sau_regions) / sizeof(k_sau_regions[0])),
    .all_ns       = false,
  };
  (void)ra8_sau_configure(&k_sau_cfg);
}

/**
 * @brief Initialise CPU1 `.data` and `.bss` segments in `SRAM_CPU1`.
 *
 * @pre Stack pointer already set from `.cpu1_vectors[0]`.
 * @post All CPU1 globals hold their C-startup values.
 * @note Uses `RA8_LOOP_BOUND_RUNTIME` against linker bounds.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_c1_init_ram(void)
{
  uint32_t*       dst = &g_ra8_ls_cpu1_data_start;
  const uint32_t* src = &g_ra8_ls_cpu1_data_load;
  RA8_LOOP_BOUND_RUNTIME(g_ra8_ls_cpu1_data_end);
  while (dst < &g_ra8_ls_cpu1_data_end) {
    *dst = *src;
    dst++;
    src++;
  }

  uint32_t* bss = &g_ra8_ls_cpu1_bss_start;
  RA8_LOOP_BOUND_RUNTIME(g_ra8_ls_cpu1_bss_end);
  while (bss < &g_ra8_ls_cpu1_bss_end) {
    *bss = 0U;
    bss++;
  }
}

/**
 * @brief Initialise IPC channels 0 (TX) and 2 (RX) from CPU1 NS world.
 *
 * @pre CPU0 has released CPU1 after setting `IPCSAR = 0x00050000`.
 * @post CPU1 HAL IPC channel handles are active at NS alias `0x50020000`.
 * @note Single-threaded CPU1 init only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_c1_init_ipc(void)
{
  const ra8_ipc_config_t tx_cfg = {
    .channel      = (uint8_t)k_c1_ipc_ch_tx,
    .reset_fifo   = false,
    .clear_status = true,
    .event_mask   = (uint32_t)k_ra8_ipc_event_none,
  };
  const ra8_ipc_config_t rx_cfg = {
    .channel      = (uint8_t)k_c1_ipc_ch_rx,
    .reset_fifo   = true,
    .clear_status = true,
    .event_mask   = (uint32_t)k_ra8_ipc_event_irq0,
  };
  (void)ra8_ipc_init(&tx_cfg);
  (void)ra8_ipc_init(&rx_cfg);
}

/**
 * @brief Drive LED1 (`P600`) output via NS PORT6 `PCNTR1`.
 *
 * @param[in] on `true` drives P600 high (LED1 illuminated), `false` low.
 *
 * @pre `PMSAR6` bit 0 configured Non-Secure by CPU0 before CPU1 release.
 * @post `PORT6.PCNTR1` PDR bit 0 is set and PODR bit 0 matches `on`.
 * @note Safe to invoke regardless of M85 sleep state.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_c1_drive_led1(bool on)
{
  uint32_t val = (uint32_t)k_c1_led1_pdr_out;
  if (on) {
    val |= (uint32_t)k_c1_led1_podr_hi;
  }
  /* HUM Ch 20.2.1 "PCNTR1" p 839 -- NS alias updates LED1 (P600) while holding. */
  *(volatile uint32_t*)k_c1_port6_pcntr1_ns = val;
}

/**
 * @brief Execute a calibrated bounded NOP dwell between CPU1 poll passes.
 *
 * @pre None.
 * @post Exactly ``k_c1_poll_dwell_nops`` NOP cycles elapsed.
 * @note NASA Rule 2 bounded loop.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_c1_dwell(void)
{
  RA8_LOOP_BOUND(k_c1_poll_dwell_nops);
  for (uint32_t i = 0U; i < (uint32_t)k_c1_poll_dwell_nops; ++i) {
    ra8_hw_nop();
  }
}

/**
 * @brief Raise an IPC0 wake event to M85 with updated reason and request count.
 *
 * @param[in,out] mbox   Shared SRAM3 mailbox.
 * @param[in]     reason ::er_lp_wake_reason_t cause of wakeup.
 *
 * @pre ``mbox != nullptr`` and `turn_req == turn_ack`.
 * @post `turn_req` incremented behind a DSB and `IPC0` IRQ0 asserted.
 * @note CPU1 single-threaded main loop only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_c1_wake_m85(volatile er_lp_mailbox_t* mbox,
                                              er_lp_wake_reason_t       reason)
{
  mbox->wake_reason = (uint32_t)reason;
  mbox->turn_req    = mbox->turn_req + 1U;
  ra8_hw_dsb();
  (void)ra8_ipc_send_event((uint8_t)k_c1_ipc_ch_tx, k_ra8_ipc_irq_event_0);
}

/**
 * @brief Service one pass of M33 hold-page supervision while M85 is parked.
 *
 * @param[in,out] mbox       Shared SRAM3 mailbox.
 * @param[in,out] sw1_prev   Previous sampled SW1 released state (`1` = up).
 * @param[in,out] sw2_prev   Previous sampled SW2 released state (`1` = up).
 * @param[in,out] hold_polls Polls accumulated during the current hold episode.
 * @param[in,out] led_on     Current LED1 blink toggle state.
 *
 * @pre `PMSAR0[9:8]` granted Non-Secure while `mbox->status == k_er_lp_status_holding`.
 * @post Heartbeat updated; wake fired if SW1, SW2, or `auto_turn_polls` triggered.
 * @note Only accesses PORT0 NS alias while M85 is parked in HOLDING status.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_c1_poll_holding(volatile er_lp_mailbox_t* mbox,
                                                  uint32_t*                 sw1_prev,
                                                  uint32_t*                 sw2_prev,
                                                  uint32_t*                 hold_polls,
                                                  bool*                     led_on)
{
  *led_on = !(*led_on);
  internal_c1_drive_led1(*led_on);
  mbox->m33_heartbeat = mbox->m33_heartbeat + 1U;
  *hold_polls += 1U;

  if (mbox->turn_req != mbox->turn_ack) {
    return;
  }

  /* HUM Ch 20.2.2 "PCNTR2" p 840 -- PIDR[9]=SW1 (P009), PIDR[8]=SW2 (P008). */
  const uint32_t pidr    = *(const volatile uint32_t*)k_c1_port0_pcntr2_ns;
  const uint32_t sw1_now = (pidr >> (uint32_t)k_c1_sw1_bit_shift) & 1U;
  const uint32_t sw2_now = (pidr >> (uint32_t)k_c1_sw2_bit_shift) & 1U;

  er_lp_wake_reason_t reason = k_er_lp_wake_none;
  if ((*sw1_prev != 0U) && (sw1_now == 0U)) {
    reason = k_er_lp_wake_next_page;
  } else if ((*sw2_prev != 0U) && (sw2_now == 0U)) {
    reason = k_er_lp_wake_toggle;
  } else if ((mbox->auto_turn_polls != 0U) && (*hold_polls >= mbox->auto_turn_polls)) {
    reason = k_er_lp_wake_auto_test;
  }

  *sw1_prev = sw1_now;
  *sw2_prev = sw2_now;

  if (reason != k_er_lp_wake_none) {
    *hold_polls = 0U;
    internal_c1_wake_m85(mbox, reason);
  }
}

/**
 * @brief Run the infinite CPU1 hold-page supervision loop.
 *
 * @pre Shared mailbox magic stamped and IPC channels active.
 * @post Never returns; toggles LED1 and wakes M85 on triggers.
 * @note Bare-metal CPU1 background loop.
 * @since 0.1.0
 */
[[noreturn]] RA8_INTERNAL static void internal_c1_run(void)
{
  volatile er_lp_mailbox_t* const mbox = (volatile er_lp_mailbox_t*)k_er_lp_mailbox_paddr;

  RA8_LOOP_BOUND(k_c1_magic_wait_cap);
  for (uint32_t w = 0U; w < (uint32_t)k_c1_magic_wait_cap; ++w) {
    if (mbox->magic == (uint32_t)k_er_lp_mailbox_magic) {
      break;
    }
    internal_c1_dwell();
  }

  mbox->m33_sig = (uint32_t)k_er_lp_m33_ready_sig;
  ra8_hw_dsb();

  uint32_t sw1_prev   = 1U;
  uint32_t sw2_prev   = 1U;
  uint32_t hold_polls = 0U;
  bool     led_on     = false;

  while (1) {
    mbox->m33_loops = mbox->m33_loops + 1U;
    if (mbox->status == (uint32_t)k_er_lp_status_holding) {
      internal_c1_poll_holding(mbox, &sw1_prev, &sw2_prev, &hold_polls, &led_on);
    } else {
      hold_polls = 0U;
      sw1_prev   = 1U;
      sw2_prev   = 1U;
      led_on     = false;
      internal_c1_drive_led1(false);
    }
    (void)ra8_ipc_clear_event((uint8_t)k_c1_ipc_ch_rx, k_ra8_ipc_irq_event_0);
    internal_c1_dwell();
  }
}

[[noreturn]] void cpu1_reset_handler(void)
{
  internal_c1_init_ram();
  internal_c1_init_sau();
  internal_c1_init_ipc();
  internal_c1_run();
}

/** @brief Minimal Cortex-M33 vector table placed at `ORIGIN(MRAM_CPU1)`. */
[[gnu::used, gnu::section(".cpu1_vectors")]] const uintptr_t g_cpu1_vector_table[] = {
  (uintptr_t)&g_ra8_ls_cpu1_stack_top,
  (uintptr_t)&cpu1_reset_handler,
};
