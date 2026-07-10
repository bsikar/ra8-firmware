/**
 * @file esp_soc.h
 * @brief ESP32-C6 SoC register map for the "ours" register-level drivers.
 *
 * @details
 * Typed-enum base addresses, register-block layouts, and inline
 * `volatile`-pointer accessors, following the RA8 tree's
 * `static inline volatile T* peripheral(void)` idiom. Only the registers the
 * spike touches (UART0 TX path, GPIO output path) are modelled; the rest of
 * each block is reserved padding so `offsetof` stays exact.
 *
 * @warning Every base address, register offset, and bitfield below is drawn
 *          from general ESP32-C6 knowledge, NOT from an open TRM, and is
 *          therefore tagged `[CONFIRM]` in its citation. A bench bring-up must
 *          verify each against the ESP32-C6 Technical Reference Manual before
 *          this is trusted on silicon. The `static_assert`s only prove the C
 *          struct layout matches the offset enums -- they cannot prove the
 *          offsets themselves are correct.
 *
 * @note The C6's on-board "user LED" (DevKitC/DevKitM GPIO8) is an ADDRESSABLE
 *       WS2812 driven over the RMT peripheral, NOT a plain push-pull GPIO. The
 *       spike therefore drives a configurable generic output pin; which pin
 *       maps to a visible LED on a given board is a bench confirm.
 * @since 0.1.0 (spike)
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @enum esp_soc_base_t
 * @brief Peripheral base addresses in the ESP32-C6 address space.
 * @details Values are `uintptr_t` so pointer casts do not truncate on the
 *          64-bit host if this header is ever compiled for a simulator.
 */
typedef enum : uintptr_t {
    /* TRM Ch "System and Memory" HP SRAM origin [CONFIRM] */
    k_esp_hp_sram_base = 0x40800000, /**< HP SRAM, 512 KiB, unified I/D. */
    /* TRM Ch "System and Memory" LP SRAM origin [CONFIRM] */
    k_esp_lp_sram_base = 0x50000000, /**< LP SRAM, 16 KiB (unused by the spike). */
    /* TRM Ch "UART Controller" UART0 block base [CONFIRM] */
    k_esp_uart0_base = 0x60000000,   /**< UART0 register block. */
    /* TRM Ch "IO MUX" IO MUX block base [CONFIRM] */
    k_esp_io_mux_base = 0x60090000,  /**< IO MUX register block (pad function select). */
    /* TRM Ch "GPIO Matrix" GPIO block base [CONFIRM] */
    k_esp_gpio_base = 0x60091000,    /**< GPIO matrix register block. */
} esp_soc_base_t;

/**
 * @struct esp_uart_regs_t
 * @brief UART0 register block (only the TX-path registers the spike needs).
 * @details Reserved words pad the gaps so @ref esp_uart_regs_t::STATUS lands at
 *          its documented offset; verified by `static_assert` below.
 * @invariant `sizeof` covers offsets 0x00..0x1F.
 */
typedef struct esp_uart_regs {
    volatile uint32_t FIFO;               /**< 0x00 R/W FIFO data (TX write / RX read). */
    volatile uint32_t reserved_04_18[6];  /**< 0x04..0x18 not modelled. */
    volatile uint32_t STATUS;             /**< 0x1C RO FIFO/line status incl. TXFIFO_CNT. */
} esp_uart_regs_t;

/**
 * @enum esp_uart_reg_off_t
 * @brief Documented byte offsets of the modelled UART registers.
 * @details Single source for the offsets asserted against
 *          @ref esp_uart_regs_t below; keeps the layout checks free of
 *          magic numbers.
 */
typedef enum : uint16_t {
    /* TRM Ch "UART Controller" UART_FIFO_REG offset [CONFIRM] */
    k_esp_uart_off_fifo = 0x00,   /**< UART_FIFO_REG byte offset. */
    /* TRM Ch "UART Controller" UART_STATUS_REG offset [CONFIRM] */
    k_esp_uart_off_status = 0x1C, /**< UART_STATUS_REG byte offset. */
} esp_uart_reg_off_t;

static_assert(offsetof(esp_uart_regs_t, FIFO) == k_esp_uart_off_fifo,
              "UART FIFO must be at its documented offset");
static_assert(offsetof(esp_uart_regs_t, STATUS) == k_esp_uart_off_status,
              "UART STATUS must be at its documented offset");

/**
 * @enum esp_uart_status_field_t
 * @brief Extraction of the TXFIFO_CNT field from `UART_STATUS_REG`.
 * @details `count = (STATUS >> shift) & mask`. Room exists when
 *          `count < k_esp_uart_fifo_depth`.
 */
typedef enum : uint32_t {
    /* TRM Ch "UART Controller" UART_STATUS_REG TXFIFO_CNT bit position [CONFIRM] */
    k_esp_uart_txfifo_cnt_shift = 16,     /**< LSB of the TXFIFO_CNT field. */
    /* TRM Ch "UART Controller" UART_STATUS_REG TXFIFO_CNT width (8-bit, [23:16]) [CONFIRM] */
    k_esp_uart_txfifo_cnt_mask = 0xFF,    /**< Field mask after shifting. */
} esp_uart_status_field_t;

/**
 * @enum esp_uart_fifo_t
 * @brief UART TX FIFO geometry.
 */
typedef enum : uint16_t {
    /* TRM Ch "UART Controller" TX FIFO depth (bytes) [CONFIRM] */
    k_esp_uart_fifo_depth = 128, /**< Bytes the TX FIFO holds; room if count is below this. */
} esp_uart_fifo_t;

