/**
 * @file main.c
 * @brief Download, validate, publish, and reopen one C6-fetched `.rabook`
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * This hardware-pending composition root joins Wi-Fi through the ESP32-C6,
 * mounts an existing FAT volume on Pmod2 micro-SD, downloads one RBKC
 * `.rabook` body through the C6 media RPC, hashes every accepted byte on the
 * RA8, strictly validates the staged container, publishes it with the VFS
 * no-replace transaction, then reopens and consumes its first inflated chunk.
 * Every large workspace is caller-owned SDRAM; neither first-party heap nor
 * stdio is used.
 *
 * Empty build-time URL or Wi-Fi credentials produce a valid image that refuses
 * before touching storage or starting an RPC. The app stays in `hw_pending`
 * until the exact mixed RA8/C6 image and removable-media path pass physical
 * fault-injection HIL.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_book.h"
#include "ra8_boot_entry.h"
#include "ra8_c6link.h"
#include "ra8_c6link_mdl_transfer.h"
#include "ra8_c6link_wifi.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_esp_hosted_c6link.h"
#include "ra8_esp_hosted_port.h"
#include "ra8_fs.h"
#include "ra8_io_vfs.h"
#include "ra8_isr.h"
#include "ra8_mdl_rabook_vfs.h"
#include "ra8_mdl_storage_vfs.h"
#include "ra8_mstp.h"
#include "ra8_rsip.h"
#include "ra8_sdmmc_spi.h"
#include "ra8_time.h"
#include "tx_api.h"

#ifndef RA8_C6_WIFI_SSID
/** @brief Empty default keeps credentials out of aggregate builds. */
#define RA8_C6_WIFI_SSID ""
#endif
#ifndef RA8_C6_WIFI_PSK
/** @brief Empty default keeps credentials out of aggregate builds. */
#define RA8_C6_WIFI_PSK ""
#endif
#ifndef RA8_MEDIA_DOWNLOAD_URL
/** @brief Empty default makes an unconfigured image fail before any RPC. */
#define RA8_MEDIA_DOWNLOAD_URL ""
#endif

/** @brief Fixed application bounds and board integration policy. */
typedef enum : uint32_t {
  k_media_uart_baud          = 115200U,  /**< J-Link OB console rate.                   */
  k_media_c6_sck_hz          = 5000000U, /**< Bench-proven C6 SPI rate.   */
  k_media_c6_edge_poll_ms    = 2U,       /**< C6 side-band poll period.   */
  k_media_c6_boot_wait_ms    = 200U,     /**< Coprocessor settle time. */
  k_media_assoc_tries        = 200U,     /**< Bounded station-event polls.              */
  k_media_assoc_gap_ms       = 50U,      /**< Delay between association polls.          */
  k_media_heartbeat_ms       = 5000U,    /**< Persistent verdict interval.       */
  k_media_worker_stack_bytes = 8192U,    /**< ThreadX worker stack. */
  k_media_worker_priority    = 8U,       /**< Worker priority and threshold.       */
  k_media_c6_arena_bytes     = 4096U,    /**< Protobuf decode arena.     */
  k_media_transfer_chunk     = 1024U,    /**< Bytes requested per C6 body pull.     */
  k_media_transfer_chunks    = 32768U,   /**< 32 MiB compressed-body ceiling.   */
  k_media_rbkc_chunk_bytes   = 65536U,   /**< Accepted inflated RBKC chunk geometry.    */
  k_media_compressed_bytes   = 66560U,   /**< One worst-case zlib-wrapped 64 KiB chunk. */
  k_media_table_entries      = 2049U,    /**< 2048 chunks + terminal offset. */
  k_media_scratch_bytes      = 65536U,   /**< Strict CRC/ownership validation scratch.  */
} media_limit_t;

/** @brief Compile-time SSID; never populated in source control. */
static const char s_ssid[] = RA8_C6_WIFI_SSID;
/** @brief Compile-time passphrase; never populated in source control. */
static const char s_psk[] = RA8_C6_WIFI_PSK;
/** @brief C6 HTTPS body URL; empty images refuse before transfer. */
static const char s_url[] = RA8_MEDIA_DOWNLOAD_URL;
/** @brief No-replace destination on the mounted SD card. */
static const char s_destination[] = "sd:/BOOKS/C6BOOK.RBK";
/** @brief Reserved sibling transaction name. */
static const char s_stage_leaf[] = "C6STAGE.TMP";

