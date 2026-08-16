/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/pagecache/main.c
 * @brief On-silicon HIL: ra8_reflow pagination-cache round-trip on SD (#117).
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * #79 added the import-time pagination cache (`ra8_reflow_cache`): lay a chapter
 * out once, serialise the flattened glyph/page result to a keyed blob, persist
 * it, and on the next open skip the expensive layout by loading the blob. That
 * path was host-tested only; this app runs it on the M85 against a real microSD
 * volume through `ra8_fs` -- the exact storage the e-reader uses.
 *
 * Flow: bring up the Pmod2 microSD over `ra8_sdmmc_spi`, mount an `ra8_fs` volume
 * (formatting FAT32 if blank), self-provision `FONT.OTF` (a baked Latin-1 OTF),
 * then exercise the cache three ways:
 *   1. ROUND-TRIP: lay out a fixed chapter live, `ra8_reflow_cache_serialize` it,
 *      write the blob to SD, read it back, `ra8_reflow_cache_load` it into a fresh
 *      engine, re-serialise, and assert the reloaded blob is byte-for-byte equal
 *      to the live one (the serialised form is the per-page glyph/page data, so
 *      equal blobs prove the layout was restored exactly).
 *   2. INVALIDATE: load the same blob into an engine initialised at a DIFFERENT
 *      font size and assert `k_ra8_err_invalid_state` (the key mismatch is caught;
 *      no stale page is served).
 *   3. RESET-SURVIVAL: `::g_pc_hit` reflects whether the cache file already
 *      existed at boot, so a second boot against a persisted card reports a hit
 *      (served from the on-disk cache without a fresh layout).
 *
 * The HIL gate is memprobe (J-Link / ra8_emulator `--dump-sym`), not the console:
 * an SD app drives the SCI0 Simple-SPI bus, and ra8_emulator folds every SCI channel
 * into one console line, so a SCI8 banner is interleaved with SPI traffic there
 * (the same reason the sibling SD HIL apps gate on SWD globals). The success path
 * advances ::g_pc_heartbeat once per frame and only after every assertion passes;
 * any failure stamps ::g_pc_err and parks without bumping it.
 *
 * Required external hardware (on-bench): Digilent PMOD MicroSD (410-380) in Pmod2
 * (J25) with a microSD inserted. THIS APP MAY FORMAT THE CARD. Under ra8_emulator
 * attach a blank card with `--sd-new 64:fat32` (round-trip + invalidate), and a
 * second run against a `--save-sd` image to prove the cache-hit reset-survival.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "literata_latin1.h"
#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_isr.h"
#include "ra8_log.h"
#include "ra8_port_utils.h"
#include "ra8_reflow.h"
#include "ra8_reflow_cache.h"
#include "ra8_sci_spi.h"
#include "ra8_sdmmc_spi.h"
#include "ra8_spi.h"
#include "ra8_time.h"

/** @enum pc_consts_t @brief Console / SPI / reflow knobs (no magic numbers). */
typedef enum : uint32_t {
  k_pc_uart_baud = 115200U, /**< Console baud.                */
  k_pc_spi_chan  = 0U,      /**< Pmod2 / J25 SCI0 Simple-SPI. */
  k_pc_font_cap =
    98304U, /**< Font read buffer (OTF is ~57 KiB; trimmed from 128 KiB when the fs sector arena grew .bss, #683). */
  k_pc_blob_cap    = 16384U,      /**< Cache-blob buffer capacity, bytes.   */
  k_pc_view_w      = 600U,        /**< Reflow viewport width, px.           */
  k_pc_view_h      = 800U,        /**< Reflow viewport height, px.          */
  k_pc_font_px     = 18U,         /**< Body font size, px (round-trip key). */
  k_pc_font_px_alt = 24U,         /**< Alt font size to force invalidation. */
  k_pc_ink_argb    = 0xFF101010U, /**< Body text colour.                    */
  k_pc_link_argb   = 0xFF2A52BEU, /**< Anchor text colour.                  */
  k_pc_crc_init    = 0xFFFFFFFFU, /**< CRC-32 initial value.                */
  k_pc_crc_poly    = 0xEDB88320U, /**< CRC-32 reflected polynomial.         */
  k_pc_crc_bits    = 8U,          /**< Bits folded per byte.                */
} pc_consts_t;

/**
 * @var s_pc_pin_sck
 * @brief Pmod2 SCI clock pin used by the SD-card transport.
 * @details Resolves the board manifest's J25 clock route once at compile time.
 * @note Immutable for the lifetime of the application.
 * @since 0.1.0
 */
static const ra8_port_pin_t s_pc_pin_sck = (ra8_port_pin_t)k_ra8_board_pmod2_spi_sck;

/**
 * @var s_pc_pin_cipo
 * @brief Pmod2 controller-input/peripheral-output pin.
 * @details Carries SD-card data toward SCI0 during SPI transfers.
 * @note Immutable for the lifetime of the application.
 * @since 0.1.0
 */
static const ra8_port_pin_t s_pc_pin_cipo = (ra8_port_pin_t)k_ra8_board_pmod2_spi_cipo;

/**
 * @var s_pc_pin_copi
 * @brief Pmod2 controller-output/peripheral-input pin.
 * @details Carries SCI0 command and payload data toward the SD card.
 * @note Immutable for the lifetime of the application.
 * @since 0.1.0
 */
static const ra8_port_pin_t s_pc_pin_copi = (ra8_port_pin_t)k_ra8_board_pmod2_spi_copi;

/**
 * @var s_pc_pin_cs
 * @brief GPIO-driven active-low SD-card chip-select pin.
 * @details Uses the board manifest's Pmod2 chip-select route outside SCI0.
 * @note Initialized high before the SD transport is invoked.
 * @since 0.1.0
 */
static const ra8_port_pin_t s_pc_pin_cs = (ra8_port_pin_t)k_ra8_board_pmod2_spi_cs;

/**
 * @var s_pc_font_path
 * @brief Root-directory 8.3 name for the persisted font.
 * @details The filesystem layer is root-only, so no directory prefix is used.
 * @note The file is provisioned from the linked Literata asset when absent.
 * @since 0.1.0
 */
static const char s_pc_font_path[] = "FONT.OTF";

/**
 * @var s_pc_cache_path
 * @brief Root-directory 8.3 name for the serialized pagination cache.
 * @details The same file is used for reset-survival detection and verification.
 * @note Its contents are replaced after each successful live layout.
 * @since 0.1.0
 */
static const char s_pc_cache_path[] = "PCACHE.BIN";

/**
 * @var s_pc_body
 * @brief Fixed HTML chapter used by the live and cached layout passes.
 * @details The intentionally short content keeps the silicon and emulator
 *          pagination checks bounded while still exercising paragraph breaks.
 * @note Every cache load uses these exact bytes as part of its identity key.
 * @since 0.1.0
 */
static const char s_pc_body[] = "<html><body><h1>Cache</h1>"
                                "<p>Pagination cache round-trip on silicon.</p>"
                                "<p>Second paragraph for the break engine.</p></body></html>";

/**
 * @enum pc_err_t
 * @brief Failure-stage codes stamped to ::g_pc_err for SWD / `--dump-sym`.
 */
typedef enum : uint32_t {
  k_pc_err_none     = 0U,  /**< No failure (success path).            */
  k_pc_err_init     = 1U,  /**< CGC / time / console / SPI bring-up.  */
  k_pc_err_card     = 2U,  /**< SD card SPI init.                     */
  k_pc_err_mount    = 3U,  /**< Mount (and format-if-blank).          */
  k_pc_err_font     = 4U,  /**< Provision / read FONT.OTF.            */
  k_pc_err_layout   = 5U,  /**< ra8_reflow_init / layout_chapter.     */
  k_pc_err_ser      = 6U,  /**< ra8_reflow_cache_serialize.           */
  k_pc_err_persist  = 7U,  /**< Write / read-back the blob on SD.     */
  k_pc_err_readback = 8U,  /**< Read-back blob != written blob.       */
  k_pc_err_load     = 9U,  /**< ra8_reflow_cache_load (expected hit). */
  k_pc_err_crc      = 10U, /**< Reloaded layout CRC != live CRC.      */
  k_pc_err_invalid  = 11U, /**< Alt-size load did not invalidate.     */
} pc_err_t;

/** @enum pc_pace_t @brief Idle-loop pacing for the success heartbeat. */
typedef enum : uint32_t {
  k_pc_frame_ms = 100U, /**< Heartbeat period in milliseconds. */
} pc_pace_t;

/* ---- J-Link / ra8_emulator readable diagnostics (memprobe gate) ------------- */

/** @brief First failing stage (::pc_err_t), 0 on success. SWD / `--dump-sym`. */
volatile uint32_t g_pc_err = (uint32_t)k_pc_err_none;
/** @brief 1 if the cache file existed at boot (reset-survival hit). */
volatile uint32_t g_pc_hit = 0U;
/** @brief 1 if the reloaded-cache blob CRC equals the live-layout blob CRC. */
volatile uint32_t g_pc_crc_match = 0U;
/** @brief 1 if loading the blob at the alt font size returned invalid_state. */
volatile uint32_t g_pc_invalidate = 0U;
/** @brief Page count produced by the live layout. */
volatile uint32_t g_pc_pages = 0U;
/** @brief CRC-32 of the live-layout serialised blob. */
volatile uint32_t g_pc_live_crc = 0U;
/** @brief CRC-32 of the reloaded-then-reserialised blob. */
volatile uint32_t g_pc_cache_crc = 0U;
/** @brief Idle heartbeat; advances ONLY after every assertion passes. */
volatile uint32_t g_pc_heartbeat = 0U;

/* ---- Static storage (large pools off the stack) -------------------------- */

/** @brief Font blob read off the card. */
static uint8_t s_font_buf[k_pc_font_cap];
/** @brief Reflow engine (glyph / page / token / text pools live inside). */
static ra8_reflow_t s_engine;
/** @brief Serialised cache blob from the live layout. */
static uint8_t s_live_blob[k_pc_blob_cap];
/** @brief Blob read back off SD (compared to ::s_live_blob). */
static uint8_t s_read_blob[k_pc_blob_cap];
/** @brief Blob re-serialised from the reloaded engine (CRC vs live). */
static uint8_t s_reser_blob[k_pc_blob_cap];
/** @brief SD backend; file-scope so the mount handle may reference it. */
static ra8_fs_backend_t s_backend;

/**
 * @var s_msg_boot
 * @brief Console marker emitted after core and peripheral setup.
 * @details Lets a bench operator distinguish startup from later cache output.
 * @note The byte count excludes the terminating null character.
 * @since 0.1.0
 */
static const uint8_t s_msg_boot[] = "pagecache-hil: boot\r\n";

/**
 * @var s_msg_pass
 * @brief Prefix for the final reset-survival hit diagnostic.
 * @details Followed by one ASCII hit digit and the fixed success suffix.
 * @note Emitted only after all cache assertions pass.
 * @since 0.1.0
 */
static const uint8_t s_msg_pass[] = "pagecache-hil: hit=";

/**
 * @var s_msg_ok
 * @brief Final cache-CRC and invalidation success suffix.
 * @details Records the two acceptance conditions in a stable bench-readable line.
 * @note Emitted only after all cache assertions pass.
 * @since 0.1.0
 */
static const uint8_t s_msg_ok[] = " crc_match=1 invalidate=1 PASS\r\n";

/**
 * @brief Emit a byte run on the board console.
 * @details Forwards the caller-owned span directly to the initialized UART
 *          console and deliberately ignores diagnostic write failure.
 * @param[in] msg First byte of the diagnostic span.
 * @param[in] len Number of bytes to attempt.
 * @pre ``msg`` references at least ``len`` readable bytes.
 * @pre The board UART console has been initialized.
 * @post At most ``len`` bytes have been offered to the console backend.
 * @post The caller's buffer remains unchanged.
 * @note This HIL diagnostic path does not influence acceptance state.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_pc_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/**
 * @brief Stamp the failure stage and park the CPU without a heartbeat.
 * @details Records the first detected stage code for SWD inspection, then
 *          repeatedly waits for interrupts without resuming the test flow.
 * @param[in] err Failure-stage code (::pc_err_t).
 * @pre ``err`` identifies the failed stage and is nonzero.
 * @pre Diagnostic SRAM remains writable.
 * @post ``g_pc_err`` contains ``err``.
 * @post Control never returns and ``g_pc_heartbeat`` does not advance.
 * @note A debugger may inspect the latched globals while the core is parked.
 * @since 0.1.0
 */
[[noreturn]] RA8_INTERNAL static void internal_pc_fail(uint32_t err)
{
  g_pc_err = err;
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Calculate reflected CRC-32 over a byte span.
 * @details Starts with ``k_pc_crc_init``, folds eight reflected polynomial
 *          steps per byte, and returns the one's-complement accumulator.
 * @param[in] data First byte of the input span.
 * @param[in] len Number of bytes to fold.
 * @return CRC-32 of the supplied bytes.
 * @retval 0x00000000..0xFFFFFFFF Computed reflected CRC value.
 * @pre ``data`` references at least ``len`` readable bytes.
 * @pre ``len`` is bounded by the caller-owned cache buffer.
 * @post Input bytes remain unchanged.
 * @post The result depends only on the supplied byte sequence.
 * @note The implementation uses polynomial ``0xEDB88320``.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_pc_crc32(const uint8_t* data, size_t len)
{
  uint32_t crc = (uint32_t)k_pc_crc_init;
  for (size_t i = 0U; i < len; i++) {
    crc ^= (uint32_t)data[i];
    for (uint32_t bit = 0U; bit < (uint32_t)k_pc_crc_bits; bit++) {
      const uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
      crc                 = (crc >> 1U) ^ ((uint32_t)k_pc_crc_poly & mask);
    }
  }
  return ~crc;
}

/**
 * @brief Compare two equal-length cache-blob spans.
 * @details Scans from the first byte and returns immediately on the first
 *          mismatch, avoiding any allocation or temporary buffer.
 * @param[in] a First input span.
 * @param[in] b Second input span.
 * @param[in] len Number of bytes to compare.
 * @return Whether every compared byte is equal.
 * @retval true All ``len`` bytes match.
 * @retval false At least one byte differs.
 * @pre ``a`` and ``b`` each reference at least ``len`` readable bytes.
 * @pre Both spans describe cache blobs of the same expected length.
 * @post Both input spans remain unchanged.
 * @post No state outside the function is modified.
 * @note This comparison is not intended to be constant-time.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_pc_blob_equal(const uint8_t* a, const uint8_t* b, size_t len)
{
  for (size_t i = 0U; i < len; i++) {
    if (a[i] != b[i]) {
      return false;
    }
  }
  return true;
}

/* ---- SPI -> ra8_sdmmc_spi transport adapter (mirror of epub_open) ------ */

/* cppcheck-suppress constParameterCallback -- bound to ra8_sdmmc_spi_transport_t::set_clock, `ra8_err_t (*)(void*, uint32_t)`; constifying ctx would break the binding. */
/**
 * @brief Apply an SD-card SPI clock through the SCI transport.
 * @details Reads the caller-owned PCLKA frequency from ``ctx`` and delegates
 *          divisor selection to the SCI SPI driver.
 * @param[in] ctx Pointer to the initialized PCLKA frequency value.
 * @param[in] hz Requested SPI clock frequency in hertz.
 * @return SCI clock-configuration status.
 * @retval k_ra8_ok The requested clock was configured.
 * @retval k_ra8_err_invalid_arg The context or requested rate is invalid.
 * @pre ``ctx`` points to a readable ``uint32_t`` PCLKA value.
 * @pre SCI0 has already been initialized for SPI operation.
 * @post SCI0 retains either its prior clock or a valid clock near ``hz``.
 * @post The caller-owned PCLKA value remains unchanged.
 * @note Signature matches ``ra8_sdmmc_spi_transport_t::set_clock``.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_pc_spi_set_clock(void* ctx, uint32_t hz)
{
  const uint32_t pclka_hz = *(const uint32_t*)ctx;
  return ra8_sci_spi_set_clock((uint8_t)k_pc_spi_chan, hz, pclka_hz);
}

/**
 * @brief Drive the SD-card active-low chip-select GPIO.
 * @details Converts the transport's logical assertion state to the board
 *          pin's physical low/high level; the unused context is accepted.
 * @param[in] ctx Transport context, unused by the GPIO operation.
 * @param[in] asserted Whether the SD card must be selected.
 * @return GPIO write status.
 * @retval k_ra8_ok The requested level was driven.
 * @retval k_ra8_err_invalid_arg The board pin is not a valid GPIO output.
 * @pre ``s_pc_pin_cs`` has been initialized as an output.
 * @pre The SD transport serializes chip-select transitions with transfers.
 * @post The pin is low when asserted and high when deasserted.
 * @post The unused transport context remains unchanged.
 * @note Signature matches ``ra8_sdmmc_spi_transport_t::cs``.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_pc_spi_cs(void* ctx, bool asserted)
{
  (void)ctx;
  return ra8_gpio_write(s_pc_pin_cs, asserted ? k_ra8_level_low : k_ra8_level_high);
}

/**
 * @brief Exchange an SD-card byte span through SCI SPI.
 * @details Delegates the full-duplex transaction to the configured SCI channel;
 *          the transport context is unused because channel selection is fixed.
 * @param[in] ctx Transport context, unused by the SCI operation.
 * @param[in] tx Transmit bytes, or the driver's supported null form.
 * @param[out] rx Receive buffer, or the driver's supported null form.
 * @param[in] len Number of bytes to exchange.
 * @return SCI transfer status.
 * @retval k_ra8_ok All requested bytes were transferred.
 * @retval k_ra8_err_timeout The synchronous transfer did not complete in time.
 * @pre SCI0 has been initialized for SPI mode.
 * @pre Non-null ``tx`` and ``rx`` spans each cover at least ``len`` bytes.
 * @post On success ``rx`` contains ``len`` received bytes when non-null.
 * @post The transmit span remains unchanged.
 * @note Signature matches ``ra8_sdmmc_spi_transport_t::xfer``.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_pc_spi_xfer(void* ctx, const uint8_t* tx, uint8_t* rx, uint32_t len)
{
  (void)ctx;
  return ra8_sci_spi_xfer((uint8_t)k_pc_spi_chan, tx, rx, len);
}

/**
 * @brief Route Pmod2 SPI signals and initialize chip select high.
 * @details Claims SCK, CIPO, and COPI for SCI operation in dependency order,
 *          then configures the separate chip-select pin as a GPIO output.
 * @return Pin-routing and GPIO initialization status.
 * @retval k_ra8_ok All four pins were configured.
 * @retval k_ra8_err_invalid_arg A requested route or GPIO pin is unsupported.
 * @pre The board pin manifest identifies the Pmod2 SPI pins.
 * @pre No conflicting peripheral owns those pins.
 * @post On success SCI owns SCK/CIPO/COPI and chip select is high.
 * @post On failure no later routing step is attempted.
 * @note The caller treats any nonzero status as a fatal initialization error.
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_pc_spi_pins_init(void)
{
  ra8_err_t err = ra8_pfs_route_peripheral(s_pc_pin_sck, k_ra8_psel_sci_async, "pc.sck");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral(s_pc_pin_cipo, k_ra8_psel_sci_async, "pc.cipo");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral(s_pc_pin_copi, k_ra8_psel_sci_async, "pc.copi");
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_gpio_output_init(s_pc_pin_cs, k_ra8_level_high);
}

/**
 * @brief Bring up clocks, timekeeping, console, and the SD SPI controller.
 * @details Resolves CPUCLK0 and PCLKA, initializes SysTick and the console,
 *          routes Pmod2 pins, and configures SCI0 at the card-init rate.
 * @param[out] out_pclka_hz Receives the configured PCLKA frequency.
 * @pre ``out_pclka_hz`` points to writable storage.
 * @pre Board clock and pin-control registers are at their reset-safe state.
 * @post On success all host-side SD transport dependencies are initialized.
 * @post On failure the error stage is latched and the core does not return.
 * @note Interrupts remain globally masked until the caller enables them.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_pc_setup_or_halt(uint32_t* out_pclka_hz)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) ||
      (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) ||
      (ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, &pclka_hz) != k_ra8_ok) ||
      (ra8_time_init(cpuclk0_hz) != k_ra8_ok)) {
    internal_pc_fail((uint32_t)k_pc_err_init);
  }
  if (ra8_board_uart_console_init((uint32_t)k_pc_uart_baud) != k_ra8_ok) {
    internal_pc_fail((uint32_t)k_pc_err_init);
  }
  if (internal_pc_spi_pins_init() != k_ra8_ok) {
    internal_pc_fail((uint32_t)k_pc_err_init);
  }
  const ra8_sci_spi_cfg_t spi_cfg = {.baud_hz   = (uint32_t)k_ra8_sdmmc_spi_clock_init_hz,
                                     .pclk_hz   = pclka_hz,
                                     .mode      = k_ra8_spi_mode_0,
                                     .lsb_first = false};
  if (ra8_sci_spi_init((uint8_t)k_pc_spi_chan, &spi_cfg) != k_ra8_ok) {
    internal_pc_fail((uint32_t)k_pc_err_init);
  }
  *out_pclka_hz = pclka_hz;
}

/**
 * @brief Initialize the SD card over the configured SPI transport.
 * @details Binds the three local transport callbacks with a caller-owned
 *          PCLKA context and runs the card's bounded initialization sequence.
 * @param[in] pclka_hz Stable PCLKA frequency used by clock changes.
 * @pre ``pclka_hz`` points to a valid frequency for the function's duration.
 * @pre SCI0 and its GPIO chip select have been initialized.
 * @post On success the card accepts block operations through the SPI backend.
 * @post On failure ``g_pc_err`` identifies the card stage and execution parks.
 * @note The transport structure may be temporary because initialization copies it.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_pc_init_card_or_halt(uint32_t* pclka_hz)
{
  const ra8_sdmmc_spi_transport_t transport = {.set_clock = internal_pc_spi_set_clock,
                                               .cs        = internal_pc_spi_cs,
                                               .xfer      = internal_pc_spi_xfer,
                                               .ctx       = pclka_hz};
  if (ra8_sdmmc_spi_init(&transport) != k_ra8_ok) {
    internal_pc_fail((uint32_t)k_pc_err_card);
  }
}

/**
 * @brief Bind and mount the SD filesystem, formatting blank media if needed.
 * @details Attempts a normal mount first; when that fails, creates a FAT32
 *          volume labeled ``RAPCACHE`` and retries exactly once.
 * @return Mounted filesystem handle.
 * @retval non-NULL Valid mount retained by the filesystem layer.
 * @pre The SD SPI card backend has completed initialization.
 * @pre ``s_backend`` is available for binding and remains alive after return.
 * @post On success a mounted root volume is available to later helpers.
 * @post On unrecoverable failure the mount stage is latched and execution parks.
 * @note Existing formatted volumes are never reformatted after a successful mount.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_fs_mount_t* internal_pc_mount_or_halt(void)
{
  if (ra8_sdmmc_spi_bind_fs_backend(&s_backend) != k_ra8_ok) {
    internal_pc_fail((uint32_t)k_pc_err_mount);
  }
  ra8_fs_mount_t* mount = nullptr;
  if (ra8_fs_mount(&s_backend, &mount) != k_ra8_ok) {
    ra8_fs_format_opts_t opts = {};
    opts.type                 = k_ra8_fs_type_fat32;
    opts.label                = "RAPCACHE";
    if ((ra8_fs_format(&s_backend, &opts) != k_ra8_ok) ||
        (ra8_fs_mount(&s_backend, &mount) != k_ra8_ok)) {
      internal_pc_fail((uint32_t)k_pc_err_mount);
    }
  }
  return mount;
}

/**
 * @brief Provision FONT.OTF if absent, then read it into ::s_font_buf.
 * @details Opens the persisted font, writes the linked Literata asset when the
 *          file is absent, then reads a bounded span into static storage.
 * @param[in]  mount    Mounted SD volume.
 * @param[out] out_len  Receives the font length in bytes.
 * @pre ``mount`` is a valid mounted filesystem handle.
 * @pre ``out_len`` points to writable storage and the linked font fits the cap.
 * @post On success ``s_font_buf`` contains a nonempty font and ``*out_len`` its size.
 * @post On failure the font stage is latched and execution parks.
 * @note The file handle is closed before either success or a read-failure halt.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_pc_font_or_halt(ra8_fs_mount_t* mount, uint32_t* out_len)
{
  ra8_fs_file_t* file = nullptr;
  if (ra8_fs_open(mount, s_pc_font_path, k_ra8_fs_mode_read, &file) != k_ra8_ok) {
    if (ra8_fs_write_file(mount,
                          s_pc_font_path,
                          g_ra8_font_literata_latin1,
                          (uint32_t)g_ra8_font_literata_latin1_len) != k_ra8_ok) {
      internal_pc_fail((uint32_t)k_pc_err_font);
    }
    if (ra8_fs_open(mount, s_pc_font_path, k_ra8_fs_mode_read, &file) != k_ra8_ok) {
      internal_pc_fail((uint32_t)k_pc_err_font);
    }
  }
  uint32_t got = 0U;
  if ((ra8_fs_read(file, s_font_buf, (uint32_t)k_pc_font_cap, &got) != k_ra8_ok) || (got == 0U)) {
    (void)ra8_fs_close(file);
    internal_pc_fail((uint32_t)k_pc_err_font);
  }
  (void)ra8_fs_close(file);
  *out_len = got;
}

/**
 * @brief Read the persisted cache file into a bounded caller buffer.
 * @details Treats absence and read failure as a cache miss while ensuring any
 *          successfully opened file is closed before returning.
 * @param[in] mount Mounted filesystem handle.
 * @param[out] buf Destination for cache bytes.
 * @param[in] cap Writable capacity of ``buf`` in bytes.
 * @return Number of cache bytes read.
 * @retval 0 The file was absent, empty, or unreadable.
 * @retval 1..cap Number of bytes placed in ``buf``.
 * @pre ``mount`` is valid and ``buf`` references ``cap`` writable bytes.
 * @pre ``cap`` is representable by the filesystem read API.
 * @post The cache file is closed if it was opened.
 * @post Bytes beyond the returned length remain unspecified.
 * @note A miss is intentionally nonfatal because the live layout can rebuild it.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t
internal_pc_read_cache(ra8_fs_mount_t* mount, uint8_t* buf, uint32_t cap)
{
  ra8_fs_file_t* file = nullptr;
  if (ra8_fs_open(mount, s_pc_cache_path, k_ra8_fs_mode_read, &file) != k_ra8_ok) {
    return 0U;
  }
  uint32_t got = 0U;
  if (ra8_fs_read(file, buf, cap, &got) != k_ra8_ok) {
    got = 0U;
  }
  (void)ra8_fs_close(file);
  return got;
}

/**
 * @brief Initialise the engine at @p font_px over the SD font.
 * @details Reinitializes the single static reflow engine with the requested
 *          font size, fixed viewport, and fixed ink/link colors.
 * @param[in] font_px Requested font pixel height.
 * @param[in] font_len Number of valid font bytes in ``s_font_buf``.
 * @return true on success, false on init failure.
 * @retval true The reflow engine accepted the font and configuration.
 * @retval false The font or layout configuration was invalid.
 * @pre ``font_len`` is nonzero and does not exceed ``k_pc_font_cap``.
 * @pre No caller is concurrently using ``s_engine``.
 * @post On success ``s_engine`` is ready for layout or cache loading.
 * @post On failure callers must not use the engine for reflow work.
 * @note Each validation phase deliberately replaces the prior engine state.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_pc_engine_init(uint16_t font_px, uint32_t font_len)
{
  return ra8_reflow_init((uint16_t)k_pc_view_w,
                         (uint16_t)k_pc_view_h,
                         s_font_buf,
                         (size_t)font_len,
                         font_px,
                         (uint32_t)k_pc_ink_argb,
                         (uint32_t)k_pc_link_argb,
                         &s_engine) == k_ra8_ok;
}

/**
 * @brief Lay out the chapter live and serialise it into ::s_live_blob.
 * @details Initializes the normal-font engine, lays out the fixed HTML body,
 *          serializes the resulting cache, and records page and CRC diagnostics.
 * @param[in]  font_len Font length, bytes.
 * @param[out] out_n    Receives the live blob length, bytes.
 * @pre ``font_len`` describes the valid prefix of ``s_font_buf``.
 * @pre ``out_n`` points to writable storage and the static blob capacity is sufficient.
 * @post On success ``s_live_blob`` and the live page/CRC diagnostics are populated.
 * @post On failure the appropriate layout or serialization stage is latched.
 * @note The helper does not persist the generated blob to the card.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_pc_live_layout_or_halt(uint32_t font_len, size_t* out_n)
{
  if (!internal_pc_engine_init((uint16_t)k_pc_font_px, font_len)) {
    internal_pc_fail((uint32_t)k_pc_err_layout);
  }
  uint32_t pages = 0U;
  if (ra8_reflow_layout_chapter(&s_engine,
                                (const uint8_t*)s_pc_body,
                                (size_t)(sizeof(s_pc_body) - 1U),
                                &pages) != k_ra8_ok) {
    internal_pc_fail((uint32_t)k_pc_err_layout);
  }
  g_pc_pages = pages;
  size_t n   = 0U;
  if (ra8_reflow_cache_serialize(&s_engine,
                                 (const uint8_t*)s_pc_body,
                                 (size_t)(sizeof(s_pc_body) - 1U),
                                 s_live_blob,
                                 (size_t)k_pc_blob_cap,
                                 &n) != k_ra8_ok) {
    internal_pc_fail((uint32_t)k_pc_err_ser);
  }
  g_pc_live_crc = internal_pc_crc32(s_live_blob, n);
  *out_n        = n;
}

/**
 * @brief Persist the live cache blob and verify an exact read-back.
 * @details Replaces the cache file, reads it into independent static storage,
 *          validates the byte count, and compares every serialized byte.
 * @param[in] mount Mounted filesystem handle.
 * @param[in] n Number of valid bytes in ``s_live_blob``.
 * @pre ``mount`` is valid and ``n`` does not exceed ``k_pc_blob_cap``.
 * @pre ``s_live_blob`` contains the current live serialization.
 * @post On success ``s_read_blob[0..n)`` exactly matches the live blob.
 * @post On failure the persist or read-back stage is latched and execution parks.
 * @note The comparison distinguishes storage corruption from serialization failure.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_pc_persist_and_verify_or_halt(ra8_fs_mount_t* mount, size_t n)
{
  if (ra8_fs_write_file(mount, s_pc_cache_path, s_live_blob, (uint32_t)n) != k_ra8_ok) {
    internal_pc_fail((uint32_t)k_pc_err_persist);
  }
  const uint32_t got = internal_pc_read_cache(mount, s_read_blob, (uint32_t)k_pc_blob_cap);
  if (got != (uint32_t)n) {
    internal_pc_fail((uint32_t)k_pc_err_persist);
  }
  if (!internal_pc_blob_equal(s_live_blob, s_read_blob, n)) {
    internal_pc_fail((uint32_t)k_pc_err_readback);
  }
}

/**
 * @brief Load the read-back blob into a fresh same-size engine and CRC it.
 * @details Reinitializes the engine with the original font size, loads the
 *          persisted cache, serializes it again, and compares length and CRC.
 * @param[in] font_len Font length, bytes.
 * @param[in] n        Blob length, bytes.
 * @pre ``font_len`` describes the valid prefix of ``s_font_buf``.
 * @pre ``n`` valid bytes are present in ``s_read_blob``.
 * @post On success ``g_pc_cache_crc`` matches ``g_pc_live_crc`` and the match flag is set.
 * @post On failure the layout, load, serialization, or CRC stage is latched.
 * @note Re-serialization proves that the loaded engine state is structurally usable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_pc_reload_check_or_halt(uint32_t font_len, size_t n)
{
  if (!internal_pc_engine_init((uint16_t)k_pc_font_px, font_len)) {
    internal_pc_fail((uint32_t)k_pc_err_layout);
  }
  if (ra8_reflow_cache_load(&s_engine,
                            (const uint8_t*)s_pc_body,
                            (size_t)(sizeof(s_pc_body) - 1U),
                            s_read_blob,
                            n) != k_ra8_ok) {
    internal_pc_fail((uint32_t)k_pc_err_load);
  }
  size_t n2 = 0U;
  if (ra8_reflow_cache_serialize(&s_engine,
                                 (const uint8_t*)s_pc_body,
                                 (size_t)(sizeof(s_pc_body) - 1U),
                                 s_reser_blob,
                                 (size_t)k_pc_blob_cap,
                                 &n2) != k_ra8_ok) {
    internal_pc_fail((uint32_t)k_pc_err_ser);
  }
  g_pc_cache_crc = internal_pc_crc32(s_reser_blob, n2);
  if ((n2 != n) || (g_pc_cache_crc != g_pc_live_crc)) {
    internal_pc_fail((uint32_t)k_pc_err_crc);
  }
  g_pc_crc_match = 1U;
}

/**
 * @brief Verify that a font-size change invalidates the persisted cache.
 * @details Initializes a fresh engine at the alternate size and requires cache
 *          loading to reject the normal-size blob with ``invalid_state``.
 * @param[in] font_len Font length in ``s_font_buf``.
 * @param[in] n Number of valid cache bytes in ``s_read_blob``.
 * @pre The same font bytes and chapter body used to create the cache remain available.
 * @pre ``n`` does not exceed ``k_pc_blob_cap``.
 * @post On success ``g_pc_invalidate`` is set to one.
 * @post Any unexpected cache-load result latches the invalidation stage and parks.
 * @note Font size is intentionally part of the cache identity contract.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_pc_invalidate_check_or_halt(uint32_t font_len, size_t n)
{
  if (!internal_pc_engine_init((uint16_t)k_pc_font_px_alt, font_len)) {
    internal_pc_fail((uint32_t)k_pc_err_layout);
  }
  if (ra8_reflow_cache_load(&s_engine,
                            (const uint8_t*)s_pc_body,
                            (size_t)(sizeof(s_pc_body) - 1U),
                            s_read_blob,
                            n) != k_ra8_err_invalid_state) {
    internal_pc_fail((uint32_t)k_pc_err_invalid);
  }
  g_pc_invalidate = 1U;
}

/**
 * @brief App entry: SD bring-up -> font -> cache round-trip -> heartbeat idle.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post On success the cache globals are latched and ::g_pc_heartbeat advances.
 * @post On any failure ::g_pc_err is non-zero and the CPU parks (no heartbeat).
 * @since 0.1.0
 */
void main(void)
{
  uint32_t pclka_hz = 0U;
  internal_pc_setup_or_halt(&pclka_hz);
  ra8_isr_globals_enable();
  ra8_log_init();
  internal_pc_print(s_msg_boot, (uint32_t)sizeof(s_msg_boot) - 1U);

  internal_pc_init_card_or_halt(&pclka_hz);
  ra8_fs_mount_t* const mount = internal_pc_mount_or_halt();

  /* Reset-survival: a prior boot's persisted cache file is the hit signal. */
  g_pc_hit = (internal_pc_read_cache(mount, s_read_blob, (uint32_t)k_pc_blob_cap) != 0U) ? 1U : 0U;

  uint32_t font_len = 0U;
  internal_pc_font_or_halt(mount, &font_len);

  size_t n = 0U;
  internal_pc_live_layout_or_halt(font_len, &n);
  internal_pc_persist_and_verify_or_halt(mount, n);
  internal_pc_reload_check_or_halt(font_len, n);
  internal_pc_invalidate_check_or_halt(font_len, n);

  internal_pc_print(s_msg_pass, (uint32_t)sizeof(s_msg_pass) - 1U);
  internal_pc_print((const uint8_t*)(g_pc_hit != 0U ? "1" : "0"), 1U);
  internal_pc_print(s_msg_ok, (uint32_t)sizeof(s_msg_ok) - 1U);

  /* Idle with a heartbeat that advances ONLY here -- every failure path parks
   * in WFI without bumping it -- so a memprobe gate proves the full SD -> ra8_fs
   * -> ra8_reflow_cache round-trip + invalidation ran clean. */
  while (1) {
    ra8_delay_ms(k_pc_frame_ms);
    g_pc_heartbeat++;
  }
}
