/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_sdfont.c
 * @brief SD-card font store with self-provisioning -- implementation.
 *
 * @par Tag
 * [Ring 4 / SDFont] {World: NS}
 *
 * @details
 * Implementation of the helper declared in `ra8_sdfont.h`. The flow is a thin
 * orchestration above three lower-ring drivers and carries no register access
 * of its own:
 *
 *   1. Route the four Pmod SPI pins (`ra8_pfs_route_peripheral` + a GPIO CS) and
 *      bring up the SCI channel in Simple-SPI mode (`ra8_sci_spi`).
 *   2. Run the SD identification sequence and bind a FAT backend
 *      (`ra8_sdmmc_spi`), then mount it (`ra8_fs`).
 *   3. Open the font for reading; on `k_ra8_err_not_found` with a provisioning
 *      blob configured, write the blob (`ra8_fs_write_file`) and re-open -- so
 *      the bytes returned are always the on-card copy.
 *
 * The `ra8_sdmmc_spi` transport callbacks are owned here and read their bus
 * context from a single file-scope ::s_ctx populated at the top of
 * ::ra8_sdfont_load (single-shot, init-context helper). Error propagation uses
 * the project's dominant light `if (err != k_ra8_ok)` idiom rather than the
 * logging `RA8_RETURN_ON_ERROR`: each lower call already logs at its own tag.
 */

#include "ra8_sdfont.h"

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_gpio_constants.h"
#include "ra8_port_utils.h"
#include "ra8_sci_spi.h"
#include "ra8_sdmmc_spi.h"
#include "ra8_spi.h"

/**
 * @var s_tag
 * @brief Log tag for diagnostics emitted by this module.
 * @note File-scope, read-only after init.
 * @since 0.1.0
 */
static const char* s_tag = "SDFONT";

/**
 * @var s_default_name
 * @brief Default 8.3 font filename used when the caller passes NULL.
 * @note File-scope, read-only.
 * @since 0.1.0
 */
static const char* s_default_name = "FONT.OTF";

/**
 * @enum sdfont_limits_t
 * @brief Sanity floors for a loaded font.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_sdfont_min_bytes = 16U, /**< Smallest plausible TTF/OTF header -- guards a truncated read. */
} sdfont_limits_t;

/**
 * @struct sdfont_ctx_t
 * @brief Bus context handed to the `ra8_sdmmc_spi` transport callbacks.
 * @details Populated once per ::ra8_sdfont_load call; not reentrant.
 * @since 0.1.0
 */
typedef struct {
  uint8_t        channel;  /**< SCI Simple-SPI channel.       */
  uint32_t       pclka_hz; /**< PCLKA rate for the baud shim. */
  ra8_port_pin_t cs;       /**< Chip-select GPIO pin.         */
} sdfont_ctx_t;

/**
 * @var s_ctx
 * @brief Single-shot transport context for the SD bus shim.
 * @warning Mutated at the top of ::ra8_sdfont_load; not thread-safe.
 * @since 0.1.0
 */
static sdfont_ctx_t s_ctx;

/* ---------------------------------------------------------------------------
 * ra8_sdmmc_spi transport shim (Dependency Inversion seam) over ra8_sci_spi.
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Transport set-clock callback: retune the SCI baud divider.
 * @details Thin DI shim bound into the `ra8_sdmmc_spi` transport: recovers the
 *          SCI channel + PCLKA from the bus context and retunes the baud divider.
 * @param[in] ctx Bus context (::sdfont_ctx_t*); must be non-NULL.
 * @param[in] hz  Target SPI clock in Hz.
 * @return ra8_err_t passthrough from `ra8_sci_spi_set_clock`.
 * @retval k_ra8_err_null_ptr @p ctx is NULL.
 * @pre @p ctx points to the live ::s_ctx.
 * @pre `ra8_sci_spi_init` has configured the SCI channel.
 * @post The SCI channel clock is retuned to @p hz on success.
 * @post No bus transaction is issued (clock configuration only).
 * @note ISR-unsafe; blocking. `ctx` is non-const to match the transport
 *       function-pointer type (constParameterCallback suppressed in-tree).
 * @since 0.1.0
 */
static ra8_err_t sdfont_set_clock(void* ctx, uint32_t hz)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx");
  const sdfont_ctx_t* c = (const sdfont_ctx_t*)ctx;
  return ra8_sci_spi_set_clock(c->channel, hz, c->pclka_hz);
}