/** @brief Cached CPU clock for the delay service. */
static uint32_t s_cpuclk_hz;
/** @brief Cached PCLKA clock for both SPI transports. */
static uint32_t s_pclka_hz;
/** @brief ESP-hosted port initialization verdict. */
static ra8_err_t s_port_err = k_ra8_err_not_initialized;
/** @brief Worker thread object. */
static TX_THREAD s_worker;
/** @brief Writable ThreadX worker name. */
static CHAR s_worker_name[] = "media_download";
/** @brief Caller-owned ThreadX worker stack. */
static UCHAR s_worker_stack[k_media_worker_stack_bytes];
/** @brief One exclusively owned C6 link. */
static ra8_c6link_t s_link;
/** @brief Fixed C6 decode arena. */
static uint8_t s_c6_arena[k_media_c6_arena_bytes];
/** @brief Station-associated event latch. */
static volatile uint8_t s_connected;
/** @brief Station-disconnected event latch. */
static volatile uint8_t s_disconnected;

/** @brief SD filesystem backend retained for the mount lifetime. */
static ra8_fs_backend_t s_fs_backend;
/** @brief Mounted existing SD volume. */
static ra8_fs_mount_t* s_mount;
/** @brief Transactional VFS storage adapter. */
static ra8_mdl_storage_vfs_t s_storage;
/** @brief Strict RBKC validator and final reader. */
static ra8_mdl_rabook_vfs_t s_rabook;
/** @brief Streaming RA8-side SHA-256 context. */
static ra8_rsip_sha256_ctx_t s_sha;

/** @brief RBKC offset table: up to 128 MiB inflated at 64 KiB per chunk. */
[[gnu::section(".sdram_data"), gnu::aligned(8)]] static uint64_t s_table[k_media_table_entries];
/** @brief One compressed zlib stream. */
[[gnu::section(".sdram_data"),
  gnu::aligned(8)]] static uint8_t s_compressed[k_media_compressed_bytes];
/** @brief One inflated RBKC chunk and final-consumption destination. */
[[gnu::section(".sdram_data"), gnu::aligned(8)]] static uint8_t s_chunk[k_media_rbkc_chunk_bytes];
/** @brief Independent strict semantic and CRC scratch. */
[[gnu::section(".sdram_data"), gnu::aligned(8)]] static uint8_t s_scratch[k_media_scratch_bytes];

/**
 * @brief Write one bounded NUL-terminated diagnostic to the board console.
 * @details Measures only within the fixed diagnostic cap, then sends that
 * byte span through the board's allocation-free UART adapter.
 * @param[in] text NUL-terminated text to emit.
 * @pre The J-Link OB console was initialized.
 * @pre @p text is non-null and terminates within 256 bytes.
 * @post At most 256 bytes were presented to the UART driver.
 * @post No application state was modified.
 * @note Single-worker use; no synchronization is provided.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_puts(const char* text)
{
  size_t length = 0U;
  while ((length < 256U) && (text[length] != '\0')) {
    length++;
  }
  (void)ra8_board_uart_console_write((const uint8_t*)text, length);
}

/**
 * @brief Emit one failed-stage diagnostic.
 * @details Composes the stage label and canonical error spelling as bounded
 * fragments so a failure path never needs a formatting stream or scratch.
 * @param[in] stage Stable stage label.
 * @param[in] err Canonical RA8 error for that stage.
 * @return Always false for direct propagation through a phase chain.
 * @retval false The stage failed.
 * @pre The console is initialized.
 * @pre @p stage is a bounded NUL-terminated string.
 * @post One FAIL line was queued.
 * @post No transfer or storage state was changed.
 * @note Formatting uses bounded writes, never stdio.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_fail(const char* stage, ra8_err_t err)
{
  internal_puts("media_download: FAIL ");
  internal_puts(stage);
  internal_puts(" err=");
  internal_puts(ra8_err_to_str(err));
  internal_puts("\r\n");
  return false;
}

/**
 * @brief Park the core permanently after an unrecoverable startup failure.
 * @pre Reset handling configured the CPU for WFI.
 * @pre No recovery path remains.
 * @post The function never returns.
 * @post The core repeatedly executes WFI.
 * @note Used only before or after ThreadX owns scheduling.
 * @since 0.1.0
 */
