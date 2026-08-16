/**
 * @file examples/ek_ra8d2/hw_pending/i2c_peripheral_responder/main.c
 * @brief RIIC (ra8_i2c) target/peripheral responder driven by an external controller
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Standalone EVM-tier app that exercises the ra8_i2c (RIIC) *target*
 * (peripheral) role added for issue #189 -- the own-address match, the
 * attached-callback dispatch (``ra8_i2c_peripheral_attach_handler`` /
 * ``ra8_i2c_peripheral_dispatch``) and the clock-stretched target TX/RX path.
 *
 * The RA8D2 answers as an I2C target at 7-bit own address 0x42 on RIIC
 * channel 1 (P512 SCL1 / P511 SDA1 -- the board's Grove / Pmod / mikroBUS /
 * Arduino I2C bus, per issue #46). An *external* I2C controller (a second
 * board running ``i2c_loopback``, a Raspberry Pi ``i2cset`` / ``i2cget``, a
 * bench host, ...) drives the bus; a single RA8D2 core cannot both clock a
 * blocking controller transfer and service its own target at once, so the
 * controller lives off-board. The flow:
 *
 *   1. ``ra8_cgc_init`` + ``ra8_mstp_init`` + ``ra8_time_init`` -- clocks up.
 *   2. ``ra8_board_uart_console_init`` -- BSP SCI8 console (PD02 / PD03).
 *   3. ``ra8_board_io_expander_apply_project_sw4_defaults()`` -- the board's
 *      bench-validated RIIC1 bring-up (bus-recover, pull-ups, SCL1/SDA1
 *      route, ``ra8_i2c_init(ch1)``); reused verbatim from ``i2c_loopback``.
 *   4. ``ra8_i2c_peripheral_attach_handler`` + ``ra8_i2c_peripheral_init``
 *      (own_addr 0x42, clock-stretch on) -- arm the target and the callback.
 *   5. Tight loop over ``ra8_i2c_peripheral_dispatch``: when the external
 *      controller writes to 0x42, the callback drains it (``_receive``);
 *      when it reads 0x42, the callback echoes the last write (``_transmit``).
 *      Each serviced transfer prints on SCI8 and toggles LED1.
 *
 * Hardware: EK-RA8D2 v1 + an external I2C controller wired to RIIC1
 * (SDA1/SCL1) with bus pull-ups. hw_pending: the on-wire target role is
 * UNVERIFIED on silicon -- ra8_emulator models RIIC only as a controller.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_i2c.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_time.h"

/** @brief 32-bit app tunables (baud, bus rate). */
typedef enum : uint32_t {
  k_riic_target_baud   = 115200U, /**< SCI8 console baud. */
  k_riic_target_bus_hz = 100000U, /**< RIIC1 bus clock (unused here; the board
                                   bring-up owns the rate, kept for clarity). */
} riic_target_u32_t;

/** @brief 8-bit app tunables (channel, own address, buffer, reply). */
typedef enum : uint8_t {
  k_riic_target_channel   = 1U,    /**< RIIC ch1 (P512 SCL1 / P511 SDA1). */
  k_riic_target_own_addr  = 0x42U, /**< Our 7-bit own address.            */
  k_riic_target_buf_len   = 8U,    /**< Controller-write capture size.    */
  k_riic_target_seed_byte = 0xA5U, /**< Seed reply until a write arrives. */
  k_riic_target_seed_len  = 1U,    /**< Seed reply length.                */
} riic_target_u8_t;

/**
 * @struct riic_target_ctx_t
 * @brief Callback context: the channel and the last controller-write payload.
 *
 * @details
 * Passed to ``ra8_i2c_peripheral_attach_handler`` and handed back to
 * ``riic_target_on_event``. A controller write is captured into ``rx`` and
 * echoed back on the next controller read, giving a simple loopback semantic.
 * cppcheck cannot see the dispatch call graph, so pre-waive the member scan.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t  channel;                   /**< RIIC channel the target answers on.        */
  uint32_t rx_len;                    /**< Bytes captured from the last write (>= 1). */
  uint8_t  rx[k_riic_target_buf_len]; /**< Last controller-write payload (echoed).    */
} riic_target_ctx_t;
/* cppcheck-suppress-end [unusedStructMember] */

/** @brief SCI8 status strings emitted per target event. */
static const uint8_t k_riic_target_msg_armed[] = "riic-target: armed 0x42 poll-dispatch\r\n";
static const uint8_t k_riic_target_msg_rx[]    = "riic-target: write serviced\r\n";
static const uint8_t k_riic_target_msg_tx[]    = "riic-target: read serviced\r\n";

/**
 * @brief Park forever after a fatal init failure.
 *
 * @pre Called only after a fatal error in boot.
 * @post CPU is parked; only a debugger or external reset wakes it.
 * @since 0.1.0
 */
static void riic_target_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Address-match callback: service the controller-driven transfer.
 *
 * @details
 * Runs in polled ``ra8_i2c_peripheral_dispatch`` context (not an ISR), so the
 * blocking target transfers and console writes here are safe. A controller
 * write is drained into ``ctx->rx``; a controller read is answered by echoing
 * the last captured write (or the seed byte before any write arrives).
 *
 * @param[in]     vctx  Opaque ``riic_target_ctx_t*`` context.
 * @param[in]     event Decoded target event from the dispatcher.
 *
 * @pre ``vctx`` is a live ``riic_target_ctx_t`` for an armed channel.
 * @post On a write ``ctx->rx`` holds the payload; on a read it was transmitted.
 * @since 0.1.0
 */