/**
 * @brief Transport chip-select callback: drive CS low (asserted) or high.
 * @details Thin DI shim bound into the `ra8_sdmmc_spi` transport: drives the
 *          chip-select GPIO from the bus context (active-low select).
 * @param[in] ctx      Bus context (::sdfont_ctx_t*); must be non-NULL.
 * @param[in] asserted true to select (CS low), false to release (CS high).
 * @return ra8_err_t passthrough from `ra8_gpio_write`.
 * @retval k_ra8_err_null_ptr @p ctx is NULL.
 * @pre @p ctx points to the live ::s_ctx.
 * @pre The CS pin was claimed as a GPIO output by ::sdfont_bringup_spi.
 * @post The CS pin reflects @p asserted on success.
 * @post No other pin or bus state changes.
 * @note ISR-unsafe. `ctx` is non-const to match the transport function-pointer
 *       type (constParameterCallback suppressed in-tree).
 * @since 0.1.0
 */
static ra8_err_t sdfont_cs(void* ctx, bool asserted)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx");
  const sdfont_ctx_t* c = (const sdfont_ctx_t*)ctx;
  return ra8_gpio_write(c->cs, asserted ? k_ra8_level_low : k_ra8_level_high);
}

/**
 * @brief Transport transfer callback: full-duplex byte exchange.
 * @details Thin DI shim bound into the `ra8_sdmmc_spi` transport: forwards a
 *          full-duplex byte exchange to the SCI channel from the bus context.
 * @param[in]  ctx Bus context (::sdfont_ctx_t*); must be non-NULL.
 * @param[in]  tx  TX bytes, or NULL to shift idle 0xFF.
 * @param[out] rx  RX buffer, or NULL to discard.
 * @param[in]  len Byte count; must be > 0.
 * @return ra8_err_t passthrough from `ra8_sci_spi_xfer`.
 * @retval k_ra8_err_null_ptr @p ctx is NULL.
 * @pre @p ctx points to the live ::s_ctx.
 * @pre `ra8_sci_spi_init` has configured the channel and CS is asserted.
 * @post @p len bytes are clocked on the bus on success.
 * @post @p rx holds the received bytes when non-NULL.
 * @note ISR-unsafe; blocking. `ctx` is non-const to match the transport
 *       function-pointer type (constParameterCallback suppressed in-tree).
 * @since 0.1.0
 */
static ra8_err_t sdfont_xfer(void* ctx, const uint8_t* tx, uint8_t* rx, uint32_t len)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx");
  const sdfont_ctx_t* c = (const sdfont_ctx_t*)ctx;
  return ra8_sci_spi_xfer(c->channel, tx, rx, len);
}

/* ---------------------------------------------------------------------------
 * Bring-up helpers
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Route the Pmod SPI pins to SCI Simple-SPI and init the channel.
 * @details Caches the bus context into ::s_ctx for the transport shim, muxes
 *          SCK/CIPO/COPI to SCI async and claims CS as a GPIO output held high,
 *          then initialises the SCI channel at the SD power-on clock.
 * @param[in] cfg Non-NULL bus descriptor.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Pins routed, CS claimed, SCI channel up.
 * @retval k_ra8_err_null_ptr @p cfg is NULL.
 * @retval other             Propagated from `ra8_pfs_route_peripheral` / `ra8_sci_spi_init`.
 * @pre `ra8_cgc_init` has run; @p cfg->pclka_hz is the live PCLKA rate.
 * @pre The Pmod2 pins are free (not routed to another peripheral).
 * @post On success the four pins are muxed and the SCI channel is configured.
 * @post ::s_ctx holds the channel / PCLKA / CS for the transport callbacks.
 * @note CS is a plain GPIO output (SD SPI-mode holds CS across a frame).
 * @since 0.1.0
 */
static ra8_err_t sdfont_bringup_spi(const ra8_sdfont_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg");

  s_ctx.channel  = cfg->spi_channel;
  s_ctx.pclka_hz = cfg->pclka_hz;
  s_ctx.cs       = cfg->cs;

  ra8_err_t err = ra8_pfs_route_peripheral(cfg->sck, k_ra8_psel_sci_async, "sdfont.sck");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral(cfg->cipo, k_ra8_psel_sci_async, "sdfont.cipo");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral(cfg->copi, k_ra8_psel_sci_async, "sdfont.copi");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_gpio_output_init(cfg->cs, k_ra8_level_high);
  if (err != k_ra8_ok) {
    return err;
  }
  const ra8_sci_spi_cfg_t spi_cfg = {
    .baud_hz   = (uint32_t)k_ra8_sdmmc_spi_clock_init_hz,
    .pclk_hz   = cfg->pclka_hz,
    .mode      = k_ra8_spi_mode_0,
    .lsb_first = false,
  };
  return ra8_sci_spi_init(cfg->spi_channel, &spi_cfg);
}