[[noreturn]] RA8_INTERNAL static void internal_halt(void)
{
  while (true) {
    __asm volatile("wfi");
  }
}

/**
 * @brief Bring up clocks, MSTP, delay timing, and the board console.
 * @details Establishes the clock values consumed by the C6 and SD transports
 * before ThreadX can schedule their single owning worker.
 * @pre Reset startup initialized `.data` and `.bss`.
 * @pre No peripheral service has been initialized yet.
 * @post Success leaves CPUCLK0/PCLKA cached and the console usable.
 * @post Any failure parks the core without starting ThreadX.
 * @note Called exactly once from ::main.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_setup_or_halt(void)
{
  if ((ra8_cgc_init() != k_ra8_ok) ||
      (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &s_cpuclk_hz) != k_ra8_ok) ||
      (ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, &s_pclka_hz) != k_ra8_ok) ||
      (ra8_mstp_init() != k_ra8_ok) || (ra8_time_init(s_cpuclk_hz) != k_ra8_ok) ||
      (ra8_board_uart_console_init((uint32_t)k_media_uart_baud) != k_ra8_ok)) {
    internal_halt();
  }
}

/**
 * @brief Latch the two station events used by the bounded association wait.
 * @details Reduces the asynchronous event stream to the two terminal states
 * needed by the worker's bounded association loop.
 * @param[in] context Unused callback context.
 * @param[in] event C6 event record, or null for a defensive no-op.
 * @pre The C6 link invokes this synchronously while polling.
 * @pre File-scope event latches are writable.
 * @post Connected/disconnected station events set their matching latch.
 * @post Other events and null input leave both latches unchanged.
 * @note The worker is the only reader, so byte latches are sufficient.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_on_event(void* context, const ra8_c6link_event_t* event)
{
  (void)context;
  if (event == nullptr) {
    return;
  }
  if (event->kind == k_ra8_c6link_event_sta_connected) {
    s_connected = 1U;
  } else if (event->kind == k_ra8_c6link_event_sta_disconnected) {
    s_disconnected = 1U;
  }
}

/**
 * @brief Pump the C6 link until association succeeds or the fixed budget ends.
 * @details Alternates bounded RPC polling with ThreadX sleeps and stops on the
 * first connection, disconnection, or exhausted-attempt condition.
 * @return Whether the station-connected event arrived first.
 * @retval true Association completed.
 * @retval false Disconnection or the poll budget ended first.
 * @pre The link is open and a join request was accepted.
 * @pre The worker exclusively owns the link.
 * @post At most ::k_media_assoc_tries poll calls were made.
 * @post No storage operation was attempted.
 * @note ThreadX sleeps between polls to avoid a busy wait.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_wait_connected(void)
{
  for (uint32_t attempt = 0U; attempt < (uint32_t)k_media_assoc_tries; ++attempt) {
    ra8_c6link_stats_t stats = {};
    (void)ra8_c6link_poll(&s_link, (uint16_t)k_ra8_c6link_announce_transfers, &stats);
    if (s_connected != 0U) {
      return true;
    }
    if (s_disconnected != 0U) {
      return false;
    }
    tx_thread_sleep((ULONG)k_media_assoc_gap_ms);
  }
  return false;
}

/**
 * @brief Open the C6 link, prove readiness, and associate its Wi-Fi station.
 * @details Binds the ESP-hosted transport, completes the C6 readiness
 * handshake, starts Wi-Fi, and applies the immutable build-time station
 * credentials.
 * @return Association status.
 * @retval k_ra8_ok The C6 is ready and the station connected.
 * @retval k_ra8_err_invalid_arg Credentials are empty.
 * @pre ESP-hosted port initialization succeeded.
 * @pre The C6 worker owns the transport.
 * @post Success leaves ::s_link open and associated.
 * @post Failure publishes no storage object.
 * @note No NetX stack is needed; HTTPS executes on the C6 service.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_open_and_join(void)
{
  if (s_ssid[0] == '\0') {
    return k_ra8_err_invalid_arg;
  }
  ra8_c6link_cfg_t link_config = {};
  ra8_err_t        err         = ra8_esp_hosted_c6link_bind(&link_config.transport);
  if (err != k_ra8_ok) {
    return err;
  }
  link_config.arena       = s_c6_arena;
  link_config.arena_bytes = sizeof(s_c6_arena);
  link_config.event_cb    = internal_on_event;
  err                     = ra8_c6link_open(&s_link, &link_config);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_c6link_fw_version_t version = {};
  err = ra8_c6link_await_ready(&s_link, (uint16_t)k_ra8_c6link_announce_transfers, &version);
  if (err == k_ra8_ok) {
    err = ra8_c6link_wifi_start(&s_link);
  }
  ra8_c6link_sta_cfg_t station = {};
  if (err == k_ra8_ok) {
    err = ra8_c6link_sta_cfg_set(&station, s_ssid, s_psk);
  }
  if (err == k_ra8_ok) {
    err = ra8_c6link_wifi_join(&s_link, &station);
  }
  if ((err == k_ra8_ok) && !internal_wait_connected()) {
    err = k_ra8_err_timeout;
  }
  return err;
}

/**
 * @brief Mount the existing Pmod2 SD FAT volume under the VFS name `sd`.
 * @details Composes the board SCI transport through SDMMC, FAT, and VFS layers,
 * then prepares only the bounded `BOOKS` namespace without formatting media.
 * @return Mount and directory-preparation status.
 * @retval k_ra8_ok The mount and `sd:/BOOKS` directory are ready.
 * @retval k_ra8_err_invalid_arg `BOOKS` exists but is not a directory.
 * @pre PCLKA is cached and the Pmod2 pins are free.
 * @pre The card already contains a supported FAT volume; it is never formatted.
 * @post Success leaves ::s_mount live and registered as `sd`.
 * @post Failure never creates or replaces the destination book.
 * @note The storage transaction later refuses an existing destination.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mount_sd(void)
{
  const ra8_sdmmc_spi_sci_pins_t pins = {
    .sck  = (ra8_port_pin_t)k_ra8_board_pmod2_spi_sck,
    .cipo = (ra8_port_pin_t)k_ra8_board_pmod2_spi_cipo,
    .copi = (ra8_port_pin_t)k_ra8_board_pmod2_spi_copi,
    .cs   = (ra8_port_pin_t)k_ra8_board_pmod2_spi_cs,
  };
  ra8_sdmmc_spi_transport_t transport = {};
  ra8_err_t err = ra8_sdmmc_spi_transport_sci((uint8_t)k_ra8_board_pmod2_sci_channel,
                                              s_pclka_hz,
                                              &pins,
                                              &transport);
  if (err == k_ra8_ok) {
    err = ra8_sdmmc_spi_init(&transport);
  }
  if (err == k_ra8_ok) {
    err = ra8_sdmmc_spi_bind_fs_backend(&s_fs_backend);
  }
  if (err == k_ra8_ok) {
    err = ra8_fs_mount(&s_fs_backend, &s_mount);
  }
  if (err == k_ra8_ok) {
    err = ra8_io_vfs_init();
  }
  if (err == k_ra8_ok) {
    err = ra8_io_vfs_mount("sd", s_mount);
  }
  ra8_io_vfs_stat_t book_dir = {};
  if (err == k_ra8_ok) {
    err = ra8_io_vfs_stat("sd:/BOOKS", &book_dir);
  }
  if ((err == k_ra8_ok) && !book_dir.exists) {
    err = ra8_io_vfs_mkdir("sd:/BOOKS");
  } else if ((err == k_ra8_ok) && !book_dir.is_directory) {
    err = k_ra8_err_invalid_arg;
  }
  return err;
}

/**
 * @brief Inflate one zlib-wrapped RBKC chunk without heap allocation.
 * @details Adapts miniz's bounded memory-to-memory decoder to the strict RBKC
 * reader callback and maps an incomplete or malformed stream to validation.
 * @param[in] source Compressed bytes.
 * @param[in] source_bytes Compressed byte count.
 * @param[out] destination Inflated destination.
 * @param[in] destination_capacity Writable destination capacity.
 * @param[out] output_bytes Exact inflated count.
 * @return Inflation or validation status.
 * @retval k_ra8_ok One complete stream inflated.
 * @retval k_ra8_err_validation_failed Miniz rejected the stream.
 * @pre All pointers are non-null and their extents match the sizes.
 * @pre The build defines `MINIZ_NO_MALLOC` and `MINIZ_NO_STDIO`.
 * @post Success initializes exactly `*output_bytes` destination bytes.
 * @post Failure publishes no reader metadata.
 * @note The callback satisfies ::ra8_book_inflate_fn.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_inflate(const void* source,
                                               size_t      source_bytes,
                                               void*       destination,
                                               size_t      destination_capacity,
                                               size_t*     output_bytes)
{
  if ((source == nullptr) || (destination == nullptr) || (output_bytes == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  const size_t inflated = tinfl_decompress_mem_to_mem(
    destination,
    destination_capacity,
    source,
    source_bytes,
    (uint32_t)(TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF));
  if (inflated == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
    return k_ra8_err_validation_failed;
  }
  *output_bytes = inflated;
  return k_ra8_ok;
}

/**
 * @brief Reset the caller-owned SHA-256 stream.
 * @details Adapts the transfer coordinator's opaque context callback to the
 * caller-owned RSIP SHA-256 state without copying or retaining input bytes.
 * @param[in,out] context Bound ::ra8_rsip_sha256_ctx_t.
 * @return SHA initialization status.
 * @retval k_ra8_ok The digest stream was reset.
 * @retval k_ra8_err_invalid_arg The bound context was invalid.
 * @pre @p context points to ::s_sha.
 * @pre The context is exclusively owned by one transfer.
 * @post Success resets the streaming digest.
 * @post No C6 or storage state changes.
 * @note The coordinator invokes this once before accepting body bytes.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_sha_init(void* context)
{
  return ra8_rsip_sha256_init((ra8_rsip_sha256_ctx_t*)context);
}

/**
 * @brief Feed one accepted body chunk to the independent RA8 digest.
 * @details Preserves transfer order while forwarding each accepted C6 body
 * fragment to the streaming RSIP digest used for final wire verification.
 * @param[in,out] context Bound ::ra8_rsip_sha256_ctx_t.
 * @param[in] data Ordered body bytes.
 * @param[in] length Body byte count.
 * @return SHA update status.
 * @retval k_ra8_ok The complete fragment entered the digest.
 * @retval k_ra8_err_invalid_arg The state or input span was invalid.
 * @pre @p context was initialized and @p data covers @p length bytes.
 * @pre Calls remain in transfer order.
 * @post Success incorporates all @p length bytes.
 * @post The source bytes are unchanged.
 * @note The transfer chunk is bounded to fit the callback's uint16 length.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_sha_update(void* context, const uint8_t* data, uint16_t length)
{
  return ra8_rsip_sha256_update((ra8_rsip_sha256_ctx_t*)context, data, length);
}

/**
 * @brief Finalize the independent digest into the coordinator output.
 * @details Completes the RSIP stream directly into the transfer coordinator's
 * fixed-size digest destination for comparison with the C6-advertised hash.
 * @param[in,out] context Bound ::ra8_rsip_sha256_ctx_t.
 * @param[out] output Exact SHA-256 digest span.
 * @return SHA finalization status.
 * @retval k_ra8_ok The complete digest was written.
 * @retval k_ra8_err_invalid_arg The state or output span was invalid.
 * @pre @p context received every accepted body byte.
 * @pre @p output covers ::k_ra8_mdl_sha256_bytes bytes.
 * @post Success initializes the complete digest.
 * @post The context is no longer an active digest stream.
 * @note Finalization retains no artifact body bytes.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_sha_final(void*   context,
                                                 uint8_t output[k_ra8_mdl_sha256_bytes])
{
  return ra8_rsip_sha256_final((ra8_rsip_sha256_ctx_t*)context, output);
}

/**
 * @brief Bind strict reader, transaction, SHA, and coordinator policy.
 * @details Connects only caller-owned workspaces and the mounted VFS to a
 * create-new transaction whose validator reopens every RBKC stream.
 * @param[out] output Complete transfer configuration.
 * @return Adapter binding status.
 * @retval k_ra8_ok Every caller-owned seam is ready.
 * @pre The SD VFS mount is registered.
 * @pre All static workspaces are exclusively available.
 * @post Success binds strict validation before transaction commit.
 * @post Failure starts no storage transaction.
 * @note The 32 MiB transfer ceiling is independent of inflated RBKC size.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_bind_transfer(ra8_mdl_transfer_config_t* output)
{
  const ra8_mdl_rabook_vfs_config_t reader_config = {
    .inflate        = internal_inflate,
    .table          = s_table,
    .compressed     = s_compressed,
    .chunk          = s_chunk,
    .scratch        = s_scratch,
    .table_cap      = (uint32_t)k_media_table_entries,
    .compressed_cap = sizeof(s_compressed),
    .chunk_cap      = sizeof(s_chunk),
    .scratch_cap    = sizeof(s_scratch),
  };
  ra8_err_t               err               = ra8_mdl_rabook_vfs_init(&s_rabook, &reader_config);
  ra8_mdl_storage_iface_t storage_interface = {};
  if (err == k_ra8_ok) {
    const ra8_mdl_storage_vfs_config_t storage_config = {
      .stage_leaf   = s_stage_leaf,
      .validate     = ra8_mdl_rabook_vfs_validate,
      .validate_ctx = &s_rabook,
    };
    err = ra8_mdl_storage_vfs_init(&s_storage, &storage_config, &storage_interface);
  }
  if (err == k_ra8_ok) {
    *output = (ra8_mdl_transfer_config_t){
      .storage     = storage_interface,
      .sha256      = {.init   = internal_sha_init,
                      .update = internal_sha_update,
                      .final  = internal_sha_final,
                      .ctx    = &s_sha},
      .chunk_bytes = (uint16_t)k_media_transfer_chunk,
      .max_chunks  = (uint32_t)k_media_transfer_chunks,
    };
  }
  return err;
}

/**
 * @brief Reopen the committed RBKC and consume its first exact inflated chunk.
 * @details Revalidates final-file geometry through the reusable reader and
 * performs one demand-paged chunk read to prove post-publication consumption.
 * @return Strict reader and consumption status.
 * @retval k_ra8_ok Metadata and the first chunk were read successfully.
 * @retval k_ra8_err_invalid_size Geometry exceeds the bound workspace.
 * @pre The transaction committed ::s_destination.
 * @pre ::s_rabook remains initialized and closed.
 * @post The final reader is closed on every path after a successful open.
 * @post Success proves at least one demand-paged read from the published file.
 * @note Full semantic validation already consumed every chunk before commit.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_consume(void)
{
  ra8_err_t         err       = ra8_mdl_rabook_vfs_open(&s_rabook, s_destination);
  ra8_book_header_t header    = {};
  uint64_t          flat_size = 0U;
  if (err == k_ra8_ok) {
    err = ra8_mdl_rabook_vfs_info(&s_rabook, &header, &flat_size);
  }
  uint32_t first_bytes = 0U;
  if (err == k_ra8_ok) {
    first_bytes = s_rabook.reader.chunk_bytes;
    if (flat_size < first_bytes) {
      first_bytes = (uint32_t)flat_size;
    }
    if ((first_bytes == 0U) || (first_bytes > sizeof(s_chunk)) ||
        ((uint64_t)header.total_size != flat_size)) {
      err = k_ra8_err_invalid_size;
    }
  }
  if (err == k_ra8_ok) {
    err = ra8_mdl_rabook_vfs_read_chunk(&s_rabook, 0U, s_chunk, first_bytes);
  }
  const ra8_err_t close_err = ra8_mdl_rabook_vfs_close(&s_rabook);
  return ((err == k_ra8_ok) ? close_err : err);
}

/**
 * @brief Execute the complete hardware composition once.
 * @details Sequences storage mount, RSIP self-test, C6 association, bounded
 * transfer, strict precommit validation, publication, and final consumption.
 * @return End-to-end verdict.
 * @retval true The artifact was downloaded, committed, and consumed.
 * @retval false One named stage failed.
 * @pre The worker is running and ESP-hosted port init completed.
 * @pre Build-time URL and credentials may be empty but are never null.
 * @post Success leaves one validated destination and no stage file.
 * @post Failure emits a named diagnostic and publishes no new destination.
 * @note An existing destination is intentionally preserved and causes failure.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_run(void)
{
  if (s_port_err != k_ra8_ok) {
    return internal_fail("c6 port", s_port_err);
  }
  if ((s_url[0] == '\0') || (s_ssid[0] == '\0')) {
    return internal_fail("build configuration", k_ra8_err_invalid_arg);
  }
  ra8_err_t err = internal_mount_sd();
  if (err != k_ra8_ok) {
    return internal_fail("sd mount", err);
  }
  const ra8_rsip_config_t rsip_config = {.run_bist = true};
  err                                 = ra8_rsip_init(&rsip_config);
  if (err != k_ra8_ok) {
    return internal_fail("rsip", err);
  }
  ra8_delay_ms((uint32_t)k_media_c6_boot_wait_ms);
  err = internal_open_and_join();
  if (err != k_ra8_ok) {
    return internal_fail("c6 join", err);
  }
  ra8_mdl_transfer_config_t transfer = {};
  err                                = internal_bind_transfer(&transfer);
  if (err == k_ra8_ok) {
    ra8_mdl_transfer_result_t result = {};
    err = ra8_c6link_mdl_transfer(&s_link, s_url, s_destination, &transfer, &result);
  }
  if (err == k_ra8_ok) {
    err = internal_consume();
  }
  (void)ra8_c6link_close(&s_link);
  if (err != k_ra8_ok) {
    return internal_fail("transfer/consume", err);
  }
  internal_puts("media_download: PASS C6->SD strict rabook publish and read\r\n");
  return true;
}

/**
 * @brief Keep the final verdict visible without busy-waiting.
 * @param[in] passed Stable end-to-end verdict.
 * @pre The ThreadX worker is running.
 * @pre The console remains initialized.
 * @post The function never returns.
 * @post One bounded heartbeat is emitted every configured interval.
 * @note Storage and C6 transports are not touched after the verdict.
 * @since 0.1.0
 */
