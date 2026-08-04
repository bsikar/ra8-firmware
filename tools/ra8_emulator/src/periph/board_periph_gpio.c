/**
 * @file board_periph_gpio.c
 * @brief GPIO / PORT peripheral-block model for the board emulator
 *
 * @details
 * Models the RA8D2 GPIO/PORT block (ra8_port_regs.h): per-port direction
 * (PDR) and output latch (PODR), the PCNTR1 combined register the FSP ioport
 * driver writes, and the PCNTR3 atomic set/clear. The block also tracks the
 * three EK-RA8D2 user LEDs (LED1 BLUE P600, LED2 GREEN P303, LED3 RED PA07) so
 * the graphical board view can light each indicator in its real colour and the
 * run summary can report each LED's level and transition count.
 *
 * Self-registers its descriptor (address range + read / write / reset) with the
 * board_periph core from a file-scope constructor, so the core needs no central
 * block list -- see board_periph_block.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_console.h"
#include "board_periph.h"
#include "board_periph_block.h"
#include "board_periph_eink.h"

/** @brief LED3 PORT/pin coordinates on the EK-RA8D2 (P10_07). */
typedef enum : uint32_t {
  k_led3_port                   = 10U,
  /**< Led3 port. */ k_led3_pin = 7U /**< Led3 pin. */
} gpio_lit_t;

/** @brief Console-tap line buffer capacity for a GPIO edge summary. */
typedef enum : uint32_t {
  k_gpio_console_line_cap = 48U, /**< Max chars in a "NAME -> ON/OFF" line. */
} gpio_console_t;

/** @brief GPIO/PORT block geometry (ra8_port_regs.h). */
typedef enum : uint64_t {
  k_port_base   = 0x40400000UL,  /**< PORT0 base.                   */
  k_port_stride = 0x20UL,        /**< Bytes between adjacent ports. */
  k_port_count  = 15UL,          /**< PORT0..PORT14.                */
  k_port_span   = 0x20UL * 15UL, /**< Full PORT address window.     */
  k_port_pcntr1 = 0x00UL,        /**< {PODR[31:16], PDR[15:0]} RW.  */
  k_port_pcntr2 = 0x04UL,        /**< {EIDR[31:16], PIDR[15:0]} R.  */
  k_port_pcntr3 = 0x08UL,        /**< {PORR[31:16], POSR[15:0]} W.  */
  k_port_pcntr4 = 0x0CUL,        /**< {EORR[31:16], EOSR[15:0]} RW. */
} port_map_t;

/** @brief Generic field shifts / masks shared by the PORT halves. */
typedef enum : uint32_t {
  k_half_shift    = 16U,     /**< High-half (PODR/PORR/EIDR) shift. */
  k_half_mask     = 0xFFFFU, /**< 16-bit half mask.                 */
  k_pins_per_port = 16U,     /**< Pins per PORT instance.           */
} port_field_t;

/** @brief EK-RA8D2 user-switch pins (PORT0): SW1=P009, SW2=P008 (UM Tbl 25). */
typedef enum : uint32_t {
  k_sw_port = 0U, /**< Both user switches are on PORT0. */
  k_sw1_pin = 9U, /**< SW1 -> P009.                     */
  k_sw2_pin = 8U, /**< SW2 -> P008.                     */
} sw_pin_t;

/** @brief RGB565 lit-colour codes for the three board LEDs. */
typedef enum : uint16_t {
  k_led_rgb565_blue  = 0x001FU, /**< LED1 blue  (P600) when driven high. */
  k_led_rgb565_green = 0x07E0U, /**< LED2 green (P303) when driven high. */
  k_led_rgb565_red   = 0xF800U, /**< LED3 red   (PA07) when driven high. */
} led_color_t;

/** @brief One PORT instance: direction + output latch (16 bits each). */
typedef struct {
  uint16_t pdr;    /**< Direction: 1 = output, 0 = input.            */
  uint16_t podr;   /**< Output-data latch.                           */
  uint16_t in_ovr; /**< Pins whose input level is externally driven. */
  uint16_t in_lvl; /**< Driven input level for the in_ovr pins.      */
} port_state_t;

/** @brief Board LED -> (port index, pin index, lit colour), from the BSP. */
typedef struct {
  uint8_t     port;  /**< Port.                                         */
  uint8_t     pin;   /**< Pin.                                          */
  uint16_t    color; /**< RGB565 colour the LED emits when driven high. */
  const char* name;  /**< Name.                                         */
} led_map_t;

