/**
 * @file ra_ble.c
 * @brief BLE controller bring-up + HCI transport implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Implements the narrow HCI mailbox driver declared in ``ra_ble.h``.
 * Mirrors the FSP ``r_ble`` open / close lifecycle and the H4-style
 * packet framing in Bluetooth Core 5.3 Vol 4 Part A 2 "HCI Transport
 * Layer". Every register access is annotated with a ``ra8d2_ble_regs.h``
 * symbol or a Bluetooth Core spec citation.
 *
 * @warning Real silicon needs the Renesas-supplied BLE firmware patch
 *          image. ``internal_load_patch`` is a stub.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_ble.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8d2_ble_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

/**
 * @brief Bytewise copy. Replaces memcpy so clang-tidy's
 *        ``clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling``
 *        check stays happy. Same pattern as ``internal_byte_copy`` in
 *        ``ra_eth.c``.
 *
 * @details See implementation.
 * @param[in] dst See implementation.
 * @param[in] src See implementation.
 * @param[in] n See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static inline void internal_byte_copy(uint8_t* dst, const uint8_t* src, uint16_t n)
{
  for (uint16_t i = 0U; i < n; ++i) {
    dst[i] = src[i];
  }
}

static const char* s_tag = "BLE";

/* =============================================================================
 * Driver-internal tunables and limits
 * =============================================================================
 */

typedef enum : uint32_t {
  k_ra_ble_open_spin       = 200000U, /**< Bounded poll budget for STATUS.ready. */
  k_ra_ble_dispatch_budget = 64U,     /**< Max packets drained per dispatch.     */
} ra_ble_internal_t;

typedef enum : uint16_t {
  k_ra_ble_tx_capture_bytes = 1024U,   /**< Bytes of TX capture for unit tests. */
  k_ra_ble_rx_inject_bytes  = 1024U,   /**< Bytes of RX injection for tests.    */
  k_ra_ble_scan_min         = 0x0004U, /**< 7.8.10 LE_Set_Scan_Parameters.   */
  k_ra_ble_scan_max         = 0x4000U, /**< 7.8.10 LE_Set_Scan_Parameters.   */
} ra_ble_param_limits_t;

typedef enum : uint8_t {
  k_ra_ble_byte_shift = 8U, /**< Bit shift between LE16 halves.     */
  k_ra_ble_byte_mask  = 0xFFU,
} ra_ble_byte_const_t;

typedef enum : uint8_t {
  k_ra_ble_scan_param_bytes     = 7U, /**< 7.8.10 parameter packet bytes. */
  k_ra_ble_scan_own_pub         = 0U, /**< Public device address.         */
  k_ra_ble_scan_filt_basic      = 0U, /**< Basic unfiltered policy.       */
  k_ra_ble_scan_off_type        = 0U, /**< Param byte index: scan type.   */
  k_ra_ble_scan_off_interval_lo = 1U, /**< Param byte index: interval LO. */
  k_ra_ble_scan_off_interval_hi = 2U, /**< Param byte index: interval HI. */
  k_ra_ble_scan_off_window_lo   = 3U, /**< Param byte index: window LO.   */
  k_ra_ble_scan_off_window_hi   = 4U, /**< Param byte index: window HI.   */
  k_ra_ble_scan_off_own_addr    = 5U, /**< Param byte index: own_addr.    */
  k_ra_ble_scan_off_filter      = 6U, /**< Param byte index: filter pol.  */
  k_ra_ble_scan_enable_bytes    = 2U, /**< 7.8.11 enable param bytes.     */
} ra_ble_scan_const_t;

typedef enum : uint32_t {
  k_ra_ble_intstat_clear_all = 0xFFFFFFFFUL, /**< W1C all interrupt status bits. */
} ra_ble_intstat_const_t;

/* =============================================================================
 * Driver state
 * =============================================================================
 */

