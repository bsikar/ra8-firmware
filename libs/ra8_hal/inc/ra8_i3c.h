/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_i3c.h
 * @brief I3C Bus Interface driver -- unified native-I3C + I2C-compat modes
 * @ingroup grp_hal_comms
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Single driver for the RA8D2 I3C peripheral (I3C0 @ 0x4035F000). The
 * peripheral can run in two modes, selected per channel at init via
 * ::ra8_i3c_cfg_t::mode:
 *
 * - ::k_ra8_i3c_mode_native -- full MIPI I3C: dynamic address assignment
 *   (DAA), Common Command Codes (CCC), private SDR read/write, HDR mode,
 *   In-Band Interrupts (IBI), and peripheral (target) mode. Mirrors FSP
 *   ``r_i3c`` -- every transfer is a command descriptor pushed to NCMDQP
 *   with payload through NTDTBP0.
 * - ::k_ra8_i3c_mode_i2c -- legacy I2C-compatibility controller (7-bit static
 *   addressing, START/RESTART/STOP, ACK/NACK, bus scan, SMBus). Mirrors
 *   FSP ``r_iic_b``.
 *
 * The data path (::ra8_i3c_init, ::ra8_i3c_write, ::ra8_i3c_read,
 * ::ra8_i3c_transfer, ::ra8_i3c_deinit) is shared and dispatches on the
 * channel's configured mode. Mode-specific calls return
 * ::k_ra8_err_invalid_state when used against a channel in the other mode:
 * - I2C-only: ::ra8_i3c_scan, ::ra8_i3c_set_clock, ::ra8_i3c_get_errors,
 *   ::ra8_i3c_clear_errors, ::ra8_i3c_abort.
 * - Native-only: ::ra8_i3c_set_address, ::ra8_i3c_bus_enable,
 *   ::ra8_i3c_dynamic_address_assign, ::ra8_i3c_set_dynamic_address,
 *   ::ra8_i3c_reset_dynamic_addresses, ::ra8_i3c_send_ccc,
 *   ::ra8_i3c_recv_ccc, ::ra8_i3c_set_hdr_mode, ::ra8_i3c_ibi_enable,
 *   ::ra8_i3c_ibi_drain, ::ra8_i3c_ibi_read, ::ra8_i3c_target_open,
 *   ::ra8_i3c_enter_stop, ::ra8_i3c_exit_stop.
 *
 * See HUM Ch 40 "I3C Bus Interface (I3C)" pp 2445-2701 for the
 * register-level reference.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_i3c_mode_t
 * @brief Operating mode selected per channel at ::ra8_i3c_init.
 *
 * @details
 * The I3C peripheral is a single block that can act as a native MIPI I3C
 * controller/target or as a legacy I2C-compatibility controller. The mode is
 * fixed for the life of an init/deinit cycle and recorded in the driver's
 * per-channel state so the shared data-path functions can dispatch.
 *
 * @invariant Exactly one mode is active per channel between init/deinit.
 * @see ra8_i3c_cfg_t
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_i3c_mode_native = 0U, /**< Native MIPI I3C (DAA/CCC/SDR/HDR/IBI). */
  k_ra8_i3c_mode_i2c    = 1U, /**< Legacy I2C-compatibility controller.   */
} ra8_i3c_mode_t;

/**
 * @struct ra8_i3c_cfg_t
 * @brief Per-channel bring-up configuration for ::ra8_i3c_init.
 *
 * @details
 * @p bus_hz and @p pclka_hz are used by the I2C-compat mode to compute
 * the STDBR bit-rate divider; native mode currently ignores them (SCL is
 * driven from the I3C clock tree). @p mode selects the operating mode.
 *
 * @invariant @p bus_hz and @p pclka_hz are non-zero in I2C mode.
 * @code
 * const ra8_i3c_cfg_t cfg = {
 *   .mode = k_ra8_i3c_mode_i2c, .bus_hz = 100000U, .pclka_hz = 8000000U };
 * ra8_i3c_init(0U, &cfg);
 * @endcode
 * @see ra8_i3c_mode_t
 * @since 0.1.0
 */
