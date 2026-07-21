/**
 * @file esp_hal.h
 * @brief Swappable driver interface (DIP seam) for the ESP32-C6 spike.
 *
 * @details
 * This header is the single abstraction boundary between the application
 * (`src/main.c`) and any concrete driver backend. It mirrors the RA8 tree's
 * `ra8_io_*` facades: the application depends only on the vtable structs
 * declared here (function-pointer interfaces), never on a specific
 * implementation. Two backends can therefore satisfy the same contract:
 *
 * - `drivers/ours/` -- our own register-level drivers (implemented today).
 * - `drivers/idf/`  -- an ESP-IDF-backed implementation (roadmap; see
 *   `drivers/idf/README.md`). Swapping it in changes ONE call in the
 *   composition root and touches no other caller.
 *
 * Each interface is a small struct of function pointers plus an opaque
 * `void* ctx` cookie, so a backend may carry per-instance state without the
 * caller knowing its shape (Interface Segregation + Dependency Inversion).
 *
 * @note All ops are synchronous and non-reentrant unless a backend documents
 *       otherwise. The spike backend is single-threaded, IRQs masked.
 * @since 0.1.0 (spike)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @enum esp_err_t
 * @brief Uniform error code returned by every HAL operation.
 *
 * @details Deliberately tiny; the spike does not need a rich error space.
 *          Mirrors the role of `ra8_err_t` in the RA8 tree.
 * @invariant `k_esp_ok` is always zero so `if (err)` reads as "failed".
 */
typedef enum : uint8_t {
  k_esp_ok              = 0, /**< Operation succeeded.                              */
  k_esp_err_invalid_arg = 1, /**< A caller-supplied argument was out of range.      */
  k_esp_err_null_ptr    = 2, /**< A required pointer argument was `nullptr`.        */
  k_esp_err_timeout     = 3, /**< A bounded hardware wait expired (Rule 2 cap hit). */
} esp_err_t;

/**
 * @struct esp_gpio_ops_t
 * @brief GPIO driver interface (the DIP seam for digital output pins).
 *
 * @details A backend fills every function pointer and points @ref ctx at its
 *          own state. Callers invoke only through these pointers.
 *
 * @invariant Every function pointer is non-null in a fully-wired backend.
 * @invariant `pin` is a SoC GPIO index; range checking is the backend's job.
 *
 * @par Example:
 * @code
 * const esp_gpio_ops_t* gpio = esp_gpio_ours_ops();
 * (void)gpio->init(gpio->ctx, 8);
 * (void)gpio->set(gpio->ctx, 8);
 * @endcode
 *
 * @see esp_gpio_ours_ops
 */
typedef struct esp_gpio_ops {
  void* ctx; /**< Opaque backend state; passed back to every call. */
  /** @brief Configure @p pin as a driven output. @return esp_err_t. */
  esp_err_t (*init)(void* ctx, uint8_t pin);
  /** @brief Drive @p pin high. @return esp_err_t. */
  esp_err_t (*set)(void* ctx, uint8_t pin);
  /** @brief Drive @p pin low. @return esp_err_t. */
  esp_err_t (*clear)(void* ctx, uint8_t pin);
  /** @brief Invert the current output level of @p pin. @return esp_err_t. */
  esp_err_t (*toggle)(void* ctx, uint8_t pin);
} esp_gpio_ops_t;

/**
 * @struct esp_uart_ops_t
 * @brief UART transmit interface (the DIP seam for console output).
 *
 * @details Receive is out of scope for the spike; only the transmit path the
 *          banner needs is defined. A backend fills every pointer and points
 *          @ref ctx at its own state.
 *
 * @invariant Every function pointer is non-null in a fully-wired backend.
 *
 * @par Example:
 * @code
 * const esp_uart_ops_t* uart = esp_uart_ours_ops();
 * (void)uart->init(uart->ctx, 115200u);
 * (void)uart->write(uart->ctx, (const uint8_t*)"hi\n", 3u);
 * @endcode
 *
 * @see esp_uart_ours_ops
 */
typedef struct esp_uart_ops {
  void* ctx; /**< Opaque backend state; passed back to every call. */
  /** @brief Bring the UART up at @p baud (bits/s). @return esp_err_t. */
  esp_err_t (*init)(void* ctx, uint32_t baud);
  /** @brief Transmit a single @p byte (blocking, bounded). @return esp_err_t. */
  esp_err_t (*putc)(void* ctx, uint8_t byte);
  /** @brief Transmit @p len bytes from @p data. @return esp_err_t. */
  esp_err_t (*write)(void* ctx, const uint8_t* data, size_t len);
  /** @brief Block (bounded) until the TX FIFO has fully drained. @return esp_err_t. */
  esp_err_t (*flush)(void* ctx);
} esp_uart_ops_t;

/*
 * Concrete-implementation factories.
 *
 * The composition root (here, `app_main`) selects a backend by calling one of
 * these. This is the ONLY place a concrete backend is named; every subsequent
 * call goes through the returned vtable. An ESP-IDF backend would add a sibling
 * `esp_uart_idf_ops()` / `esp_gpio_idf_ops()` and the root would flip a single
 * call -- no other file changes (see `drivers/idf/README.md`).
 */

/**
 * @brief Return the "ours" (register-level) GPIO backend vtable.
 * @details Backed by `drivers/ours/esp_gpio.c` writing the SoC GPIO registers.
 * @return Pointer to a process-lifetime-static, fully-wired vtable.
 * @retval non-null Always; the vtable has static storage duration.
 * @pre The SoC GPIO peripheral is powered (ROM default on the C6).
 * @pre Caller uses the returned pointer only from a single thread.
 * @post Every function pointer in the returned struct is non-null.
 * @post `ctx` points at the backend's static state.
 * @note Thread-unsafe: the returned backend is a singleton.
 * @see esp_gpio_ops_t
 * @since 0.1.0 (spike)
 */
const esp_gpio_ops_t* esp_gpio_ours_ops(void);

/**
 * @brief Return the "ours" (register-level) UART backend vtable.
 * @details Backed by `drivers/ours/esp_uart.c` writing the SoC UART0 registers.
 * @return Pointer to a process-lifetime-static, fully-wired vtable.
 * @retval non-null Always; the vtable has static storage duration.
 * @pre UART0 is powered and its pins are routed (ROM download console default).
 * @pre Caller uses the returned pointer only from a single thread.
 * @post Every function pointer in the returned struct is non-null.
 * @post `ctx` points at the backend's static state.
 * @note Thread-unsafe: the returned backend is a singleton.
 * @see esp_uart_ops_t
 * @since 0.1.0 (spike)
 */
const esp_uart_ops_t* esp_uart_ours_ops(void);
