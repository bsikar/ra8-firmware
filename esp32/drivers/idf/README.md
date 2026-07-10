# drivers/idf -- ESP-IDF-backed HAL implementation (roadmap stub)

This directory is deliberately empty of code today. It documents how an
**ESP-IDF-backed** implementation of the `hal/esp_hal.h` interface drops in
**without touching a single caller** -- the whole point of the DIP seam.

## The seam

`src/main.c` depends only on `hal/esp_hal.h`: the `esp_gpio_ops_t` /
`esp_uart_ops_t` function-pointer vtables and the two factory declarations
`esp_gpio_ours_ops()` / `esp_uart_ours_ops()`. It includes no register header
and pokes no MMIO. The concrete backend is named in exactly one place -- the
composition root at the top of `app_main`.

Today that root wires the register-level "ours" backend:

```c
const esp_gpio_ops_t* gpio = esp_gpio_ours_ops();   /* drivers/ours */
const esp_uart_ops_t* uart = esp_uart_ours_ops();   /* drivers/ours */
```

## The swap

An IDF backend implements the same vtables on top of ESP-IDF driver calls and
exposes sibling factories. A signature sketch (`drivers/idf/esp_uart_idf.c`):

```c
#include "esp_hal.h"
#include "driver/uart.h"     /* ESP-IDF -- only ever included under drivers/idf */

typedef struct { uart_port_t port; bool initialized; } esp_uart_idf_ctx_t;
static esp_uart_idf_ctx_t s_ctx = { .port = UART_NUM_0, .initialized = false };

static esp_err_t idf_uart_init(void* ctx, uint32_t baud) {
    esp_uart_idf_ctx_t* self = (esp_uart_idf_ctx_t*)ctx;
    if (self == nullptr)  { return k_esp_err_null_ptr; }
    if (baud == 0u)       { return k_esp_err_invalid_arg; }
    const uart_config_t cfg = { .baud_rate = (int)baud, /* 8N1 ... */ };
    if (uart_param_config(self->port, &cfg) != ESP_OK) { return k_esp_err_invalid_arg; }
    self->initialized = true;
    return k_esp_ok;
}

static esp_err_t idf_uart_write(void* ctx, const uint8_t* data, size_t len) {
    esp_uart_idf_ctx_t* self = (esp_uart_idf_ctx_t*)ctx;
    if (self == nullptr) { return k_esp_err_null_ptr; }
    if (data == nullptr) { return k_esp_err_null_ptr; }
    return (uart_write_bytes(self->port, data, len) < 0)
               ? k_esp_err_timeout : k_esp_ok;
}
/* idf_uart_putc / idf_uart_flush likewise wrap uart_write_bytes / uart_wait_tx_done. */

static const esp_uart_ops_t s_idf_ops = {
    .ctx = &s_ctx, .init = idf_uart_init, .putc = idf_uart_putc,
    .write = idf_uart_write, .flush = idf_uart_flush,
};
const esp_uart_ops_t* esp_uart_idf_ops(void) { return &s_idf_ops; }
```

Flipping the backend is a **one-line change in the composition root** plus its
declaration in `hal/esp_hal.h`:

```c
const esp_uart_ops_t* uart = esp_uart_idf_ops();   /* was esp_uart_ours_ops() */
```

`app_main`, the blink loop, the banner, and every other caller are untouched.
Because the `esp_*_ops_t` contracts are identical, the two backends are Liskov
substitutes: any caller works with either.

## Why this stays a stub for the spike

Pulling in `driver/uart.h` requires the ESP-IDF build system and headers, which
this spike deliberately avoids (the whole exercise is "our own drivers + our own
build, no idf.py"). The IDF backend is the escape hatch for when the owner wants
Espressif's drivers instead; the interface is already shaped so that choice
costs one line. Building it will live behind an opt-in switch so the two
toolchains stay independent (mirroring how the RA8 tree keeps host and target
builds apart).