typedef struct {
  ra8_i3c_mode_t mode;     /**< Operating mode for this channel.          */
  uint32_t       bus_hz;   /**< Target bus clock in Hz (I2C mode).        */
  uint32_t       pclka_hz; /**< Source clock for the bit-rate calc (I2C). */
} ra8_i3c_cfg_t;

/**
 * @enum ra8_i3c_err_t
 * @brief Latched error bits returned by ::ra8_i3c_get_errors (I2C mode).
 *
 * @details Mirrors the legacy IIC_B status: a bitwise OR of the conditions
 * latched since the last ::ra8_i3c_clear_errors.
 *
 * @invariant ::k_ra8_i3c_err_none is the all-clear sentinel (0).
 * @see ra8_i3c_get_errors
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_i3c_err_none     = 0x00U, /**< No latched error.           */
  k_ra8_i3c_err_arb_lost = 0x01U, /**< Arbitration lost (BST.ALF). */
  k_ra8_i3c_err_nack     = 0x02U, /**< NACK received (BST.NACKDF). */
  k_ra8_i3c_err_timeout  = 0x04U, /**< Bus timeout (BST.TODF).     */
} ra8_i3c_err_t;

/**
 * @typedef ra8_i3c_event_fn_t
 * @brief Event callback fired by ::ra8_i3c_dispatch.
 *
 * @param[in] ctx    Opaque context registered with ::ra8_i3c_attach_handler.
 * @param[in] status In native mode the INST snapshot; in I2C mode the latched
 *                   ::ra8_i3c_err_t mask.
 * @since 0.1.0
 */
typedef void (*ra8_i3c_event_fn_t)(void* ctx, uint32_t status);

/**
 * @struct ra8_i3c_daa_target_t
 * @brief One target entry filled in by ENTDAA (native mode).
 *
 * @details Each row passed to ::ra8_i3c_dynamic_address_assign is filled
 * with the 48-bit Provisional ID, BCR, and DCR read during ENTDAA; the
 * caller pre-populates @p dynamic_address with the address to hand out.
 *
 * @invariant @p dynamic_address is a valid 7-bit address on entry.
 * @see ra8_i3c_dynamic_address_assign
 * @since 0.1.0
 */
typedef struct {
  uint8_t pid[6];          /**< 48-bit Provisional ID (BE order).    */
  uint8_t bcr;             /**< Bus Characteristic Register byte.    */
  uint8_t dcr;             /**< Device Characteristic Register byte. */
  uint8_t dynamic_address; /**< Address assigned to this target.     */
} ra8_i3c_daa_target_t;

/**
 * @enum ra8_i3c_ibi_type_t
 * @brief Type encoded in an IBI status descriptor (native mode).
 *
 * @invariant Values match the MIPI IBI-ID low bits.
 * @see ra8_i3c_ibi_t
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_i3c_ibi_type_interrupt    = 0U, /**< Peripheral-initiated IBI. */
  k_ra8_i3c_ibi_type_hot_join     = 1U, /**< Hot-join request.         */
  k_ra8_i3c_ibi_type_main_request = 2U, /**< Mainship request.         */
} ra8_i3c_ibi_type_t;

/**
 * @struct ra8_i3c_ibi_t
 * @brief Decoded IBI inbound descriptor (native mode).
 *
 * @details Returned by ::ra8_i3c_ibi_read -- the 32-bit IBI status word at
 * NIBIQP split into address, type, payload length, and payload bytes.
 *
 * @invariant @p payload_len <= sizeof(@p payload).
 * @see ra8_i3c_ibi_read
 * @since 0.1.0
 */
typedef struct {
  uint8_t            address;     /**< Originating dynamic address.     */
  ra8_i3c_ibi_type_t type;        /**< IBI / HJ / MR classification.    */
  uint8_t            payload_len; /**< Bytes that follow in payload[].  */
  uint8_t            payload[8];  /**< Up to 8 bytes of payload.        */
  uint8_t            last;        /**< 1 if this is the final fragment. */
} ra8_i3c_ibi_t;