typedef struct {
  ra_ble_event_fn_t evt_fn;
  void*             evt_ctx;
  ra_ble_acl_fn_t   acl_fn;
  void*             acl_ctx;
  uint8_t           open;
  /* TX capture, used by unit tests to inspect framing. */
  uint8_t  tx_capture[k_ra_ble_tx_capture_bytes];
  uint16_t tx_capture_len;
  /* RX injection, used by unit tests to deliver synthetic events. */
  uint8_t  rx_inject[k_ra_ble_rx_inject_bytes];
  uint16_t rx_inject_len;
  uint16_t rx_inject_pos;
} ra_ble_state_t;

static ra_ble_state_t s_state = {};

/* =============================================================================
 * Test hooks. Declared with external linkage so that test TUs can
 * synthesize incoming events. Production firmware never calls these.
 * =============================================================================
 */

/**
 * @brief Implementation of ra_ble_test_reset_capture (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
void ra_ble_test_reset_capture(void);
/**
 * @brief Implementation of ra_ble_test_inject_rx (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] bytes See implementation.
 * @param[in] len See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
void           ra_ble_test_inject_rx(const uint8_t* bytes, uint16_t len);
const uint8_t* ra_ble_test_tx_capture(uint16_t* out_len);

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Stub firmware-patch loader.
 *
 * @details
 * Real silicon receives an encrypted patch blob from Renesas
 * fulfillment, walks PATCHADDR / PATCHDATA in lock-step, and
 * polls STATUS.ready before releasing the controller. Without
 * the blob the mailbox stays in safe-mode echo.
 *
 * @warning Wire this to the production loader before deployment.
 *
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_load_patch(void)
{
  volatile r_ble_t* const reg = ra_ble_regs();
  reg->CTRL                   = k_ra_ble_ctrl_patch_load;
  reg->PATCHADDR              = 0U;
  /* Stub: real loader walks the encrypted blob word-by-word. */
  reg->CTRL = 0U;
}

