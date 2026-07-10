/**
 * @file esp_gpio.c
 * @brief "Ours" register-level GPIO backend implementing @ref esp_gpio_ops_t.
 *
 * @details
 * Drives a single 32-bit GPIO bank through the write-1-to-set / write-1-to-clear
 * registers so no read-modify-write race exists on the set/clear paths. `toggle`
 * is the one path that must read `GPIO_OUT` first. Every register touch carries a
 * TRM citation (all `[CONFIRM]` -- see @ref esp_soc.h).
 *
 * @note Single-threaded, IRQs masked. The backend is a singleton exposed via
 *       @ref esp_gpio_ours_ops.
 * @since 0.1.0 (spike)
 */

#include "esp_hal.h"
#include "esp_soc.h"

/**
 * @struct esp_gpio_ctx_t
 * @brief Per-backend state for the "ours" GPIO driver.
 * @invariant `ready` is true for the lifetime of the singleton.
 */
typedef struct esp_gpio_ctx {
    bool ready; /**< Marks the singleton as constructed (used for validation). */
} esp_gpio_ctx_t;

/**
 * @var s_esp_gpio_ctx
 * @brief Singleton state backing @ref esp_gpio_ours_ops.
 * @note File-scope; address handed out only through the vtable `ctx`.
 * @warning Do not mutate directly; go through the vtable.
 */
static esp_gpio_ctx_t s_esp_gpio_ctx = {.ready = true};

/**
 * @brief Build the single-bit mask for @p pin.
 * @param[in] pin GPIO index, 0..@ref k_esp_gpio_pin_max.
 * @return `1u << pin` as a 32-bit mask.
 * @pre @p pin <= @ref k_esp_gpio_pin_max (shift stays in range).
 * @post Exactly one bit is set.
 * @since 0.1.0 (spike)
 */
static inline uint32_t esp_gpio_pin_mask(uint8_t pin) {
    return ((uint32_t)1u) << pin;
}

/**
 * @brief Configure @p pin as a driven output (@ref esp_gpio_ops_t::init).
 * @param[in,out] ctx Backend cookie; must be @ref s_esp_gpio_ctx.
 * @param[in] pin GPIO index to enable, 0..@ref k_esp_gpio_pin_max.
 * @return esp_err_t
 * @retval k_esp_ok Output enabled for @p pin.
 * @retval k_esp_err_null_ptr @p ctx was null.
 * @retval k_esp_err_invalid_arg @p pin out of range.
 * @pre @p ctx is non-null.
 * @pre @p pin is a valid GPIO index.
 * @post The pin's output driver is enabled.
 * @note Pad IO-MUX function select is left at the ROM default; a real bench
 *       bring-up may need an IO_MUX write first (roadmap, see @ref esp_soc.h).
 * @since 0.1.0 (spike)
 */
static esp_err_t esp_gpio_ours_init(void* ctx, uint8_t pin) {
    esp_gpio_ctx_t* self = (esp_gpio_ctx_t*)ctx;
    if (self == nullptr) {
        return k_esp_err_null_ptr;
    }
    if (pin > k_esp_gpio_pin_max) {
        return k_esp_err_invalid_arg;
    }
    /* TRM Ch "GPIO Matrix" GPIO_ENABLE_W1TS_REG @0x24 -- enable output for pin [CONFIRM] */
    esp_gpio_regs()->ENABLE_W1TS = esp_gpio_pin_mask(pin);
    return k_esp_ok;
}

/**
 * @brief Drive @p pin high (@ref esp_gpio_ops_t::set).
 * @param[in,out] ctx Backend cookie; must be @ref s_esp_gpio_ctx.
 * @param[in] pin GPIO index, 0..@ref k_esp_gpio_pin_max.
 * @return esp_err_t
 * @retval k_esp_ok Pin driven high.
 * @retval k_esp_err_null_ptr @p ctx was null.
 * @retval k_esp_err_invalid_arg @p pin out of range.
 * @pre @p ctx is non-null.
 * @pre @p pin is a valid GPIO index.
 * @post The pin output latch bit is set.
 * @since 0.1.0 (spike)
 */