/**
 * @enum ra8_i3c_hdr_mode_t
 * @brief HDR transfer-mode selector for ::ra8_i3c_set_hdr_mode (native).
 *
 * @invariant Values map to NCMDQP word-0 [27:26].
 * @see ra8_i3c_set_hdr_mode
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_i3c_hdr_mode_sdr = 0U, /**< Plain SDR transfer (default). */
  k_ra8_i3c_hdr_mode_ddr = 1U, /**< HDR-DDR.                      */
  k_ra8_i3c_hdr_mode_ts  = 2U, /**< HDR-TS.                       */
} ra8_i3c_hdr_mode_t;

/* =========================================================================
 * Lifecycle (shared)
 * =========================================================================
 */

/**
 * @brief Initialise an I3C channel in the configured mode.
 *
 * @details
 * Ungates the I3C MSTP, runs the FSP-mirrored bring-up, and -- in I2C mode
 * -- programmes the bit-rate from @p cfg. The channel's mode is recorded so
 * later data-path calls dispatch correctly. Native mode leaves the bus
 * disabled (call ::ra8_i3c_bus_enable separately).
 *
 * @param[in] channel I3C channel index (0 on RA8D2).
 * @param[in] cfg     Bring-up configuration; must be non-NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Channel ready in the requested mode.
 * @retval k_ra8_err_null_ptr     @p cfg was NULL.
 * @retval k_ra8_err_invalid_arg  @p channel out of range, or zero clocks in I2C mode.
 * @retval k_ra8_err_hw_init_failed MSTP enable failed.
 *
 * @pre MSTP module is initialized.
 * @pre No other driver owns the I3C peripheral.
 * @post On success the channel's mode equals @p cfg->mode.
 * @post On failure no peripheral state is left half-configured.
 * @note Not thread-safe; call from single-threaded init.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_init(uint8_t channel, const ra8_i3c_cfg_t* cfg);

/**
 * @brief Tear down an I3C channel and gate the peripheral.
 *
 * @param[in] channel I3C channel index.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Channel torn down, MSTP gated.
 * @retval k_ra8_err_invalid_arg @p channel out of range.
 *
 * @pre @p channel was previously initialized.
 * @pre Caller is single-threaded with respect to the peripheral.
 * @post The channel is marked uninitialized.
 * @post The I3C MSTP is gated when no channel remains active.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_deinit(uint8_t channel);

/* =========================================================================
 * Data path (shared, mode-dispatched)
 * =========================================================================
 */

/**
 * @brief Write @p len bytes to @p addr.
 *
 * @details
 * In I2C mode this is a controller transmit to 7-bit @p addr with optional
 * repeated-START framing. In native mode it is a private SDR write to the
 * dynamic address @p addr; @p restart is ignored.
 *
 * @param[in] channel I3C channel index.
 * @param[in] addr    7-bit target/dynamic address.
 * @param[in] data    Bytes to transmit; may be NULL only when @p len is 0.
 * @param[in] len     Byte count.
 * @param[in] restart True to hold the bus for a following transfer (I2C).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              All bytes transmitted.
 * @retval k_ra8_err_null_ptr    @p data NULL with @p len > 0.
 * @retval k_ra8_err_invalid_arg @p channel or @p addr out of range.
 * @retval k_ra8_err_nack        Target did not acknowledge (I2C).
 * @retval k_ra8_err_hw_timeout  Bus stalled (I2C).
 *
 * @pre Channel initialized.
 * @pre In native mode the bus is enabled.
 * @post On success the payload has been queued/transmitted.
 * @post On failure the bus is released (I2C) where possible.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_i3c_write(uint8_t channel, uint8_t addr, const uint8_t* data, uint32_t len, bool restart);

/**
 * @brief Read @p len bytes from @p addr.
 *
 * @details I2C mode: controller receive from 7-bit @p addr. Native mode:
 * private SDR read from dynamic address @p addr; @p restart is ignored.
 *
 * @param[in]  channel I3C channel index.
 * @param[in]  addr    7-bit target/dynamic address.
 * @param[out] buf     Destination buffer; must be non-NULL.
 * @param[in]  len     Byte count (> 0).
 * @param[in]  restart True to hold the bus for a following transfer (I2C).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              All bytes received.
 * @retval k_ra8_err_null_ptr    @p buf NULL.
 * @retval k_ra8_err_invalid_arg @p channel/@p addr out of range, or @p len 0.
 * @retval k_ra8_err_nack        Target did not acknowledge (I2C).
 * @retval k_ra8_err_hw_timeout  Bus stalled (I2C).
 *
 * @pre Channel initialized.
 * @pre In native mode the bus is enabled.
 * @post On success @p buf holds the received bytes.
 * @post On failure the bus is released (I2C) where possible.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_i3c_read(uint8_t channel, uint8_t addr, uint8_t* buf, uint32_t len, bool restart);

/**
 * @brief Write-then-read with repeated-START framing (I2C mode).
 *
 * @details Transmits @p wr_len bytes, issues a repeated START, then reads
 * @p rd_len bytes from the same address. Native mode returns
 * ::k_ra8_err_invalid_state (use ::ra8_i3c_write / ::ra8_i3c_read or CCCs).
 *
 * @param[in]  channel I3C channel index.
 * @param[in]  addr    7-bit target address.
 * @param[in]  wr      Bytes to write; non-NULL when @p wr_len > 0.
 * @param[in]  wr_len  Write byte count.
 * @param[out] rd      Read destination; non-NULL when @p rd_len > 0.
 * @param[in]  rd_len  Read byte count.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Transfer completed.
 * @retval k_ra8_err_null_ptr      A required buffer was NULL.
 * @retval k_ra8_err_invalid_arg   @p channel/@p addr out of range.
 * @retval k_ra8_err_invalid_state Channel is in native mode.
 * @retval k_ra8_err_nack          Target did not acknowledge.
 *
 * @pre Channel initialized in I2C mode.
 * @pre @p wr_len + @p rd_len > 0.
 * @post On success both phases completed and STOP was issued.
 * @post On failure the bus is released.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_transfer(uint8_t        channel,
                                         uint8_t        addr,
                                         const uint8_t* wr,
                                         uint32_t       wr_len,
                                         uint8_t*       rd,
                                         uint32_t       rd_len);

/* =========================================================================
 * I2C-mode-only
 * =========================================================================
 */

