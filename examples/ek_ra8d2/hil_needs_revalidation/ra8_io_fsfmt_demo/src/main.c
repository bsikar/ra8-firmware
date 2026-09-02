/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/ra8_io_fsfmt_demo/src/main.c
 * @brief ra8_io pluggable filesystem-format registry demo (Phase 4, #159).
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Exercises the format registry that lets the fabric recognise the on-disk
 * filesystem without the upper layers hard-coding a `switch` over FAT vs exFAT:
 *   1. Build a RAM block device, bridge it to ra8_fs, and format a FAT12 volume
 *      on it (Phase 1/3, #156/#158).
 *   2. Register the built-in formats (`ra8_io_fsfmt_init`) and probe the FAT
 *      volume: it must resolve to the "fat" format and report FAT capabilities
 *      (writable, streaming-write, 8.3 name length).
 *   3. Demonstrate the foreign-format seam: register a bounded read-only format
 *      whose probe claims a magic byte in block 0 and whose namespace exposes
 *      one immutable file. A second tiny RAM device must resolve to "stub" with
 *      its case-sensitive capabilities, proving a new format plugs in with no
 *      change to the registry's built-ins.
 *   4. Report progress on the SCI8 console through a ra8_io UART stream sink.
 *
 * The ra8_emulator captures the SCI8 console, so the PASS line is
 * observable headlessly: a successful run prints
 * `ra8_io_fsfmt_demo: probed fat maxname=741 + foreign stub seam PASS`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_io.h"
#include "ra8_io_fsfmt.h"
#include "ra8_log.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_sci.h"
#include "ra8_time.h"

/** @enum demo_const_t @brief Console + volume knobs (no magic numbers). */
typedef enum : uint32_t {
  k_demo_uart_chan    = 8U,      /**< SCI8 J-Link OB console.               */
  k_demo_uart_baud    = 115200U, /**< Console baud.                         */
  k_demo_disk_blocks  = 512U,    /**< 256 KiB RAM-disk (FAT12).             */
  k_demo_stub_blocks  = 8U,      /**< 4 KiB tiny disk for the stub vol.     */
  k_demo_pin_shift    = 8U,      /**< Port byte position in ra8_port_pin_t. */
  k_demo_stub_maxname = 16U,     /**< Stub format max name length.          */
  k_demo_stub_magic   = 0x5AU,   /**< Stub block-0 signature byte.          */
} demo_const_t;

/** @brief SCI8 console TXD = PD02. */
static const ra8_port_pin_t s_demo_txd =
  (ra8_port_pin_t)(((uint16_t)k_ra8_port_13 << (uint16_t)k_demo_pin_shift) | (uint16_t)k_ra8_pin_2);
/** @brief SCI8 console RXD = PD03. */
static const ra8_port_pin_t s_demo_rxd =
  (ra8_port_pin_t)(((uint16_t)k_ra8_port_13 << (uint16_t)k_demo_pin_shift) | (uint16_t)k_ra8_pin_3);

/** @brief 256 KiB RAM-disk backing buffer for the FAT volume (in SRAM .bss). */
static uint8_t s_disk[(size_t)k_demo_disk_blocks * (size_t)k_ra8_io_block_size_bytes];
/** @brief 4 KiB RAM-disk backing buffer for the stub-signature volume. */
static uint8_t s_stub_disk[(size_t)k_demo_stub_blocks * (size_t)k_ra8_io_block_size_bytes];
/** @brief Block-device handle + its RAM backend state (FAT volume). */
static ra8_io_blockdev_t           s_bd;
static ra8_io_blockdev_ram_state_t s_bstate;
/** @brief ra8_fs backend bridged onto the FAT block device. */
static ra8_fs_backend_t s_be;
/** @brief UART output stream + its sink state. */
static ra8_io_stream_t            s_uart;
static ra8_io_stream_uart_state_t s_ust;

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_fsfmt_demo";

/** @brief Immutable contents served by the foreign demo format. */
static const uint8_t s_demo_stub_payload[] = "registered-format-data";

/** @brief Single caller-independent mount context for the demo format. */
typedef struct {
  bool mounted; /**< True between successful mount and unmount operations. */
} demo_stub_mount_t;

/** @brief Single bounded stream context for the foreign demo file. */
typedef struct {
  uint32_t offset; /**< Current read cursor within ::s_demo_stub_payload. */
  bool     open;   /**< True while the immutable file is open.            */
} demo_stub_file_t;

/** @brief Caller-independent foreign-format mount state. */
static demo_stub_mount_t s_demo_stub_mount;
/** @brief Caller-independent foreign-format stream state. */
static demo_stub_file_t s_demo_stub_file;

/**
 * @brief Print a NUL-terminated string on the UART stream.
 *
 * @details Delegates string emission to the initialized stream and intentionally
 * ignores diagnostic-output errors in this terminal demo.
 *
 * @param[in] msg NUL-terminated message to emit.
 * @pre @p msg is non-NULL and readable through its terminator.
 * @pre ``s_uart`` has been initialized with its caller-owned UART state.
 * @post The stream has accepted the message or reported an ignored sink error.
 * @post Filesystem-format registry and block-device state remain unchanged.
 * @note This single-threaded diagnostic helper performs no retry.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_demo_print(const char* msg)
{
  (void)ra8_io_stream_puts(&s_uart, msg);
}

/**
 * @brief Test whether block zero carries the foreign-format stub signature.
 *
 * @details Validates the backend seam, reads exactly one block into bounded
 * stack storage, and compares its first byte with the demo magic value.
 *
 * @param[in] be Backend whose block zero is probed.
 * @return Whether the backend contains the stub signature.
 * @retval true The block read succeeded and the signature byte matched.
 * @retval false The backend was invalid, the read failed, or the byte differed.
 * @pre A non-NULL backend provides a context compatible with ``read_block``.
 * @pre The backend exposes at least one standard-size block when readable.
 * @post The backend contents and registry remain unchanged.
 * @post Temporary block storage leaves scope on return.
 * @note Probe failures collapse to ``false`` so the registry can try another format.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_stub_probe(const ra8_fs_backend_t* be)
{
  if (be == nullptr) {
    return false;
  }
  if (be->read_block == nullptr) {
    return false;
  }
  uint8_t blk[(size_t)k_ra8_io_block_size_bytes];
  if (be->read_block(be->ctx, 0, 1, blk) != k_ra8_ok) {
    return false;
  }
  return blk[0] == (uint8_t)k_demo_stub_magic;
}

/**
 * @brief Mount the bounded foreign demo format.
 *
 * @details Revalidates the volume marker and publishes the one statically
 * allocated mount context when it is not already active.
 *
 * @param[in] backend Backend selected by the registry.
 * @param[out] out_mount Receives the mounted foreign context.
 * @return Mount validation and lifecycle status.
 * @retval k_ra8_ok The foreign context was mounted and published.
 * @pre @p backend identifies the backend that matched the signature probe.
 * @pre @p out_mount, when non-NULL, is owned by the caller.
 * @post Success marks the singleton context mounted and publishes its address.
 * @post Failure leaves the mount state and output destination unchanged.
 * @note The example has one mount slot and performs no allocation.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_stub_mount(const ra8_fs_backend_t* backend, void** out_mount)
{
  if (out_mount == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (!internal_stub_probe(backend)) {
    return k_ra8_err_not_found;
  }
  if (s_demo_stub_mount.mounted) {
    return k_ra8_err_busy;
  }
  s_demo_stub_mount.mounted = true;
  *out_mount                = &s_demo_stub_mount;
  return k_ra8_ok;
}

/**
 * @brief Release the mounted foreign demo context.
 * @details Validates the singleton identity before clearing its lifecycle bit.
 * @param[in,out] mount_ctx Context previously returned by ::internal_stub_mount.
 * @return Unmount validation status.
 * @retval k_ra8_ok The live foreign context was released.
 * @pre @p mount_ctx is the only context this format can publish.
 * @pre No foreign stream remains open.
 * @post Success leaves the singleton context available for a later mount.
 * @post Failure leaves the context state unchanged.
 * @note No resource release callback or allocator is required.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_stub_unmount(void* mount_ctx)
{
  if ((mount_ctx != &s_demo_stub_mount) || !s_demo_stub_mount.mounted || s_demo_stub_file.open) {
    return k_ra8_err_invalid_state;
  }
  s_demo_stub_mount.mounted = false;
  return k_ra8_ok;
}

/**
 * @brief Open the immutable foreign demo file.
 * @details Accepts only read mode and the exact `/README.TXT` path, then resets
 * the single bounded cursor.
 * @param[in] mount_ctx Active foreign mount context.
 * @param[in] path Normalized volume-relative path.
 * @param[in] mode Requested open mode.
 * @param[out] out_file Receives the singleton file context.
 * @return Path, mode, and lifecycle status.
 * @retval k_ra8_ok The immutable file was opened at offset zero.
 * @pre Required pointers follow the registered filesystem callback contract.
 * @pre @p mount_ctx identifies the active demo mount.
 * @post Success publishes the open file context at offset zero.
 * @post Failure does not alter the output destination.
 * @note The format deliberately supports one concurrent read stream.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_stub_open(void* mount_ctx, const char* path, ra8_fs_mode_t mode, void** out_file)
{
  if ((path == nullptr) || (out_file == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((mount_ctx != &s_demo_stub_mount) || !s_demo_stub_mount.mounted) {
    return k_ra8_err_invalid_state;
  }
  if (mode != k_ra8_fs_mode_read) {
    return k_ra8_err_not_supported;
  }
  if (strcmp(path, "/README.TXT") != 0) {
    return k_ra8_err_not_found;
  }
  if (s_demo_stub_file.open) {
    return k_ra8_err_busy;
  }
  s_demo_stub_file.offset = 0U;
  s_demo_stub_file.open   = true;
  *out_file               = &s_demo_stub_file;
  return k_ra8_ok;
}

/**
 * @brief Close the immutable foreign demo file.
 * @details Validates the singleton identity and consumes its open lifecycle.
 * @param[in,out] file_ctx Context returned by ::internal_stub_open.
 * @return Close validation status.
 * @retval k_ra8_ok The live file context was closed.
 * @pre @p file_ctx is the single format-owned stream context.
 * @pre The stream is currently open.
 * @post Success clears the open lifecycle bit.
 * @post Payload and mount state remain unchanged.
 * @note No flush is necessary because the payload is immutable.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_stub_close(void* file_ctx)
{
  if ((file_ctx != &s_demo_stub_file) || !s_demo_stub_file.open) {
    return k_ra8_err_invalid_state;
  }
  s_demo_stub_file.open = false;
  return k_ra8_ok;
}

/**
 * @brief Read a bounded prefix from the immutable foreign payload.
 * @details Copies at most the remaining payload bytes and advances only by the
 * count reported through @p out_read.
 * @param[in,out] file_ctx Active foreign file context.
 * @param[out] buf Caller-owned read destination.
 * @param[in] bytes Requested byte count.
 * @param[out] out_read Receives the copied byte count.
 * @return Stream validation status.
 * @retval k_ra8_ok Zero or more bytes were copied without exceeding @p bytes.
 * @pre @p buf addresses @p bytes writable bytes when @p bytes is nonzero.
 * @pre @p out_read addresses writable count storage.
 * @post Success advances the cursor by exactly `*out_read`.
 * @post Payload and mount state remain unchanged.
 * @note End of file is reported as success with a zero count.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_stub_read(void* file_ctx, void* buf, uint32_t bytes, uint32_t* out_read)
{
  if ((out_read == nullptr) || ((buf == nullptr) && (bytes != 0U))) {
    return k_ra8_err_null_ptr;
  }
  if ((file_ctx != &s_demo_stub_file) || !s_demo_stub_file.open) {
    return k_ra8_err_invalid_state;
  }
  const uint32_t size  = (uint32_t)sizeof(s_demo_stub_payload) - 1U;
  uint32_t       count = size - s_demo_stub_file.offset;
  if (bytes < count) {
    count = bytes;
  }
  (void)memcpy(buf, &s_demo_stub_payload[s_demo_stub_file.offset], count);
  s_demo_stub_file.offset += count;
  *out_read = count;
  return k_ra8_ok;
}

/**
 * @brief Seek within the immutable foreign demo file.
 * @details Clamps the requested absolute offset to the payload extent.
 * @param[in,out] file_ctx Active foreign file context.
 * @param[in] offset_bytes Requested absolute stream offset.
 * @return Seek validation status.
 * @retval k_ra8_ok The bounded stream offset was updated.
 * @pre @p file_ctx identifies the open singleton stream.
 * @pre @p offset_bytes may span the public 64-bit interface range.
 * @post Success sets the offset to `min(offset_bytes, payload_size)`.
 * @post Payload and mount state remain unchanged.
 * @note Seeking beyond EOF selects EOF rather than creating sparse content.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_stub_seek(void* file_ctx, uint64_t offset_bytes)
{
  if ((file_ctx != &s_demo_stub_file) || !s_demo_stub_file.open) {
    return k_ra8_err_invalid_state;
  }
  const uint32_t size     = (uint32_t)sizeof(s_demo_stub_payload) - 1U;
  s_demo_stub_file.offset = (offset_bytes > (uint64_t)size) ? size : (uint32_t)offset_bytes;
  return k_ra8_ok;
}

/**
 * @brief Report the current foreign stream offset.
 * @details Copies the bounded singleton cursor into caller storage.
 * @param[in] file_ctx Active foreign file context.
 * @param[out] out_offset Receives the current byte offset.
 * @return Query validation status.
 * @retval k_ra8_ok The current stream offset was published.
 * @pre @p file_ctx identifies the open singleton stream.
 * @pre @p out_offset addresses writable storage.
 * @post Success publishes an offset no greater than the payload size.
 * @post Stream state and payload remain unchanged.
 * @note Pure with respect to format-owned state.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_stub_tell(const void* file_ctx, uint64_t* out_offset)
{
  if (out_offset == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if ((file_ctx != &s_demo_stub_file) || !s_demo_stub_file.open) {
    return k_ra8_err_invalid_state;
  }
  *out_offset = s_demo_stub_file.offset;
  return k_ra8_ok;
}

/**
 * @brief Report the immutable foreign payload size.
 * @details Publishes the compile-time payload extent without changing the cursor.
 * @param[in] file_ctx Active foreign file context.
 * @param[out] out_bytes Receives the payload byte count.
 * @return Query validation status.
 * @retval k_ra8_ok The immutable payload size was published.
 * @pre @p file_ctx identifies the open singleton stream.
 * @pre @p out_bytes addresses writable storage.
 * @post Success publishes `sizeof(payload) - 1`.
 * @post Stream state and payload remain unchanged.
 * @note The trailing C-string terminator is not part of the file.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_stub_size(const void* file_ctx, uint64_t* out_bytes)
{
  if (out_bytes == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if ((file_ctx != &s_demo_stub_file) || !s_demo_stub_file.open) {
    return k_ra8_err_invalid_state;
  }
  *out_bytes = (uint64_t)sizeof(s_demo_stub_payload) - 1U;
  return k_ra8_ok;
}

/**
 * @brief Report metadata for the foreign root or immutable file.
 * @details Recognizes only `/` and `/README.TXT` and publishes bounded metadata.
 * @param[in] mount_ctx Active foreign mount context.
 * @param[in] path Normalized volume-relative path.
 * @param[out] out Receives metadata for the recognized path.
 * @return Path and lifecycle status.
 * @retval k_ra8_ok Metadata for a recognized path was published.
 * @pre Required pointers follow the registered filesystem callback contract.
 * @pre @p mount_ctx identifies the active demo mount.
 * @post Success fully initializes @p out.
 * @post Failure leaves the namespace and stream state unchanged.
 * @note The format publishes no timestamps.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_stub_stat(void* mount_ctx, const char* path, ra8_fs_stat_t* out)
{
  if ((path == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((mount_ctx != &s_demo_stub_mount) || !s_demo_stub_mount.mounted) {
    return k_ra8_err_invalid_state;
  }
  *out = (ra8_fs_stat_t){};
  if (strcmp(path, "/") == 0) {
    out->attr         = (uint8_t)k_ra8_fs_attr_directory;
    out->is_directory = true;
    return k_ra8_ok;
  }
  if (strcmp(path, "/README.TXT") == 0) {
    out->attr       = (uint8_t)k_ra8_fs_attr_read_only;
    out->size_bytes = (uint64_t)sizeof(s_demo_stub_payload) - 1U;
    return k_ra8_ok;
  }
  return k_ra8_err_not_found;
}

/**
 * @brief Enumerate the foreign root's immutable file.
 * @details Accepts only `/` and invokes the supplied callback exactly once.
 * @param[in] mount_ctx Active foreign mount context.
 * @param[in] path Normalized directory path.
 * @param[in] cb Caller callback receiving the one entry.
 * @param[in,out] cb_ctx Caller context forwarded unchanged.
 * @return Path and lifecycle status.
 * @retval k_ra8_ok The root entry was reported once.
 * @pre Required pointers follow the registered filesystem callback contract.
 * @pre @p mount_ctx identifies the active demo mount.
 * @post Success reports `README.TXT` with read-only metadata.
 * @post Namespace and stream state remain unchanged.
 * @note Callback execution is synchronous.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_stub_listdir(void* mount_ctx, const char* path, ra8_fs_listdir_cb_t cb, void* cb_ctx)
{
  if ((path == nullptr) || (cb == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((mount_ctx != &s_demo_stub_mount) || !s_demo_stub_mount.mounted) {
    return k_ra8_err_invalid_state;
  }
  if (strcmp(path, "/") != 0) {
    return k_ra8_err_not_found;
  }
  cb("README.TXT",
     (uint8_t)k_ra8_fs_attr_read_only,
     (uint64_t)sizeof(s_demo_stub_payload) - 1U,
     cb_ctx);
  return k_ra8_ok;
}

/** @brief Complete read-side operations for the bounded foreign descriptor. */
static const ra8_io_fsfmt_ops_t s_demo_stub_ops = {
  .probe      = internal_stub_probe,
  .mount      = internal_stub_mount,
  .unmount    = internal_stub_unmount,
  .open       = internal_stub_open,
  .close      = internal_stub_close,
  .read       = internal_stub_read,
  .write      = nullptr,
  .seek       = internal_stub_seek,
  .tell       = internal_stub_tell,
  .size       = internal_stub_size,
  .sync       = nullptr,
  .stat       = internal_stub_stat,
  .listdir    = internal_stub_listdir,
  .unlink     = nullptr,
  .rename     = nullptr,
  .mkdir      = nullptr,
  .rmdir      = nullptr,
  .free_space = nullptr,
};

/** @brief Foreign stub format descriptor (read-only, case-sensitive). */
static const ra8_io_fsfmt_t s_demo_stub = {
  .name = "stub",
  .caps =
    {
      .max_name_len             = (uint16_t)k_demo_stub_maxname,
      .read_only                = true,
      .supports_mkdir           = false,
      .supports_streaming_write = false,
      .case_sensitive           = true,
    },
  .ops = &s_demo_stub_ops,
};

/**
 * @brief Bring up CGC, SysTick, and the SCI8 console; halt on failure.
 *
 * @details Resolves CPUCLK0 and PCLKA, initializes the time base, routes both
 * console pins, and configures SCI8 in dependency order.
 *
 * @pre Reset startup has initialized data and BSS storage.
 * @pre Peripheral register mappings for clocks, pins, and SCI8 are accessible.
 * @post On return, SCI8 is configured for the requested diagnostic baud.
 * @post Any required setup failure parks the application before returning.
 * @note This helper is intended for the single-threaded startup path only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) ||
      (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) ||
      (ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, &pclka_hz) != k_ra8_ok) ||
      (ra8_time_init(cpuclk0_hz) != k_ra8_ok) ||
      (ra8_pfs_route_peripheral(s_demo_txd, k_ra8_psel_sci_async, "demo.txd") != k_ra8_ok) ||
      (ra8_pfs_route_peripheral(s_demo_rxd, k_ra8_psel_sci_async, "demo.rxd") != k_ra8_ok)) {
    while (true) {
    }
  }
  const ra8_sci_cfg_t sci_cfg = {.baud      = (uint32_t)k_demo_uart_baud,
                                 .data_bits = k_ra8_sci_data_8,
                                 .parity    = k_ra8_sci_parity_none,
                                 .stop_bits = k_ra8_sci_stop_1,
                                 .pclk_hz   = pclka_hz};
  if (ra8_sci_init((uint8_t)k_demo_uart_chan, &sci_cfg) != k_ra8_ok) {
    while (true) {
    }
  }
}

/**
 * @brief Format a FAT12 RAM volume and require the ``fat`` format probe result.
 *
 * @details Initializes the RAM backend, formats FAT12, initializes the registry,
 * probes the volume, and validates the reported FAT capabilities.
 *
 * @param[out] out_fmt Receives the detected FAT descriptor on success.
 * @return Error from initialization/probing or an explicit capability mismatch.
 * @retval k_ra8_ok FAT was detected with the expected writable capabilities.
 * @retval k_ra8_err_not_found A different format name was reported.
 * @retval k_ra8_err_not_supported Required write capability was absent.
 * @retval k_ra8_err_invalid_size The advertised maximum name length differed.
 * @pre @p out_fmt addresses writable descriptor-pointer storage.
 * @pre The static RAM disk retains its complete declared capacity.
 * @post On success, @p out_fmt points at the registered FAT descriptor.
 * @post The formatted FAT12 volume remains available through ``s_be``.
 * @note The probe validates registry metadata as well as on-disk recognition.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_demo_probe_fat(const ra8_io_fsfmt_t** out_fmt)
{
  RA8_CHECK_NULL_PTR(out_fmt, s_tag, "out_fmt");
  RA8_RETURN_ON_ERROR(
    ra8_io_blockdev_ram_init(&s_bd, &s_bstate, s_disk, (uint32_t)k_demo_disk_blocks, false),
    s_tag,
    "blockdev init");
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_as_fs_backend(&s_bd, &s_be), s_tag, "fs bridge");
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat12;
  opts.label                = "RAIO";
  RA8_RETURN_ON_ERROR(ra8_fs_format(&s_be, &opts), s_tag, "format");

  RA8_RETURN_ON_ERROR(ra8_io_fsfmt_init(), s_tag, "fsfmt init");
  RA8_RETURN_ON_ERROR(ra8_io_fsfmt_probe(&s_be, out_fmt), s_tag, "probe fat");
  if (strcmp((*out_fmt)->name, "fat") != 0) {
    return k_ra8_err_not_found;
  }
  if ((*out_fmt)->caps.read_only || !(*out_fmt)->caps.supports_streaming_write) {
    return k_ra8_err_not_supported;
  }
  if ((*out_fmt)->caps.max_name_len != (uint16_t)k_ra8_io_fsfmt_fat_max_name_utf8) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

/**
 * @brief Register the foreign stub format, craft a volume, and require it wins.
 *
 * @details Registers the file-local descriptor, initializes a tiny RAM backend,
 * writes its signature block, probes it, and verifies advertised capabilities.
 *
 * @return Error from registration/storage/probing or a capability mismatch.
 * @retval k_ra8_ok The stub format was detected with the expected capabilities.
 * @retval k_ra8_err_not_found The registry selected a different format.
 * @retval k_ra8_err_not_supported Required stub capabilities were absent.
 * @pre The format registry has been initialized by the FAT probe stage.
 * @pre The static stub disk retains at least one writable block.
 * @post On success, the stub descriptor remains registered for future probes.
 * @post The first stub-disk byte contains the configured signature value.
 * @note Caller-owned local backend state is sufficient for the synchronous probe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_demo_probe_foreign(void)
{
  RA8_RETURN_ON_ERROR(ra8_io_fsfmt_register(&s_demo_stub), s_tag, "register stub");

  ra8_io_blockdev_t           bd    = {};
  ra8_io_blockdev_ram_state_t state = {};
  RA8_RETURN_ON_ERROR(
    ra8_io_blockdev_ram_init(&bd, &state, s_stub_disk, (uint32_t)k_demo_stub_blocks, false),
    s_tag,
    "stub blockdev");
  uint8_t blk[(size_t)k_ra8_io_block_size_bytes] = {};
  blk[0]                                         = (uint8_t)k_demo_stub_magic;
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_write(&bd, 0, 1, blk), s_tag, "stub mark");

  ra8_fs_backend_t be = {};
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_as_fs_backend(&bd, &be), s_tag, "stub bridge");
  const ra8_io_fsfmt_t* fmt = nullptr;
  RA8_RETURN_ON_ERROR(ra8_io_fsfmt_probe(&be, &fmt), s_tag, "probe stub");
  if (strcmp(fmt->name, "stub") != 0) {
    return k_ra8_err_not_found;
  }
  if (!fmt->caps.read_only || !fmt->caps.case_sensitive) {
    return k_ra8_err_not_supported;
  }
  return k_ra8_ok;
}

/**
 * @brief Firmware entry point.
 *
 * @pre SystemInit set VTOR / FPU / priority grouping.
 */
void main(void)
{
  ra8_log_init();
  internal_demo_setup_or_halt();
  (void)ra8_io_stream_uart_init(&s_uart, &s_ust, (uint8_t)k_demo_uart_chan);
  (void)ra8_io_log_attach(&s_uart); /* route ra8_log into the UART stream too */
  internal_demo_print("ra8_io_fsfmt_demo: boot\r\n");

  const ra8_io_fsfmt_t* fat = nullptr;
  if ((internal_demo_probe_fat(&fat) == k_ra8_ok) && (internal_demo_probe_foreign() == k_ra8_ok)) {
    internal_demo_print("ra8_io_fsfmt_demo: probed ");
    internal_demo_print(fat->name);
    internal_demo_print(" maxname=");
    (void)ra8_io_stream_put_u32(&s_uart, (uint32_t)fat->caps.max_name_len);
    internal_demo_print(" + foreign stub seam PASS\r\n");
  } else {
    internal_demo_print("ra8_io_fsfmt_demo: FAIL\r\n");
  }
  (void)ra8_sci_flush((uint8_t)k_demo_uart_chan);
  while (true) {
  }
}