[[noreturn]] RA8_INTERNAL static void internal_heartbeat(bool passed)
{
  while (true) {
    internal_puts(passed ? "media_download: alive PASS\r\n" : "media_download: alive FAIL\r\n");
    tx_thread_sleep((ULONG)k_media_heartbeat_ms);
  }
}

/**
 * @brief Worker entry for the one-shot media transaction.
 * @details Runs the complete transaction exactly once and converts its stable
 * result into a low-duty-cycle heartbeat for bench observation.
 * @param[in] input Unused ThreadX entry value.
 * @pre ThreadX started this worker after ::tx_application_define.
 * @pre The console and clock cache are ready.
 * @post The worker transitions permanently into verdict heartbeat.
 * @post The transaction runs at most once.
 * @note This worker exclusively owns C6, VFS, reader, and SHA contexts.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_worker(ULONG input)
{
  (void)input;
  internal_heartbeat(internal_run());
}

/**
 * @brief Initialize ESP-hosted and create the single application worker.
 * @param[in] first_unused_memory ThreadX free-memory pointer; unused.
 * @pre ::tx_kernel_enter was called and PCLKA is cached.
 * @pre No ESP-hosted API was called earlier.
 * @post ::s_port_err records the exact port-init status.
 * @post A successful thread creation starts exactly one worker.
 * @note ThreadX object creation belongs in this kernel hook.
 * @since 0.1.0
 */