/**
 * @struct esp_gpio_regs_t
 * @brief GPIO matrix register block (only the output-path registers).
 * @details Single 32-bit bank (the C6 has one GPIO word). Reserved words pad
 *          the gaps; offsets verified by `static_assert` below.
 * @invariant `sizeof` covers offsets 0x00..0x2B.
 */
typedef struct esp_gpio_regs {
    volatile uint32_t reserved_00;        /**< 0x00 BT_SELECT, not modelled. */
    volatile uint32_t OUT;                /**< 0x04 R/W output data (read for toggle). */
    volatile uint32_t OUT_W1TS;           /**< 0x08 WO write-1-to-set output bits. */
    volatile uint32_t OUT_W1TC;           /**< 0x0C WO write-1-to-clear output bits. */
    volatile uint32_t reserved_10_1c[4];  /**< 0x10..0x1C not modelled. */
    volatile uint32_t ENABLE;             /**< 0x20 R/W output-enable data. */
    volatile uint32_t ENABLE_W1TS;        /**< 0x24 WO write-1-to-set output enable. */
    volatile uint32_t ENABLE_W1TC;        /**< 0x28 WO write-1-to-clear output enable. */
} esp_gpio_regs_t;

/**
 * @enum esp_gpio_reg_off_t
 * @brief Documented byte offsets of the modelled GPIO matrix registers.
 * @details Single source for the offsets asserted against
 *          @ref esp_gpio_regs_t below; keeps the layout checks free of
 *          magic numbers.
 */
typedef enum : uint16_t {
    /* TRM Ch "GPIO Matrix" GPIO_OUT_REG offset [CONFIRM] */
    k_esp_gpio_off_out = 0x04,         /**< GPIO_OUT_REG byte offset. */
    /* TRM Ch "GPIO Matrix" GPIO_OUT_W1TS_REG offset [CONFIRM] */
    k_esp_gpio_off_out_w1ts = 0x08,    /**< GPIO_OUT_W1TS_REG byte offset. */
    /* TRM Ch "GPIO Matrix" GPIO_OUT_W1TC_REG offset [CONFIRM] */
    k_esp_gpio_off_out_w1tc = 0x0C,    /**< GPIO_OUT_W1TC_REG byte offset. */
    /* TRM Ch "GPIO Matrix" GPIO_ENABLE_W1TS_REG offset [CONFIRM] */
    k_esp_gpio_off_enable_w1ts = 0x24, /**< GPIO_ENABLE_W1TS_REG byte offset. */
    /* TRM Ch "GPIO Matrix" GPIO_ENABLE_W1TC_REG offset [CONFIRM] */
    k_esp_gpio_off_enable_w1tc = 0x28, /**< GPIO_ENABLE_W1TC_REG byte offset. */
} esp_gpio_reg_off_t;

static_assert(offsetof(esp_gpio_regs_t, OUT) == k_esp_gpio_off_out,
              "GPIO OUT must be at its documented offset");
static_assert(offsetof(esp_gpio_regs_t, OUT_W1TS) == k_esp_gpio_off_out_w1ts,
              "GPIO OUT_W1TS must be at its documented offset");
static_assert(offsetof(esp_gpio_regs_t, OUT_W1TC) == k_esp_gpio_off_out_w1tc,
              "GPIO OUT_W1TC must be at its documented offset");
static_assert(offsetof(esp_gpio_regs_t, ENABLE_W1TS) == k_esp_gpio_off_enable_w1ts,
              "GPIO ENABLE_W1TS must be at its documented offset");
static_assert(offsetof(esp_gpio_regs_t, ENABLE_W1TC) == k_esp_gpio_off_enable_w1tc,
              "GPIO ENABLE_W1TC must be at its documented offset");

/**
 * @enum esp_gpio_pin_limit_t
 * @brief Valid GPIO pin-index range for output.
 */
typedef enum : uint8_t {
    /* TRM Ch "GPIO Matrix" highest GPIO index (GPIO0..GPIO30) [CONFIRM] */
    k_esp_gpio_pin_max = 30, /**< Inclusive maximum pin index accepted by the driver. */
} esp_gpio_pin_limit_t;

/**
 * @brief Typed accessor for the UART0 register block.
 * @return Volatile pointer to UART0 at @ref k_esp_uart0_base.
 * @retval non-null Always; a fixed MMIO address.
 * @pre UART0 is powered (ROM default).
 * @pre The address map matches @ref k_esp_uart0_base.
 * @post The returned pointer aliases live MMIO; reads/writes have side effects.
 * @note Not thread-safe; MMIO has no locking.
 * @since 0.1.0 (spike)
 */
static inline volatile esp_uart_regs_t* esp_uart0(void) {
    return (volatile esp_uart_regs_t*)k_esp_uart0_base;
}

/**
 * @brief Typed accessor for the GPIO matrix register block.
 * @return Volatile pointer to the GPIO block at @ref k_esp_gpio_base.
 * @retval non-null Always; a fixed MMIO address.
 * @pre The GPIO peripheral is powered (ROM default).
 * @pre The address map matches @ref k_esp_gpio_base.
 * @post The returned pointer aliases live MMIO; reads/writes have side effects.
 * @note Not thread-safe; MMIO has no locking.
 * @since 0.1.0 (spike)
 */
static inline volatile esp_gpio_regs_t* esp_gpio_regs(void) {
    return (volatile esp_gpio_regs_t*)k_esp_gpio_base;
}
