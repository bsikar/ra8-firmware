/**
 * @file port/esp-hosted/src/ra8_esp_hosted_c6link.c
 * @brief The three trampolines that put `ra8_c6link` on this board's wire.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Each row goes through the vendored OS-abstraction vtable rather than
 * straight to the HAL, so the facade clocks its transactions through exactly
 * the code path the vendored driver would use. That is what makes a bench
 * result from the facade a bench result about the port: if the two used
 * different paths, a passing facade would say nothing about the driver.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_esp_hosted_c6link.h"

#include <stdint.h>
#include <string.h>

#include "esp_hosted_os_abstraction.h"
#include "port_esp_hosted_host_config.h"
#include "port_esp_hosted_host_os.h"
#include "port_esp_hosted_host_spi.h"
#include "ra8_attributes.h"
#include "ra8_c6link.h"
#include "ra8_c6link_transport.h"
#include "ra8_err.h"
#include "ra8_esp_hosted_port.h"
#include "transport_drv.h"

/**
 * @var s_ra8_esp_hosted_c6link_tx
 * @brief DMA-aligned staging buffer the transmit frame is clocked from.
 * @details The facade hands over a const pointer and the vendored transport
 * context wants a mutable one; copying rather than casting keeps the port
 * inside MISRA Rule 11.4 with no deviation to record, and guarantees the bus
 * always sees an aligned buffer.
 * @note Written only inside ::ra8_esp_hosted_c6link_transfer, on the pumping
 *       thread, which is also the only thread allowed to drive the bus.
 * @warning Not re-entrant: two threads pumping one link would interleave here,
 *          which is the same rule the facade already states about its handle.
 * @since 0.1.0
 */
alignas(k_ra8_c6link_dma_align) static uint8_t s_ra8_esp_hosted_c6link_tx[k_ra8_c6link_frame_bytes];

/**
 * @brief Clock one full-duplex transaction through the port's SPI slice.
 * @details Goes through the vendored OS-abstraction vtable rather than the SPI
 *        HAL, so the facade's transactions take exactly the path the vendored
 *        driver would.
 * @param[in] ctx Unused; the port is a singleton.
 * @param[in] tx Bytes to transmit; must be non-null.
 * @param[out] rx Where the received bytes land; must be non-null.
 * @param[in] len Transaction length in bytes.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The transaction completed.
 * @retval k_ra8_err_not_initialized The port is not up.
 * @retval k_ra8_err_spi_error The bus transfer did not return `RET_OK`.
 * @pre ::ra8_esp_hosted_port_init has succeeded.
 * @pre @p len is at most ::k_ra8_c6link_frame_bytes and both buffers cover it.
 * @post On success @p rx holds exactly @p len received bytes.
 * @post No port state outside the staging buffer is modified.
 * @note Runs on the caller's thread and blocks for the transaction.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
ra8_esp_hosted_c6link_transfer(void* ctx, const uint8_t* tx, uint8_t* rx, uint16_t len)
{
  (void)ctx;
  if (!ra8_esp_hosted_port_is_ready()) {
    return k_ra8_err_not_initialized;
  }
  if (len > (uint16_t)k_ra8_c6link_frame_bytes) {
    return k_ra8_err_invalid_size;
  }

  /* The vendored transport context's transmit pointer is not const, and
     casting the caller's const one to fit would be a pointer-to-integer
     conversion with no recorded MISRA deviation behind it. Staging the frame
     instead costs a memcpy of about a microsecond against a transaction that
     takes milliseconds, and it buys something real: the buffer the bus sees is
     always DMA-aligned regardless of what the caller allocated. */
  (void)memcpy(s_ra8_esp_hosted_c6link_tx, tx, (size_t)len);

  struct hosted_transport_context_t xfer = {};
  xfer.tx_buf                            = s_ra8_esp_hosted_c6link_tx;
  xfer.tx_buf_size                       = (uint32_t)len;
  xfer.rx_buf                            = rx;

  const int32_t moved = (int32_t)g_h.funcs->_h_do_bus_transfer(&xfer);
  return (moved == (int32_t)RET_OK) ? k_ra8_ok : k_ra8_err_spi_error;
}

/**
 * @brief Sample the HANDSHAKE line through the port's GPIO slice.
 * @details Reads the line through the same vtable row the vendored driver
 *        reads it through, so a bench result about the facade is a bench
 *        result about the port.
 * @param[in] ctx Unused; the port is a singleton.
 * @return true when the co-processor has armed itself for a transaction.
 * @retval true HANDSHAKE reads at its active level.
 * @retval false It does not, or the port is not up.
 * @pre ::ra8_esp_hosted_port_init has succeeded, or the answer is false.
 * @pre HANDSHAKE has been configured as an input by the port.
 * @post No port state is modified.
 * @post Exactly one pin read was performed.
 * @note Read through the vtable rather than the GPIO HAL, so the facade sees
 *       exactly what the vendored driver would see.
 * @since 0.1.0
 */
RA8_INTERNAL static bool ra8_esp_hosted_c6link_handshake(void* ctx)
{
  (void)ctx;
  if (!ra8_esp_hosted_port_is_ready()) {
    return false;
  }
  const int32_t level =
    (int32_t)g_h.funcs->_h_read_gpio(H_GPIO_HANDSHAKE_Port, (uint32_t)H_GPIO_HANDSHAKE_Pin);
  return level == (int32_t)H_HS_VAL_ACTIVE;
}

/**
 * @brief Yield for approximately the requested number of milliseconds.
 * @details The port's sleep is an RTOS sleep; routing it through the seam is
 *        what lets a host test bind one that costs no wall time.
 * @param[in] ctx Unused; the port is a singleton.
 * @param[in] ms Milliseconds to sleep.
 * @return Nothing.
 * @pre ::ra8_esp_hosted_port_init has succeeded, or the call is a no-op.
 * @pre The caller is a thread, not an interrupt handler.
 * @post At least @p ms milliseconds of ThreadX ticks have elapsed.
 * @post No port state is modified.
 * @note The whole point of routing this through the seam is that a host test
 *       binds a delay that costs no wall time.
 * @since 0.1.0
 */
RA8_INTERNAL static void ra8_esp_hosted_c6link_delay(void* ctx, uint16_t ms)
{
  (void)ctx;
  if (!ra8_esp_hosted_port_is_ready()) {
    return;
  }
  (void)g_h.funcs->_h_msleep((unsigned int)ms);
}

ra8_err_t ra8_esp_hosted_c6link_bind(ra8_c6link_transport_t* out)
{
  if (out == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *out = (ra8_c6link_transport_t){};
  if (!ra8_esp_hosted_port_is_ready()) {
    return k_ra8_err_not_initialized;
  }

  out->transfer         = ra8_esp_hosted_c6link_transfer;
  out->handshake_active = ra8_esp_hosted_c6link_handshake;
  out->delay_ms         = ra8_esp_hosted_c6link_delay;
  out->ctx              = nullptr;
  return k_ra8_ok;
}