/**
 * @brief Re-programme the I2C bit rate (I2C mode).
 *
 * @param[in] channel  I3C channel index.
 * @param[in] bus_hz   Target bus clock in Hz.
 * @param[in] pclka_hz Source clock for the divider calculation.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Bit rate updated.
 * @retval k_ra8_err_invalid_arg   @p channel out of range or zero clocks.
 * @retval k_ra8_err_invalid_state Channel is in native mode.
 *
 * @pre Channel initialized in I2C mode.
 * @pre @p bus_hz and @p pclka_hz are non-zero.
 * @post On success the divider reflects @p bus_hz / @p pclka_hz.
 * @post On failure the previous divider is unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_set_clock(uint8_t channel, uint32_t bus_hz, uint32_t pclka_hz);

/**
 * @brief Probe whether a 7-bit address acknowledges (I2C mode).
 *
 * @param[in]  channel   I3C channel index.
 * @param[in]  addr      7-bit address to probe.
 * @param[out] out_acked Set true if the address ACKed; false otherwise.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Probe completed (see @p out_acked).
 * @retval k_ra8_err_null_ptr      @p out_acked NULL.
 * @retval k_ra8_err_invalid_state Channel is in native mode.
 *
 * @pre Channel initialized in I2C mode.
 * @pre @p out_acked is non-NULL.
 * @post @p out_acked reflects the ACK/NACK outcome.
 * @post The bus is released with a STOP.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_scan(uint8_t channel, uint8_t addr, bool* out_acked);

/**
 * @brief Read the latched I2C error mask (I2C mode).
 *
 * @param[in]  channel  I3C channel index.
 * @param[out] out_mask OR of ::ra8_i3c_err_t bits since the last clear.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Mask returned.
 * @retval k_ra8_err_null_ptr      @p out_mask NULL.
 * @retval k_ra8_err_invalid_state Channel is in native mode.
 *
 * @pre Channel initialized in I2C mode.
 * @pre @p out_mask is non-NULL.
 * @post @p out_mask holds the latched error bits.
 * @post No peripheral state is mutated.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_get_errors(uint8_t channel, uint8_t* out_mask);

/**
 * @brief Clear latched I2C error bits (I2C mode).
 *
 * @param[in] channel I3C channel index.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Errors cleared.
 * @retval k_ra8_err_invalid_state Channel is in native mode.
 *
 * @pre Channel initialized in I2C mode.
 * @pre Caller is single-threaded.
 * @post The latched error mask reads ::k_ra8_i3c_err_none.
 * @post No transfer is in progress.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_clear_errors(uint8_t channel);

/**
 * @brief Abort an in-flight I2C transfer and release the bus (I2C mode).
 *
 * @param[in] channel I3C channel index.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Bus released.
 * @retval k_ra8_err_invalid_state Channel is in native mode.
 *
 * @pre Channel initialized in I2C mode.
 * @pre Caller is single-threaded.
 * @post The bus is idle and any held state is cleared.
 * @post A subsequent transfer starts from a clean state.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_abort(uint8_t channel);

/* =========================================================================
 * IRQ / status (shared)
 * =========================================================================
 */