static esp_err_t esp_gpio_ours_set(void* ctx, uint8_t pin) {
    esp_gpio_ctx_t* self = (esp_gpio_ctx_t*)ctx;
    if (self == nullptr) {
        return k_esp_err_null_ptr;
    }
    if (pin > k_esp_gpio_pin_max) {
        return k_esp_err_invalid_arg;
    }
    /* TRM Ch "GPIO Matrix" GPIO_OUT_W1TS_REG @0x08 -- set output high [CONFIRM] */
    esp_gpio_regs()->OUT_W1TS = esp_gpio_pin_mask(pin);
    return k_esp_ok;
}

/**
 * @brief Drive @p pin low (@ref esp_gpio_ops_t::clear).
 * @param[in,out] ctx Backend cookie; must be @ref s_esp_gpio_ctx.
 * @param[in] pin GPIO index, 0..@ref k_esp_gpio_pin_max.
 * @return esp_err_t
 * @retval k_esp_ok Pin driven low.
 * @retval k_esp_err_null_ptr @p ctx was null.
 * @retval k_esp_err_invalid_arg @p pin out of range.
 * @pre @p ctx is non-null.
 * @pre @p pin is a valid GPIO index.
 * @post The pin output latch bit is cleared.
 * @since 0.1.0 (spike)
 */
static esp_err_t esp_gpio_ours_clear(void* ctx, uint8_t pin) {
    esp_gpio_ctx_t* self = (esp_gpio_ctx_t*)ctx;
    if (self == nullptr) {
        return k_esp_err_null_ptr;
    }
    if (pin > k_esp_gpio_pin_max) {
        return k_esp_err_invalid_arg;
    }
    /* TRM Ch "GPIO Matrix" GPIO_OUT_W1TC_REG @0x0C -- set output low [CONFIRM] */
    esp_gpio_regs()->OUT_W1TC = esp_gpio_pin_mask(pin);
    return k_esp_ok;
}

/**
 * @brief Invert the current output level of @p pin (@ref esp_gpio_ops_t::toggle).
 * @param[in,out] ctx Backend cookie; must be @ref s_esp_gpio_ctx.
 * @param[in] pin GPIO index, 0..@ref k_esp_gpio_pin_max.
 * @return esp_err_t
 * @retval k_esp_ok Pin level inverted.
 * @retval k_esp_err_null_ptr @p ctx was null.
 * @retval k_esp_err_invalid_arg @p pin out of range.
 * @pre @p ctx is non-null.
 * @pre @p pin is a valid GPIO index.
 * @post The pin output latch bit is the complement of its prior value.
 * @since 0.1.0 (spike)
 */
static esp_err_t esp_gpio_ours_toggle(void* ctx, uint8_t pin) {
    esp_gpio_ctx_t* self = (esp_gpio_ctx_t*)ctx;
    if (self == nullptr) {
        return k_esp_err_null_ptr;
    }
    if (pin > k_esp_gpio_pin_max) {
        return k_esp_err_invalid_arg;
    }
    const uint32_t mask = esp_gpio_pin_mask(pin);
    /* TRM Ch "GPIO Matrix" GPIO_OUT_REG @0x04 -- read current output latch [CONFIRM] */
    const uint32_t current = esp_gpio_regs()->OUT;
    if ((current & mask) != 0u) {
        /* TRM Ch "GPIO Matrix" GPIO_OUT_W1TC_REG @0x0C -- was high, drive low [CONFIRM] */
        esp_gpio_regs()->OUT_W1TC = mask;
    } else {
        /* TRM Ch "GPIO Matrix" GPIO_OUT_W1TS_REG @0x08 -- was low, drive high [CONFIRM] */
        esp_gpio_regs()->OUT_W1TS = mask;
    }
    return k_esp_ok;
}

/**
 * @var s_esp_gpio_ours_ops
 * @brief Fully-wired "ours" GPIO vtable handed out by @ref esp_gpio_ours_ops.
 * @note `const` so it lives in .rodata; `ctx` points at @ref s_esp_gpio_ctx.
 */
static const esp_gpio_ops_t s_esp_gpio_ours_ops = {
    .ctx = &s_esp_gpio_ctx,
    .init = esp_gpio_ours_init,
    .set = esp_gpio_ours_set,
    .clear = esp_gpio_ours_clear,
    .toggle = esp_gpio_ours_toggle,
};

const esp_gpio_ops_t* esp_gpio_ours_ops(void) {
    return &s_esp_gpio_ours_ops;
}