static const led_map_t k_led_map[k_board_led_count] = {
  {6U, 0U, (uint16_t)k_led_rgb565_blue, "LED1 BLUE  P600"},
  {3U, 3U, (uint16_t)k_led_rgb565_green, "LED2 GREEN P303"},
  {(uint8_t)k_led3_port, (uint8_t)k_led3_pin, (uint16_t)k_led_rgb565_red, "LED3 RED   PA07"},
};

static port_state_t s_port[k_port_count];
static uint32_t     s_led_level[k_board_led_count];       /**< Last driven level. */
static uint32_t     s_led_transitions[k_board_led_count]; /**< 0->1 / 1->0 count. */

/** @brief Note a board-LED edge when a traced port/pin output latch changes. */
static void port_trace_leds(uint32_t port_idx, uint16_t before, uint16_t after)
{
  for (uint32_t i = 0U; i < (uint32_t)k_board_led_count; i++) {
    if (k_led_map[i].port != (uint8_t)port_idx) {
      continue;
    }
    const uint16_t mask = (uint16_t)(1U << k_led_map[i].pin);
    const uint32_t was  = ((before & mask) != 0U) ? 1U : 0U;
    const uint32_t now  = ((after & mask) != 0U) ? 1U : 0U;
    if (was != now) {
      s_led_level[i] = now;
      s_led_transitions[i]++;
      /* Console GPIO tab: one line per pin-level edge (bounded -- only the mapped
       * board LEDs, and only on a 0<->1 change, so a blink loop cannot flood). */
      char ln[k_gpio_console_line_cap];
      (void)snprintf(ln, sizeof(ln), "%s -> %s", k_led_map[i].name, now ? "ON" : "OFF");
      board_console_push(k_board_console_ch_gpio, ln);
      if (board_periph_trace()) {
        (void)fprintf(stderr, "  [trace] %s -> %s\n", k_led_map[i].name, now ? "ON" : "OFF");
      }
    }
  }
}

/** @brief Apply a new PODR value to a port and trace any LED transition. */
static void port_set_podr(uint32_t port_idx, uint16_t new_podr)
{
  const uint16_t old = s_port[port_idx].podr;
  if (old != new_podr) {
    port_trace_leds(port_idx, old, new_podr);
    s_port[port_idx].podr = new_podr;
  }
}

/** @brief Dispatch a PORT register read; returns PCNTR value for the port. */
static uint64_t port_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)size;
  const uint32_t idx = (uint32_t)((addr - (uint64_t)k_port_base) / (uint64_t)k_port_stride);
  const uint64_t off = (addr - (uint64_t)k_port_base) % (uint64_t)k_port_stride;
  if (idx >= (uint32_t)k_port_count) {
    return 0U;
  }
  const port_state_t* p = &s_port[idx];
  if (off == (uint64_t)k_port_pcntr1) {
    return ((uint32_t)p->podr << (uint32_t)k_half_shift) | (uint32_t)p->pdr;
  }
  if (off == (uint64_t)k_port_pcntr2) {
    /* PIDR reads the live pin level: an output pin reads back its driven latch
     * (so a "read what I drove" check is honest); an input pin reads any
     * externally-injected level (user buttons -- see board_periph_gpio_set_input),
     * else 0. */
    const uint16_t driven = (uint16_t)(p->podr & p->pdr);
    const uint16_t inputs = (uint16_t)((uint16_t)(~p->pdr) & p->in_ovr & p->in_lvl);
    return (uint32_t)(uint16_t)(driven | inputs);
  }
  return 0U; /* PCNTR3 is write-only; PCNTR4 unmodelled -> 0. */
}

/** @brief Dispatch a PORT register write (PCNTR1 direction/latch, PCNTR3 set/clear). */
static void port_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  (void)size;
  const uint32_t idx = (uint32_t)((addr - (uint64_t)k_port_base) / (uint64_t)k_port_stride);
  const uint64_t off = (addr - (uint64_t)k_port_base) % (uint64_t)k_port_stride;
  if (idx >= (uint32_t)k_port_count) {
    return;
  }
  if (off == (uint64_t)k_port_pcntr1) {
    s_port[idx].pdr = (uint16_t)(value & (uint32_t)k_half_mask);
    port_set_podr(idx,
                  (uint16_t)(((uint32_t)value >> (uint32_t)k_half_shift) & (uint32_t)k_half_mask));
  } else if (off == (uint64_t)k_port_pcntr3) {
    const uint16_t posr = (uint16_t)((uint32_t)value & (uint32_t)k_half_mask);
    const uint16_t porr =
      (uint16_t)(((uint32_t)value >> (uint32_t)k_half_shift) & (uint32_t)k_half_mask);
    uint16_t podr = s_port[idx].podr;
    podr |= posr;            /* atomic set   */
    podr &= (uint16_t)~porr; /* atomic clear */
    port_set_podr(idx, podr);
  }
}