/**
 * @brief Attach an event callback for a channel.
 *
 * @param[in] channel I3C channel index.
 * @param[in] fn      Callback (NULL to detach).
 * @param[in] ctx     Opaque context passed back to @p fn.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Handler registered.
 * @retval k_ra8_err_invalid_arg @p channel out of range.
 *
 * @pre Channel initialized.
 * @pre Caller is single-threaded with respect to dispatch.
 * @post The channel's handler equals @p fn.
 * @post The channel's context equals @p ctx.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_attach_handler(uint8_t channel, ra8_i3c_event_fn_t fn, void* ctx);

/**
 * @brief Snapshot status and fire the channel's callback.
 *
 * @details In native mode reads and clears the INST register and passes the
 * snapshot to the handler; in I2C mode reads and clears the latched IIC_B
 * error mask and passes it instead. A no-op when no handler is attached.
 *
 * @param[in] channel I3C channel index.
 *
 * @pre Channel initialized.
 * @pre A handler may or may not be attached.
 * @post The status flags are cleared after snapshot.
 * @post The handler (if any) has been invoked once.
 * @note Not thread-safe; call from IRQ or polled context.
 * @since 0.1.0
 */
void ra8_i3c_dispatch(uint8_t channel);

/* =========================================================================
 * Native-mode-only (single I3C0 instance)
 * =========================================================================
 */