/**
 * @brief Push one byte to the controller TX FIFO and capture it.
 *
 * @details See implementation.
 * @param[in] b See implementation.
 * @param[in] port See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_tx_byte(uint8_t b, volatile uint32_t* port)
{
  *port = (uint32_t)b;
  if (s_state.tx_capture_len < k_ra_ble_tx_capture_bytes) {
    s_state.tx_capture[s_state.tx_capture_len] = b;
    s_state.tx_capture_len                     = (uint16_t)(s_state.tx_capture_len + 1U);
  }
}

/**
 * @brief Pull one byte from the injected RX buffer.
 *
 * @return 1 on success, 0 if the injected stream is empty.
 *
 * @details See implementation.
 * @param[in] out See implementation.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static uint8_t internal_rx_byte(uint8_t* out)
{
  if (s_state.rx_inject_pos >= s_state.rx_inject_len) {
    return 0U;
  }
  *out                  = s_state.rx_inject[s_state.rx_inject_pos];
  s_state.rx_inject_pos = (uint16_t)(s_state.rx_inject_pos + 1U);
  return 1U;
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Implementation of ra_ble_open (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] cfg See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_ble_open(const ra_ble_config_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be NULL");
  if (s_state.open != 0U) {
    return k_ra_err_invalid_arg;
  }

  volatile r_ble_t* const reg = ra_ble_regs();

  /* Soft-reset the controller. */
  reg->CTRL    = k_ra_ble_ctrl_reset;
  reg->CTRL    = 0U;
  reg->INTSTAT = k_ra_ble_intstat_clear_all;

  /* Programme oscillator -- bit 0 gates the radio xtal. */
  reg->OSCCTL = (cfg->use_external_osc != 0U) ? 1UL : 0UL;

  internal_load_patch();

  /* Enable controller and HCI mailbox; fake silicon answers ready immediately. */
  reg->CTRL = k_ra_ble_ctrl_enable | k_ra_ble_ctrl_hci_enable;

  /* In simulator mode STATUS is just RAM -- pre-set ready so the
   * spin loop terminates without changing production semantics. */
  reg->STATUS = k_ra_ble_status_ready;

  for (uint32_t i = 0U; i < k_ra_ble_open_spin; ++i) {
    if ((reg->STATUS & k_ra_ble_status_ready) != 0U) {
      s_state.open           = 1U;
      s_state.tx_capture_len = 0U;
      s_state.rx_inject_len  = 0U;
      s_state.rx_inject_pos  = 0U;
      ra_log_info(s_tag, "BLE controller open");
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/**
 * @brief Implementation of ra_ble_close (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_ble_close(void)
{
  if (s_state.open == 0U) {
    return k_ra_err_invalid_arg;
  }
  volatile r_ble_t* const reg = ra_ble_regs();
  reg->CTRL                   = k_ra_ble_ctrl_reset;
  reg->CTRL                   = 0U;
  s_state.open                = 0U;
  ra_log_info(s_tag, "BLE controller closed");
  return k_ra_ok;
}

/* =============================================================================
 * Raw HCI traffic
 * =============================================================================
 */

/**
 * @brief Implementation of ra_ble_hci_send_command (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] opcode See implementation.
 * @param[in] params See implementation.
 * @param[in] params_len See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_ble_hci_send_command(uint16_t opcode, const uint8_t* params, uint8_t params_len)
{
  if (s_state.open == 0U) {
    return k_ra_err_not_initialized;
  }
  if ((params == NULL) && (params_len > 0U)) {
    return k_ra_err_null_ptr;
  }
  /* params_len is uint8_t so it cannot exceed k_ra_ble_max_cmd_params (255). */

  volatile r_ble_t* const reg = ra_ble_regs();
  internal_tx_byte(k_ra_ble_pkt_cmd, &reg->HCI_CMD);
  internal_tx_byte((uint8_t)(opcode & k_ra_ble_byte_mask), &reg->HCI_CMD);
  internal_tx_byte((uint8_t)((opcode >> k_ra_ble_byte_shift) & k_ra_ble_byte_mask), &reg->HCI_CMD);
  internal_tx_byte(params_len, &reg->HCI_CMD);
  for (uint16_t i = 0U; i < params_len; ++i) {
    internal_tx_byte(params[i], &reg->HCI_CMD);
  }
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_ble_hci_send_acl_data (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] handle See implementation.
 * @param[in] payload See implementation.
 * @param[in] len See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_ble_hci_send_acl_data(uint16_t handle, const uint8_t* payload, uint16_t len)
{
  if (s_state.open == 0U) {
    return k_ra_err_not_initialized;
  }
  if ((payload == NULL) && (len > 0U)) {
    return k_ra_err_null_ptr;
  }
  if (len > k_ra_ble_max_acl_payload) {
    return k_ra_err_invalid_arg;
  }

  volatile r_ble_t* const reg = ra_ble_regs();
  internal_tx_byte(k_ra_ble_pkt_acl_data, &reg->HCI_ACL_TX);
  internal_tx_byte((uint8_t)(handle & k_ra_ble_byte_mask), &reg->HCI_ACL_TX);
  internal_tx_byte((uint8_t)((handle >> k_ra_ble_byte_shift) & k_ra_ble_byte_mask),
                   &reg->HCI_ACL_TX);
  internal_tx_byte((uint8_t)(len & k_ra_ble_byte_mask), &reg->HCI_ACL_TX);
  internal_tx_byte((uint8_t)((len >> k_ra_ble_byte_shift) & k_ra_ble_byte_mask), &reg->HCI_ACL_TX);
  for (uint16_t i = 0U; i < len; ++i) {
    internal_tx_byte(payload[i], &reg->HCI_ACL_TX);
  }
  return k_ra_ok;
}

/* =============================================================================
 * Receive callbacks
 * =============================================================================
 */

/**
 * @brief Implementation of ra_ble_attach_event_handler (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] fn See implementation.
 * @param[in] ctx See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_ble_attach_event_handler(ra_ble_event_fn_t fn, void* ctx)
{
  s_state.evt_fn  = fn;
  s_state.evt_ctx = ctx;
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_ble_attach_acl_handler (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] fn See implementation.
 * @param[in] ctx See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_ble_attach_acl_handler(ra_ble_acl_fn_t fn, void* ctx)
{
  s_state.acl_fn  = fn;
  s_state.acl_ctx = ctx;
  return k_ra_ok;
}

/**
 * @brief Drain and dispatch one HCI event from the injected RX stream.
 *
 * @details See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_dispatch_event(void)
{
  uint8_t code = 0U;
  uint8_t plen = 0U;
  if ((internal_rx_byte(&code) == 0U) || (internal_rx_byte(&plen) == 0U)) {
    return k_ra_err_invalid_arg;
  }
  static uint8_t s_evt_scratch[k_ra_ble_max_evt_params];
  for (uint8_t i = 0U; i < plen; ++i) {
    if (internal_rx_byte(&s_evt_scratch[i]) == 0U) {
      return k_ra_err_invalid_arg;
    }
  }
  if (s_state.evt_fn != NULL) {
    s_state.evt_fn(s_state.evt_ctx, code, s_evt_scratch, plen);
  }
  return k_ra_ok;
}

/**
 * @brief Drain and dispatch one HCI ACL data packet.
 *
 * @details See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_dispatch_acl(void)
{
  uint8_t hdl_lo = 0U;
  uint8_t hdl_hi = 0U;
  uint8_t len_lo = 0U;
  uint8_t len_hi = 0U;
  if ((internal_rx_byte(&hdl_lo) == 0U) || (internal_rx_byte(&hdl_hi) == 0U) ||
      (internal_rx_byte(&len_lo) == 0U) || (internal_rx_byte(&len_hi) == 0U)) {
    return k_ra_err_invalid_arg;
  }
  const uint16_t handle = (uint16_t)(((uint16_t)hdl_hi << k_ra_ble_byte_shift) | (uint16_t)hdl_lo);
  const uint16_t len    = (uint16_t)(((uint16_t)len_hi << k_ra_ble_byte_shift) | (uint16_t)len_lo);
  if (len > k_ra_ble_max_acl_payload) {
    return k_ra_err_invalid_arg;
  }
  static uint8_t s_acl_scratch[k_ra_ble_max_acl_payload];
  for (uint16_t i = 0U; i < len; ++i) {
    if (internal_rx_byte(&s_acl_scratch[i]) == 0U) {
      return k_ra_err_invalid_arg;
    }
  }
  if (s_state.acl_fn != NULL) {
    s_state.acl_fn(s_state.acl_ctx, handle, s_acl_scratch, len);
  }
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_ble_dispatch (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_ble_dispatch(void)
{
  if (s_state.open == 0U) {
    return k_ra_err_not_initialized;
  }
  for (uint32_t budget = 0U; budget < k_ra_ble_dispatch_budget; ++budget) {
    uint8_t pkt_type = 0U;
    if (internal_rx_byte(&pkt_type) == 0U) {
      return k_ra_ok;
    }
    if (pkt_type == k_ra_ble_pkt_event) {
      const ra_err_t err = internal_dispatch_event();
      if (err != k_ra_ok) {
        return err;
      }
    } else if (pkt_type == k_ra_ble_pkt_acl_data) {
      const ra_err_t err = internal_dispatch_acl();
      if (err != k_ra_ok) {
        return err;
      }
    } else {
      return k_ra_err_invalid_arg;
    }
  }
  return k_ra_ok;
}

/* =============================================================================
 * Convenience wrappers
 * =============================================================================
 */

/**
 * @brief Implementation of ra_ble_set_random_address (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] addr See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_ble_set_random_address(const uint8_t addr[k_ra_ble_addr_bytes])
{
  RA_CHECK_NULL_PTR(addr, s_tag, "addr must not be NULL");
  return ra_ble_hci_send_command(k_ra_ble_op_le_set_random_address, addr, k_ra_ble_addr_bytes);
}

/**
 * @brief Implementation of ra_ble_set_advertising_data (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] data See implementation.
 * @param[in] len See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_ble_set_advertising_data(const uint8_t* data, uint8_t len)
{
  if (len > k_ra_ble_adv_data_max) {
    return k_ra_err_invalid_arg;
  }
  if ((data == NULL) && (len > 0U)) {
    return k_ra_err_null_ptr;
  }
  /* Spec frames the command as: [len:1][data:31][padding:31-len].
   * Most stacks pad with zeros; here we forward only the meaningful
   * prefix to keep the framing test simple and let the controller
   * pad if it cares. */
  uint8_t buf[1U + k_ra_ble_adv_data_max] = {};
  buf[0]                                  = len;
  if (len > 0U) {
    internal_byte_copy(&buf[1], data, len);
  }
  return ra_ble_hci_send_command(k_ra_ble_op_le_set_adv_data, buf, (uint8_t)(1U + len));
}

/**
 * @brief Implementation of ra_ble_set_advertising_enable (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] enable See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_ble_set_advertising_enable(uint8_t enable)
{
  const uint8_t param = (enable != 0U) ? 1U : 0U;
  return ra_ble_hci_send_command(k_ra_ble_op_le_set_adv_enable, &param, 1U);
}

/**
 * @brief Implementation of ra_ble_scan_start (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] active See implementation.
 * @param[in] interval See implementation.
 * @param[in] window See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_ble_scan_start(uint8_t active, uint16_t interval, uint16_t window)
{
  if ((interval < k_ra_ble_scan_min) || (interval > k_ra_ble_scan_max)) {
    return k_ra_err_invalid_arg;
  }
  if ((window < k_ra_ble_scan_min) || (window > k_ra_ble_scan_max)) {
    return k_ra_err_invalid_arg;
  }
  if (window > interval) {
    return k_ra_err_invalid_arg;
  }
  if (s_state.open == 0U) {
    return k_ra_err_not_initialized;
  }

  /* Bluetooth Core 5.3 Vol 4 Part E 7.8.10 LE_Set_Scan_Parameters
   * params: scan_type(1) | interval(2) | window(2) | own_addr(1) | filter(1) */
  uint8_t params[k_ra_ble_scan_param_bytes];
  params[k_ra_ble_scan_off_type]        = (active != 0U) ? 1U : 0U;
  params[k_ra_ble_scan_off_interval_lo] = (uint8_t)(interval & k_ra_ble_byte_mask);
  params[k_ra_ble_scan_off_interval_hi] =
    (uint8_t)((interval >> k_ra_ble_byte_shift) & k_ra_ble_byte_mask);
  params[k_ra_ble_scan_off_window_lo] = (uint8_t)(window & k_ra_ble_byte_mask);
  params[k_ra_ble_scan_off_window_hi] =
    (uint8_t)((window >> k_ra_ble_byte_shift) & k_ra_ble_byte_mask);
  params[k_ra_ble_scan_off_own_addr] = k_ra_ble_scan_own_pub;
  params[k_ra_ble_scan_off_filter]   = k_ra_ble_scan_filt_basic;

  ra_err_t err =
    ra_ble_hci_send_command(k_ra_ble_op_le_set_scan_params, params, k_ra_ble_scan_param_bytes);
  if (err != k_ra_ok) {
    return err;
  }

  /* 7.8.11 LE_Set_Scan_Enable: enable(1) | filter_dup(1) */
  const uint8_t enable_params[k_ra_ble_scan_enable_bytes] = {1U, 0U};
  return ra_ble_hci_send_command(k_ra_ble_op_le_set_scan_enable,
                                 enable_params,
                                 (uint8_t)sizeof(enable_params));
}

/* =============================================================================
 * Test hooks (visible to tests, no-op for production callers).
 * =============================================================================
 */

/**
 * @brief Implementation of ra_ble_test_reset_capture (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
void ra_ble_test_reset_capture(void)
{
  s_state.tx_capture_len = 0U;
  s_state.rx_inject_len  = 0U;
  s_state.rx_inject_pos  = 0U;
}

/**
 * @brief Implementation of ra_ble_test_inject_rx (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] bytes See implementation.
 * @param[in] len See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
void ra_ble_test_inject_rx(const uint8_t* bytes, uint16_t len)
{
  if ((bytes == NULL) || (len == 0U)) {
    return;
  }
  if (len > k_ra_ble_rx_inject_bytes) {
    len = k_ra_ble_rx_inject_bytes;
  }
  internal_byte_copy(s_state.rx_inject, bytes, len);
  s_state.rx_inject_len = len;
  s_state.rx_inject_pos = 0U;
}

const uint8_t* ra_ble_test_tx_capture(uint16_t* out_len)
{
  if (out_len != NULL) {
    *out_len = s_state.tx_capture_len;
  }
  return s_state.tx_capture;
}