/**
 * @brief Run SD identification and mount the FAT volume.
 * @details Builds the `ra8_sdmmc_spi` transport over the module shims (bound to
 *          ::s_ctx), runs the SPI-mode card identification sequence, binds a FAT
 *          backend, and mounts it through `ra8_fs`.
 * @param[out] out_mount Non-NULL; receives the mounted volume handle.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Card identified and FAT volume mounted.
 * @retval k_ra8_err_null_ptr @p out_mount is NULL.
 * @retval other             Propagated from `ra8_sdmmc_spi_*` / `ra8_fs_mount`.
 * @pre ::sdfont_bringup_spi returned k_ra8_ok and ::s_ctx is populated.
 * @pre A FAT-formatted card is seated in the Pmod2 slot.
 * @post On success @p *out_mount is a usable FAT mount on the SD card.
 * @post On failure no FAT volume is left mounted.
 * @note Blocking, polled SD bring-up.
 * @since 0.1.0
 */
static ra8_err_t sdfont_mount(ra8_fs_mount_t** out_mount)
{
  RA8_CHECK_NULL_PTR(out_mount, s_tag, "out_mount");

  const ra8_sdmmc_spi_transport_t transport = {
    .set_clock = sdfont_set_clock,
    .cs        = sdfont_cs,
    .xfer      = sdfont_xfer,
    .ctx       = &s_ctx,
  };
  ra8_err_t err = ra8_sdmmc_spi_init(&transport);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_fs_backend_t backend = {};
  err                      = ra8_sdmmc_spi_bind_fs_backend(&backend);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_fs_mount(&backend, out_mount);
}

/**
 * @brief Open the font for reading, provisioning from the blob if absent.
 * @details Opens @p name read-only; if that returns `k_ra8_err_not_found` and a
 *          non-empty provisioning blob is configured, writes the blob to the
 *          card and re-opens it -- so the returned handle is always the on-card
 *          copy. The provisioning conditions are separate single-condition
 *          guards (no compound decision) so each branch is independently covered.
 * @param[in]  cfg        Non-NULL bus + provisioning descriptor.
 * @param[in]  mount      Non-NULL mounted FAT volume.
 * @param[in]  name       Non-NULL 8.3 filename.
 * @param[out] out_file   Non-NULL; receives the opened read handle.
 * @param[out] out_source Non-NULL (caller-guaranteed); set to card vs provisioned.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok            File open for reading.
 * @retval k_ra8_err_null_ptr  @p cfg, @p mount, or @p out_file is NULL.
 * @retval k_ra8_err_not_found File absent and provisioning disabled.
 * @retval other              Propagated from `ra8_fs_open` / `ra8_fs_write_file`.
 * @pre @p mount is a live FAT volume.
 * @pre @p out_source points to caller-owned storage.
 * @post On success @p *out_file is readable and @p *out_source is set.
 * @post On a provisioning write the blob exists on the card as @p name.
 * @note Provisioning writes the blob only on a `k_ra8_err_not_found` open.
 * @since 0.1.0
 */
static ra8_err_t sdfont_open_or_provision(const ra8_sdfont_cfg_t* cfg,
                                          ra8_fs_mount_t*         mount,
                                          const char*             name,
                                          ra8_fs_file_t**         out_file,
                                          ra8_sdfont_source_t*    out_source)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg");
  RA8_CHECK_NULL_PTR(mount, s_tag, "mount");
  RA8_CHECK_NULL_PTR(out_file, s_tag, "out_file");

  *out_source   = k_ra8_sdfont_source_card;
  ra8_err_t err = ra8_fs_open(mount, name, k_ra8_fs_mode_read, out_file);

  /* Self-provision only when the file is genuinely absent and a blob is
   * configured. Kept as separate single-condition guards (not one compound
   * decision) so each branch is independently covered without MC/DC vectors. */
  if (err != k_ra8_err_not_found) {
    return err;
  }
  if (cfg->provision_blob == nullptr) {
    return err; /* provisioning disabled -- propagate k_ra8_err_not_found */
  }
  if (cfg->provision_len == 0U) {
    return err; /* nothing to write -- propagate k_ra8_err_not_found */
  }
  err = ra8_fs_write_file(mount, name, cfg->provision_blob, cfg->provision_len);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_source = k_ra8_sdfont_source_provisioned;
  return ra8_fs_open(mount, name, k_ra8_fs_mode_read, out_file);
}