/**
 * @brief Programme the primary dynamic address (native mode).
 * @param[in] addr 7-bit primary address.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Address programmed.
 * @retval k_ra8_err_invalid_arg   @p addr out of range.
 * @retval k_ra8_err_invalid_state I3C0 is not in native mode.
 * @pre I3C0 initialized in native mode.
 * @pre Caller is single-threaded.
 * @post MSDVAD reflects @p addr with the valid bit set.
 * @post The bus may still be disabled.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_set_address(uint32_t addr);

/**
 * @brief Enable/disable bus-primary operation via BCTL.BUSE (native mode).
 * @param[in] enable True to enable primary operation.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                BUSE updated.
 * @retval k_ra8_err_invalid_state I3C0 is not in native mode.
 * @pre I3C0 initialized in native mode.
 * @pre Caller is single-threaded.
 * @post BCTL.BUSE reflects @p enable.
 * @post No transfer is in progress.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_bus_enable(bool enable);

/**
 * @brief Read the native INST status register (native mode).
 * @param[out] out_mask INST snapshot.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Status returned.
 * @retval k_ra8_err_null_ptr      @p out_mask NULL.
 * @retval k_ra8_err_invalid_state I3C0 is not in native mode.
 * @pre I3C0 initialized in native mode.
 * @pre @p out_mask non-NULL.
 * @post @p out_mask holds the INST snapshot.
 * @post No peripheral state is mutated.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_get_status(uint32_t* out_mask);

/**
 * @brief Clear native INST status bits (native mode).
 * @param[in] mask Bits to clear.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Bits cleared.
 * @retval k_ra8_err_invalid_state I3C0 is not in native mode.
 * @pre I3C0 initialized in native mode.
 * @pre Caller is single-threaded.
 * @post The requested INST bits read 0.
 * @post Other bits are unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_clear_status(uint32_t mask);

/**
 * @brief Put native I3C0 into MSTP-gated stop (native mode).
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Stopped.
 * @retval k_ra8_err_invalid_state I3C0 is not in native mode.
 * @pre I3C0 initialized in native mode.
 * @pre Caller is single-threaded.
 * @post BCTL/CECTL cleared and the MSTP gated.
 * @post The bus is idle.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_enter_stop(void);

/**
 * @brief Exit MSTP-gated stop (native mode).
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Resumed.
 * @retval k_ra8_err_invalid_state I3C0 is not in native mode.
 * @pre I3C0 was put into stop via ::ra8_i3c_enter_stop.
 * @pre Caller is single-threaded.
 * @post The I3C MSTP is ungated.
 * @post The peripheral is ready for re-init of bus state.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_exit_stop(void);

/**
 * @brief Issue ENTDAA to enumerate I3C targets (native mode).
 * @param[in,out] targets      Caller array; rows updated in place.
 * @param[in]     target_count Number of valid rows (1..max).
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Command queued; FIFOs read.
 * @retval k_ra8_err_null_ptr      @p targets NULL.
 * @retval k_ra8_err_invalid_arg   @p target_count 0 or out of range.
 * @retval k_ra8_err_invalid_state I3C0 is not in native mode.
 * @pre I3C0 initialized in native mode and bus enabled.
 * @pre @p targets non-NULL.
 * @post NCMDQP holds the ENTDAA descriptor.
 * @post @p targets reflect responses received before STOP.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_dynamic_address_assign(ra8_i3c_daa_target_t* targets,
                                                       uint8_t               target_count);

/**
 * @brief Issue SETDASA to assign one target a dynamic address (native mode).
 * @param[in] static_addr  7-bit static address in use by the target.
 * @param[in] dynamic_addr 7-bit dynamic address to assign.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Descriptor queued.
 * @retval k_ra8_err_invalid_arg   Address out of range.
 * @retval k_ra8_err_invalid_state I3C0 is not in native mode.
 * @pre I3C0 initialized in native mode.
 * @pre Caller is single-threaded.
 * @post NCMDQP holds the SETDASA descriptor.
 * @post The command-queue-empty flag is cleared.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_set_dynamic_address(uint8_t static_addr, uint8_t dynamic_addr);

/**
 * @brief Issue RSTDAA to clear all dynamic addresses (native mode).
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Descriptor queued.
 * @retval k_ra8_err_invalid_state I3C0 is not in native mode.
 * @pre I3C0 initialized in native mode.
 * @pre Caller is single-threaded.
 * @post NCMDQP holds the RSTDAA descriptor.
 * @post The command-queue-empty flag is cleared.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_reset_dynamic_addresses(void);

/**
 * @brief Generic CCC send -- broadcast or directed write (native mode).
 * @param[in] ccc          CCC opcode.
 * @param[in] target_addr  Dynamic address (directed CCCs only).
 * @param[in] payload      Optional payload; NULL when @p len is 0.
 * @param[in] len          Payload byte count.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Descriptor queued.
 * @retval k_ra8_err_invalid_arg   Address out of range.
 * @retval k_ra8_err_null_ptr      @p payload NULL with @p len > 0.
 * @retval k_ra8_err_invalid_state I3C0 is not in native mode.
 * @pre I3C0 initialized in native mode.
 * @pre Caller is single-threaded.
 * @post NCMDQP holds the CCC descriptor.
 * @post Payload (if any) was pushed to NTDTBP0.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_i3c_send_ccc(uint8_t ccc, uint8_t target_addr, const uint8_t* payload, uint8_t len);

/**
 * @brief Generic CCC receive -- directed read (native mode).
 * @param[in]  ccc         Direct CCC opcode (bit 7 set).
 * @param[in]  target_addr Dynamic address.
 * @param[out] buf         Destination buffer.
 * @param[in]  max_len     Maximum bytes to drain (> 0).
 * @param[out] got_len     Bytes actually drained.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Read complete.
 * @retval k_ra8_err_invalid_arg   Non-direct CCC, address bad, or @p max_len 0.
 * @retval k_ra8_err_null_ptr      Output pointers NULL.
 * @retval k_ra8_err_invalid_state I3C0 is not in native mode.
 * @pre I3C0 initialized in native mode.
 * @pre @p buf and @p got_len non-NULL.
 * @post @p buf holds the response bytes.
 * @post @p got_len holds the drained count.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_i3c_recv_ccc(uint8_t ccc, uint8_t target_addr, uint8_t* buf, uint8_t max_len, uint8_t* got_len);

/**
 * @brief Programme the NCMDQP transfer-mode field for a target (native mode).
 * @param[in] target_addr 7-bit dynamic address.
 * @param[in] mode        HDR-mode selector.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Descriptor queued.
 * @retval k_ra8_err_invalid_arg   @p target_addr or @p mode out of range.
 * @retval k_ra8_err_invalid_state I3C0 is not in native mode.
 * @pre I3C0 initialized in native mode and bus enabled.
 * @pre Caller is single-threaded.
 * @post NCMDQP holds an HDR-mode descriptor.
 * @post The command-queue-empty flag is cleared.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_set_hdr_mode(uint8_t target_addr, ra8_i3c_hdr_mode_t mode);

/**
 * @brief Enable inbound IBI capture for a target (native mode).
 * @param[in] target_addr 7-bit dynamic address authorised to push an IBI.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                IBI capture enabled.
 * @retval k_ra8_err_invalid_arg   @p target_addr out of range.
 * @retval k_ra8_err_invalid_state I3C0 is not in native mode.
 * @pre I3C0 initialized in native mode.
 * @pre Caller is in IRQ-masked or init context.
 * @post NTIBIVCTL.VLCNT is set for the target.
 * @post Subsequent IBIs from the target are queued.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_ibi_enable(uint8_t target_addr);

/**
 * @brief Pop one inbound IBI (alias of ::ra8_i3c_ibi_read, native mode).
 * @param[out] ibi Descriptor populated on success.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                One IBI dequeued.
 * @retval k_ra8_err_no_data       IBI queue empty.
 * @retval k_ra8_err_null_ptr      @p ibi NULL.
 * @retval k_ra8_err_invalid_state I3C0 is not in native mode.
 * @pre I3C0 initialized in native mode.
 * @pre @p ibi non-NULL.
 * @post On success @p ibi holds the descriptor.
 * @post NTST.IBIQEFF cleared on success.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_ibi_drain(ra8_i3c_ibi_t* ibi);

/**
 * @brief Drain one inbound IBI from NIBIQP (native mode).
 * @param[out] out_ibi Descriptor populated on success.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                One IBI dequeued.
 * @retval k_ra8_err_no_data       IBI queue empty.
 * @retval k_ra8_err_null_ptr      @p out_ibi NULL.
 * @retval k_ra8_err_invalid_state I3C0 is not in native mode.
 * @pre I3C0 initialized in native mode.
 * @pre @p out_ibi non-NULL.
 * @post On success @p out_ibi holds the descriptor.
 * @post NTST.IBIQEFF cleared on success.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_ibi_read(ra8_i3c_ibi_t* out_ibi);

/**
 * @brief Enter peripheral (target) mode (native mode).
 * @param[in] static_addr Static (pre-DAA) 7-bit address.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Peripheral mode entered.
 * @retval k_ra8_err_invalid_arg   @p static_addr out of range.
 * @retval k_ra8_err_invalid_state I3C0 is not in native mode.
 * @pre I3C0 initialized in native mode.
 * @pre Caller is single-threaded init context.
 * @post BCTL.SLVE set; BCTL.BUSE clear.
 * @post NSDVAD reflects @p static_addr with the valid bit set.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_target_open(uint8_t static_addr);

/* =========================================================================
 * I2C-compatibility peripheral (responder) mode
 * =========================================================================
 */