/** @brief Clear every PORT latch / direction and the per-LED observability state. */
static void port_reset(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_port_count; i++) {
    s_port[i] = (port_state_t){};
  }
  /* EK-RA8D2 user switches are active-low with pull-ups: idle (released) reads
   * high. Seed P009/P008 (SW1/SW2) high so a firmware poll sees "not pressed"
   * until board_periph_gpio_set_input() drives them low (a --button press). */
  const uint16_t sw_mask = (uint16_t)((1U << (uint32_t)k_sw1_pin) | (1U << (uint32_t)k_sw2_pin));
  s_port[k_sw_port].in_ovr |= sw_mask;
  s_port[k_sw_port].in_lvl |= sw_mask;
  for (uint32_t i = 0U; i < (uint32_t)k_board_led_count; i++) {
    s_led_level[i]       = 0U;
    s_led_transitions[i] = 0U;
  }
  /* An attached --eink IT8951 controller re-arms its HRDY "ready" GPIO input
   * high here, AFTER the ports are cleared, so the firmware's ra8_epaper HRDY
   * poll reads ready (a no-op when no controller is attached). Mirrors the
   * user-switch seeding above. */
  board_eink_apply_gpio_defaults();
}

void board_periph_gpio_set_input(uint8_t port, uint8_t pin, bool level)
{
  if ((uint32_t)port >= (uint32_t)k_port_count || (uint32_t)pin >= (uint32_t)k_pins_per_port) {
    return;
  }
  const uint16_t bit = (uint16_t)(1U << (uint32_t)pin);
  s_port[port].in_ovr |= bit;
  if (level) {
    s_port[port].in_lvl |= bit;
  } else {
    s_port[port].in_lvl &= (uint16_t)~bit;
  }
}

bool board_periph_gpio_get_input(uint8_t port, uint8_t pin)
{
  if ((uint32_t)port >= (uint32_t)k_port_count || (uint32_t)pin >= (uint32_t)k_pins_per_port) {
    return false;
  }
  const uint16_t bit = (uint16_t)(1U << (uint32_t)pin);
  return (s_port[port].in_lvl & bit) != 0U;
}

uint32_t board_periph_led_level(board_led_id_t led)
{
  if ((uint32_t)led >= (uint32_t)k_board_led_count) {
    return 0U;
  }
  return s_led_level[(uint32_t)led];
}

uint16_t board_periph_led_color_rgb565(board_led_id_t led)
{
  if ((uint32_t)led >= (uint32_t)k_board_led_count) {
    return 0U;
  }
  return k_led_map[(uint32_t)led].color;
}

/** @brief Print the board-LED final level and transition-count line. */
static void port_report(void)
{
  (void)fprintf(stderr, "  GPIO LEDs     :");
  for (uint32_t i = 0U; i < (uint32_t)k_board_led_count; i++) {
    (void)fprintf(stderr,
                  " [%s %s x%u]",
                  k_led_map[i].name,
                  s_led_level[i] ? "ON" : "OFF",
                  s_led_transitions[i]);
  }
  (void)fprintf(stderr, "\n");
}

/** @brief This block's descriptor (static lifetime; the core keeps the pointer). */
static const board_periph_block_t k_gpio_block = {
  .base   = (uint64_t)k_port_base,
  .span   = (uint64_t)k_port_span,
  .order  = (uint32_t)k_block_order_gpio,
  .read   = port_read,
  .write  = port_write,
  .tick   = nullptr,
  .reset  = port_reset,
  .report = port_report,
  .name   = "GPIO/PORT",
};

/** @brief Self-register the GPIO block before main runs (decentralized). */
[[gnu::constructor]] static void board_periph_gpio_register(void)
{
  board_periph_register_block(&k_gpio_block);
}