/**
 * @brief Read an open font into the caller buffer and validate its length.
 * @details Reads up to @p cap bytes, closes the handle on every path, and
 *          rejects a truncated read (shorter than a usable font header) so the
 *          caller never hands a stub blob to `ra8_reflow_init`.
 * @param[in]  file    Non-NULL open read handle (closed before return).
 * @param[out] buf     Non-NULL destination buffer.
 * @param[in]  cap     Capacity of @p buf in bytes.
 * @param[out] out_len Non-NULL; receives the bytes read.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Font read; @p *out_len in `[16, cap]`.
 * @retval k_ra8_err_null_ptr A required pointer is NULL.
 * @retval k_ra8_err_no_data  Read fewer than a usable font header.
 * @retval other             Propagated from `ra8_fs_read`.
 * @pre @p file is open for reading.
 * @pre @p buf has room for @p cap bytes.
 * @post @p file is closed on every return path.
 * @post On k_ra8_ok @p *out_len holds the byte count read.
 * @note Blocking read.
 * @since 0.1.0
 */
static ra8_err_t
sdfont_read_font(ra8_fs_file_t* file, uint8_t* buf, uint32_t cap, uint32_t* out_len)
{
  RA8_CHECK_NULL_PTR(file, s_tag, "file");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf");
  RA8_CHECK_NULL_PTR(out_len, s_tag, "out_len");

  uint32_t        got = 0U;
  const ra8_err_t err = ra8_fs_read(file, buf, cap, &got);
  (void)ra8_fs_close(file);
  if (err != k_ra8_ok) {
    return err;
  }
  if (got < (uint32_t)k_sdfont_min_bytes) {
    return k_ra8_err_no_data;
  }
  *out_len = got;
  return k_ra8_ok;
}

/**
 * @brief Validate the public load arguments (precondition gate).
 * @details Centralises the null-pointer and capacity checks for
 *          ::ra8_sdfont_load so the orchestrator stays within the per-function
 *          statement budget while keeping the contract explicit.
 * @param[in] cfg     Bus + provisioning descriptor.
 * @param[in] buf     Destination buffer.
 * @param[in] out_len Length output.
 * @param[in] cap     Buffer capacity in bytes.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              All arguments valid.
 * @retval k_ra8_err_null_ptr    @p cfg, @p buf, or @p out_len is NULL.
 * @retval k_ra8_err_invalid_arg @p cap is 0.
 * @pre Called first from ::ra8_sdfont_load, before any bus access.
 * @pre @p cfg / @p buf / @p out_len are the caller's load arguments.
 * @post On k_ra8_ok every pointer is non-NULL and @p cap > 0.
 * @post No side effects on any path (pure gate).
 * @note Reentrant and stateless; safe to call from any context.
 * @since 0.1.0
 */
static ra8_err_t sdfont_validate(const ra8_sdfont_cfg_t* cfg,
                                 const uint8_t*          buf,
                                 const uint32_t*         out_len,
                                 uint32_t                cap)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf");
  RA8_CHECK_NULL_PTR(out_len, s_tag, "out_len");
  if (cap == 0U) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_sdfont_load(const ra8_sdfont_cfg_t* cfg,
                          uint8_t*                buf,
                          uint32_t                cap,
                          uint32_t*               out_len,
                          ra8_sdfont_source_t*    out_source)
{
  ra8_err_t err = sdfont_validate(cfg, buf, out_len, cap);
  if (err != k_ra8_ok) {
    return err;
  }

  err = sdfont_bringup_spi(cfg);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_fs_mount_t* mount = nullptr;
  err                   = sdfont_mount(&mount);
  if (err != k_ra8_ok) {
    return err;
  }

  const char*         name = (cfg->filename != nullptr) ? cfg->filename : s_default_name;
  ra8_fs_file_t*      file = nullptr;
  ra8_sdfont_source_t src  = k_ra8_sdfont_source_card;
  err                      = sdfont_open_or_provision(cfg, mount, name, &file, &src);
  if (err == k_ra8_ok) {
    err = sdfont_read_font(file, buf, cap, out_len);
  }
  (void)ra8_fs_unmount(mount);
  if (err != k_ra8_ok) {
    return err;
  }

  if (out_source != nullptr) {
    *out_source = src;
  }
  return k_ra8_ok;
}