/**
 * @struct ra8_i3c_peripheral_cfg_t
 * @brief Bring-up configuration for I2C-compat responder mode.
 *
 * @details Used by ::ra8_i3c_peripheral_open to put a channel (configured in
 * I2C mode) into responder operation answering its own 7-bit address.
 *
 * @invariant @p peripheral_addr_7b is a valid 7-bit address.
 * @see ra8_i3c_peripheral_open
 * @since 0.1.0
 */
typedef struct {
  uint8_t peripheral_addr_7b; /**< 7-bit own address.                       */
  uint8_t general_call;       /**< Non-zero -> answer general-call address. */
} ra8_i3c_peripheral_cfg_t;

/**
 * @enum ra8_i3c_peripheral_status_t
 * @brief Latched responder-mode event bits from ::ra8_i3c_peripheral_status.
 *
 * @invariant ::k_ra8_i3c_peripheral_status_idle is the all-clear sentinel (0).
 * @see ra8_i3c_peripheral_status
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_i3c_peripheral_status_idle     = 0x00U, /**< No latched event.      */
  k_ra8_i3c_peripheral_status_aas      = 0x01U, /**< Address matched.       */
  k_ra8_i3c_peripheral_status_rx_full  = 0x02U, /**< Receive buffer full.   */
  k_ra8_i3c_peripheral_status_tx_empty = 0x04U, /**< Transmit buffer empty. */
  k_ra8_i3c_peripheral_status_stop     = 0x08U, /**< STOP detected.         */
  k_ra8_i3c_peripheral_status_nack     = 0x10U, /**< NACK detected.         */
} ra8_i3c_peripheral_status_t;