static void riic_target_on_event(void* vctx, ra8_i2c_peripheral_event_t event)
{
  riic_target_ctx_t* ctx = (riic_target_ctx_t*)vctx;
  if (ctx == nullptr) {
    return;
  }
  if (event == k_ra8_i2c_peripheral_event_write) {
    uint32_t got = 0U;
    (void)ra8_i2c_peripheral_receive(ctx->channel, ctx->rx, (uint32_t)k_riic_target_buf_len, &got);
    if (got > 0U) {
      ctx->rx_len = got;
    }
    (void)ra8_board_uart_console_write(k_riic_target_msg_rx, sizeof(k_riic_target_msg_rx) - 1U);
    (void)ra8_board_led_toggle(k_ra8_board_led1);
    return;
  }
  if (event == k_ra8_i2c_peripheral_event_read) {
    uint32_t sent = 0U;
    (void)ra8_i2c_peripheral_transmit(ctx->channel, ctx->rx, ctx->rx_len, &sent);
    (void)ra8_board_uart_console_write(k_riic_target_msg_tx, sizeof(k_riic_target_msg_tx) - 1U);
    (void)ra8_board_led_toggle(k_ra8_board_led1);
  }
}

/**
 * @brief Bring CGC + SysTick + MSTP up. Panic-halts on any failure.
 *
 * @pre IRQs masked or single-threaded init context.
 * @post On success CGC is live and SysTick is ticking.
 * @since 0.1.0
 */
static void riic_target_clocks_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    riic_target_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    riic_target_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    riic_target_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    riic_target_panic_halt();
  }
}

/**
 * @brief Bring CGC + console + RIIC1 up. Panic-halts on any failure.
 *
 * @details
 * Reuses the board's bench-validated RIIC1 bring-up
 * (``ra8_board_io_expander_apply_project_sw4_defaults``) exactly as
 * ``i2c_loopback`` does, then brings up LED1.
 *
 * @pre Reset boot finished; single-threaded init context.
 * @post On success RIIC1 (ch1) is initialized and LED1 is ready.
 * @since 0.1.0
 */
static void riic_target_setup_or_halt(void)
{
  riic_target_clocks_or_halt();

  if (ra8_board_uart_console_init((uint32_t)k_riic_target_baud) != k_ra8_ok) {
    riic_target_panic_halt();
  }
  if (ra8_board_io_expander_apply_project_sw4_defaults() != k_ra8_ok) {
    riic_target_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    riic_target_panic_halt();
  }
}

/**
 * @brief Arm the RIIC1 target at 0x42 with a clock-stretched callback.
 *
 * @param[in,out] ctx Callback context to register (channel pre-filled).
 *
 * @return ``ra8_err_t`` from the attach + arm sequence.
 * @retval k_ra8_ok Target armed and the callback attached.
 *
 * @pre RIIC1 is initialized (``riic_target_setup_or_halt`` ran).
 * @pre ``ctx`` outlives every ``ra8_i2c_peripheral_dispatch`` call.
 * @post On success the channel answers 0x42 and fires ``riic_target_on_event``.
 * @post Clock stretching (ICMR3.WAIT) is armed so the poll loop can service.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t riic_target_arm(riic_target_ctx_t* ctx)
{
  if (ctx == nullptr) {
    return k_ra8_err_null_ptr;
  }
  const ra8_err_t attached =
    ra8_i2c_peripheral_attach_handler((uint8_t)k_riic_target_channel, riic_target_on_event, ctx);
  if (attached != k_ra8_ok) {
    return attached;
  }
  const ra8_i2c_peripheral_cfg_t cfg = {
    .own_addr_7b   = (uint8_t)k_riic_target_own_addr,
    .slot          = k_ra8_i2c_peripheral_slot_0,
    .general_call  = false,
    .clock_stretch = true,
    .irq_enable    = false, /* polled dispatch, no NVIC wiring */
  };
  return ra8_i2c_peripheral_init((uint8_t)k_riic_target_channel, &cfg);
}

/**
 * @brief Application entry. Arms the RIIC1 target and polls the dispatcher.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 * @post The CPU stays in the dispatch poll loop forever.
 * @post On any fatal init error the CPU parks in ``riic_target_panic_halt``.
 * @since 0.1.0
 */
void main(void)
{
  riic_target_setup_or_halt();
  ra8_isr_globals_enable();

  static riic_target_ctx_t s_ctx = {
    .channel = (uint8_t)k_riic_target_channel,
    .rx_len  = (uint32_t)k_riic_target_seed_len,
    .rx      = {(uint8_t)k_riic_target_seed_byte},
  };
  if (riic_target_arm(&s_ctx) != k_ra8_ok) {
    riic_target_panic_halt();
  }
  if (ra8_board_uart_console_write(k_riic_target_msg_armed, sizeof(k_riic_target_msg_armed) - 1U) !=
      k_ra8_ok) {
    riic_target_panic_halt();
  }

  while (1) {
    ra8_i2c_peripheral_dispatch((uint8_t)k_riic_target_channel);
  }

  riic_target_panic_halt();
}