void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;
  const ra8_esp_hosted_port_cfg_t port_config = {
    .pclk_hz      = s_pclka_hz,
    .sck_hz       = (uint32_t)k_media_c6_sck_hz,
    .edge_poll_ms = (uint16_t)k_media_c6_edge_poll_ms,
    .sci_channel  = (uint8_t)k_ra8_board_pmod1_sci_channel,
  };
  s_port_err = ra8_esp_hosted_port_init(&port_config);
  if (tx_thread_create(&s_worker,
                       s_worker_name,
                       internal_worker,
                       0U,
                       s_worker_stack,
                       sizeof(s_worker_stack),
                       (UINT)k_media_worker_priority,
                       (UINT)k_media_worker_priority,
                       TX_NO_TIME_SLICE,
                       TX_AUTO_START) != TX_SUCCESS) {
    internal_puts("media_download: FAIL worker create\r\n");
  }
}

/**
 * @brief Initialize the board and hand control to ThreadX.
 * @pre `Reset_Handler` initialized static storage.
 * @pre `SystemInit` configured VTOR, FPU, and priority grouping.
 * @post The startup banner is printed once.
 * @post Control enters ThreadX and never returns normally.
 * @note The #707 freestanding contract is declared by `ra8_boot_entry.h`.
 * @since 0.1.0
 */
void main(void)
{
  internal_setup_or_halt();
  ra8_isr_globals_enable();
  internal_puts("media_download: boot; C6 Pmod1 + SD Pmod2\r\n");
  tx_kernel_enter();
  internal_halt();
}