/**
 * @brief Open a channel in I2C-compat responder mode.
 * @param[in] channel I3C channel index.
 * @param[in] cfg     Responder configuration; non-NULL.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Responder mode entered.
 * @retval k_ra8_err_null_ptr      @p cfg NULL.
 * @retval k_ra8_err_invalid_arg   @p channel or address out of range.
 * @retval k_ra8_err_invalid_state Channel is not in I2C mode.
 * @pre Channel initialized in I2C mode.
 * @pre @p cfg non-NULL.
 * @post The channel answers @p cfg->peripheral_addr_7b.
 * @post No transfer is in progress.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_peripheral_open(uint8_t                         channel,
                                                const ra8_i3c_peripheral_cfg_t* cfg);

/**
 * @brief Close responder mode on a channel.
 * @param[in] channel I3C channel index.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Responder mode closed.
 * @retval k_ra8_err_invalid_arg   @p channel out of range.
 * @retval k_ra8_err_invalid_state Channel is not in I2C mode.
 * @pre Channel was opened in responder mode.
 * @pre Caller is single-threaded.
 * @post The channel no longer answers as a responder.
 * @post The bus is released.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_peripheral_close(uint8_t channel);

/**
 * @brief Queue bytes for the controller to read (responder mode).
 * @param[in] channel I3C channel index.
 * @param[in] data    Bytes to send; non-NULL when @p len > 0.
 * @param[in] len     Byte count.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Bytes queued.
 * @retval k_ra8_err_null_ptr      @p data NULL with @p len > 0.
 * @retval k_ra8_err_invalid_state Channel is not in I2C mode.
 * @pre Channel opened in responder mode.
 * @pre @p data valid for @p len.
 * @post The bytes are staged for the next controller read.
 * @post No controller transfer is corrupted.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_peripheral_send(uint8_t channel, const uint8_t* data, uint32_t len);

/**
 * @brief Read bytes the controller wrote (responder mode).
 * @param[in]  channel I3C channel index.
 * @param[out] buf     Destination buffer; non-NULL.
 * @param[in]  len     Byte count (> 0).
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Bytes read.
 * @retval k_ra8_err_null_ptr      @p buf NULL.
 * @retval k_ra8_err_invalid_state Channel is not in I2C mode.
 * @pre Channel opened in responder mode.
 * @pre @p buf valid for @p len.
 * @post @p buf holds the received bytes.
 * @post The receive buffer is drained.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_peripheral_receive(uint8_t channel, uint8_t* buf, uint32_t len);

/**
 * @brief Read the latched responder-mode status (responder mode).
 * @param[in]  channel  I3C channel index.
 * @param[out] out_mask OR of ::ra8_i3c_peripheral_status_t bits; non-NULL.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Status returned.
 * @retval k_ra8_err_null_ptr      @p out_mask NULL.
 * @retval k_ra8_err_invalid_state Channel is not in I2C mode.
 * @pre Channel opened in responder mode.
 * @pre @p out_mask non-NULL.
 * @post @p out_mask holds the latched event bits.
 * @post No peripheral state is mutated.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_peripheral_status(uint8_t channel, uint8_t* out_mask);

#ifdef __cplusplus
}
#endif
