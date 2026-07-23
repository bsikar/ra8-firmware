/**
 * @file examples/ek_ra8d2/hw_pending/ra8_io_swap_demo/main.c
 * @brief ra8_io fabric capstone: two block-dev backends swapped behind one
 *        interface, a VFS open/read/write/close round-trip, and stdio
 *        retargeted to both a UART and a RAM stream (epic #155, issue #264).
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Where the sibling `ra8_io_*_demo` apps each bind ONE backend, this app is the
 * capstone that shows off every ra8_io abstraction in a single binary:
 *
 *   1. **Drop-in block-device swap.** One backend-agnostic engine
 *      (::swap_run_one) runs the IDENTICAL fabric round-trip -- bridge the block
 *      device to `ra8_fs`, format + mount a FAT12 volume, register it in the VFS,
 *      write a file, read it back, byte-compare, then tear the mount down -- over
 *      a table of ::swap_backend_t rows. Row 0 is an in-SRAM RAM block device
 *      (`ra8_io_blockdev_ram`, erase-to-zero, no erase-before-write); row 1 is the
 *      on-board Octo-SPI (xSPI) NOR flash (`ra8_io_blockdev_xspi`, an
 *      erase-before-write medium that reads back 0xFF after erase and does a
 *      whole-4-KiB-sector read-modify-write on every 512-byte block write). The
 *      engine names NO peripheral: the two capability-different media are proven
 *      interchangeable behind the one ::ra8_io_blockdev_t vtable.
 *
 *   2. **VFS open/read/write/close.** Each backend is reached by name
 *      (`ram:/DATA.BIN`, `xs:/DATA.BIN`); the round-trip opens for write, writes,
 *      closes, re-opens for read, reads, closes, and compares -- the streaming
 *      file API, not a whole-file shortcut.
 *
 *   3. **Targetable stdio (retarget to two sinks).** The engine writes its
 *      progress through a ::ra8_io_stream_t. During the swap phase that stream is
 *      an in-RAM capture sink (`ra8_io_stream_ram`), so the SAME `puts` / `put_u32`
 *      calls that would otherwise land on the serial console are captured into a
 *      byte buffer. Afterwards the captured bytes are replayed out of the UART
 *      sink (`ra8_io_stream_uart`), proving one writer, two destinations.
 *
 * board_sim models both the RAM region and the OSPI NOR array, so the run is
 * headless and observable over the SCI8 / J-Link OB VCOM console: a successful
 * run prints `ra8_io_swap_demo: two-backend swap (ram + xs) PASS`. The OSPI RMW
 * is slow in the emulator, so give board_sim a generous instruction budget (see
 * this app's README). It lives under hw_pending because the xSPI leg writes the
 * non-volatile on-board NOR and has not yet been captured on the bench; the RAM,
 * VFS, and stdio legs are fully exercised in board_sim and in the host unit test
 * `tests/test_app_ra8_io_swap_demo.c`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_io.h"
#include "ra8_log.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_sci.h"
#include "ra8_time.h"
#include "ra8_xspi.h"

/**
 * @enum swap_const_t
 * @brief Console, volume, and payload knobs (no magic numbers).
 *
 * @details Collects every literal the app uses so the magic-number gate stays
 *          silent and the sizing reads symbolically. The RAM volume is kept small
 *          (in-SRAM .bss) and the xSPI volume matches the sibling
 *          `ra8_io_xspi_demo` window; the payload fits inside one NOR sector so
 *          the erase-before-write round-trip stays cheap.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_swap_uart_chan   = 8U,      /**< SCI8 J-Link OB console.                  */
  k_swap_uart_baud   = 115200U, /**< Console baud.                            */
  k_swap_pin_shift   = 8U,      /**< Port byte position in ra8_port_pin_t.    */
  k_swap_ram_blocks  = 256U,    /**< 128 KiB in-SRAM RAM disk (FAT12).        */
  k_swap_xspi_inst   = 0U,      /**< xSPI controller instance index.          */
  k_swap_xspi_base   = 0U,      /**< Flash byte offset of logical block 0.    */
  k_swap_xspi_blocks = 512U,    /**< 256 KiB OSPI NOR window (FAT12).         */
  k_swap_payload     = 96U,     /**< Bytes written + read back per backend.   */
  k_swap_log_cap     = 512U,    /**< RAM stdio-capture buffer capacity.       */
  k_swap_backends    = 2U,      /**< Number of block-device backends swapped. */
} swap_const_t;

/**
 * @enum swap_pattern_t
 * @brief Deterministic payload-pattern coefficients (no magic numbers).
 *
 * @details The written bytes follow `byte[i] = (i * mul + add) ^ xor` so the
 *          read-back compare is a strong end-to-end check of the whole stack; a
 *          swapped or truncated buffer is caught byte-for-byte.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_swap_pat_mul = 7U,    /**< Linear pattern multiplier.    */
  k_swap_pat_add = 3U,    /**< Linear pattern additive bias. */
  k_swap_pat_xor = 0x5AU, /**< Linear pattern XOR mask.      */
} swap_pattern_t;

/**
 * @struct swap_backend_t
 * @brief One row of the drop-in block-device swap table.
 *
 * @details Describes a single pre-bound block device plus the FAT/VFS identity
 *          the engine uses to mount and address it. The engine (::swap_run_one)
 *          consumes only these fields, so adding a third medium (SD, SDRAM, MRAM)
 *          is one more row -- the engine body never changes.
 *
 * @invariant `bd` points at a block device already bound by its `_init` helper.
 * @invariant `name` is a non-empty VFS volume name with no ':' or '/'.
 *
 * @see swap_run_one
 * @since 0.1.0
 */
typedef struct {
  ra8_io_blockdev_t* bd;    /**< Pre-bound block-device handle (RAM or xSPI). */
  const char*        name;  /**< VFS volume name, e.g. "ram" or "xs".         */
  const char*        label; /**< 11-char-max FAT volume label literal.        */
  const char*        file;  /**< Full "<name>:/DATA.BIN" VFS path.            */
} swap_backend_t;

/** @brief SCI8 console TXD = PD02. */
static const ra8_port_pin_t k_swap_txd =
  (ra8_port_pin_t)(((uint16_t)k_ra8_port_13 << (uint16_t)k_swap_pin_shift) | (uint16_t)k_ra8_pin_2);
/** @brief SCI8 console RXD = PD03. */
static const ra8_port_pin_t k_swap_rxd =
  (ra8_port_pin_t)(((uint16_t)k_ra8_port_13 << (uint16_t)k_swap_pin_shift) | (uint16_t)k_ra8_pin_3);

/** @brief 128 KiB RAM-disk backing buffer (in SRAM .bss). */
static uint8_t s_ram_disk[(size_t)k_swap_ram_blocks * (size_t)k_ra8_io_block_size_bytes];
/** @brief RAM block-device handle + its backend state. */
static ra8_io_blockdev_t           s_ram_bd;
static ra8_io_blockdev_ram_state_t s_ram_state;
/** @brief xSPI (OSPI NOR) block-device handle + its backend state. */
static ra8_io_blockdev_t            s_xspi_bd;
static ra8_io_blockdev_xspi_state_t s_xspi_state;
/** @brief Reusable ra8_fs bridge (one mount is live at a time; reused per row). */
static ra8_fs_backend_t s_be;

/** @brief UART output stream + its sink state (the serial console target). */
static ra8_io_stream_t            s_uart;
static ra8_io_stream_uart_state_t s_uart_state;
/** @brief In-RAM stdio capture stream + its sink state and backing buffer. */
static ra8_io_stream_t           s_ram_log;
static ra8_io_stream_ram_state_t s_ram_log_state;
static uint8_t                   s_ram_log_buf[k_swap_log_cap];

/** @brief Deterministic write payload + read-back scratch (no heap; NASA Rule 3). */
static uint8_t s_payload[k_swap_payload];
static uint8_t s_readback[k_swap_payload];

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_swap_demo";

/**
 * @brief Print a NUL-terminated string on the UART console stream.
 *
 * @details Thin wrapper over ::ra8_io_stream_puts bound to ::s_uart so call sites
 *          do not repeat the sink handle. Used only for the boot banner, the
 *          replay of the RAM capture, and the final verdict lines.
 *
 * @param[in] msg NUL-terminated ASCII string (CR/LF supplied by the caller).
 *
 * @return None.
 *
 * @pre ::s_uart was initialised by ::ra8_io_stream_uart_init.
 * @pre @p msg is non-NULL and NUL-terminated.
 * @post The bytes of @p msg are queued on the UART stream.
 * @post No other state changes.
 *
 * @note Blocking polled TX; not interrupt-safe.
 * @since 0.1.0
 */
static void swap_uart_print(const char* msg)
{
  (void)ra8_io_stream_puts(&s_uart, msg);
}

/**
 * @brief Fill ::s_payload with the deterministic linear byte pattern.
 *
 * @details Writes `byte[i] = (i * k_swap_pat_mul + k_swap_pat_add) ^ k_swap_pat_xor`
 *          over the first @p len bytes so the read-back compare validates the
 *          whole fabric path with reproducible content.
 *
 * @param[in] len Number of bytes to fill (must be <= ::k_swap_payload).
 *
 * @return None.
 *
 * @pre @p len is at most ::k_swap_payload.
 * @pre ::s_payload is file-scope storage (always allocated).
 * @post The first @p len bytes of ::s_payload hold the reproducible pattern.
 * @post No other state is modified.
 *
 * @note Not thread-safe; mutates the shared payload buffer.
 * @since 0.1.0
 */
static void swap_fill(uint32_t len)
{
  for (uint32_t i = 0U; i < len; ++i) {
    s_payload[i] = (uint8_t)(((i * (uint32_t)k_swap_pat_mul) + (uint32_t)k_swap_pat_add) ^
                             (uint32_t)k_swap_pat_xor);
  }
}

/**
 * @brief Open a VFS path for writing, write @p len bytes, and close it.
 *
 * @details Streaming write half of the round-trip: `ra8_io_vfs_open(write)` ->
 *          `ra8_fs_write` -> `ra8_fs_close`. The handle is closed on every path so
 *          no slot leaks even when the write itself fails.
 *
 * @param[in] path Full "<name>:/..." VFS path to create/truncate.
 * @param[in] len  Byte count to write from ::s_payload.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           The payload was written and the handle closed.
 * @retval k_ra8_err_null_ptr @p path was NULL.
 * @retval (other)           The first failing VFS / ra8_fs step's code.
 *
 * @pre @p path is non-NULL and its volume is mounted.
 * @pre @p len is at most ::k_swap_payload.
 * @post On success @p path holds the ::s_payload bytes.
 * @post No file handle is left open on any return path.
 *
 * @note Not thread-safe; single-caller boot context.
 * @since 0.1.0
 */
static ra8_err_t swap_vfs_write(const char* path, uint32_t len)
{
  RA8_CHECK_NULL_PTR(path, s_tag, "path");
  RA8_CHECK_RANGE_TAG(len, 0U, (uint32_t)k_swap_payload, k_ra8_err_invalid_size, s_tag);

  ra8_fs_file_t* wf = nullptr;
  RA8_RETURN_ON_ERROR(ra8_io_vfs_open(path, k_ra8_fs_mode_write, &wf), s_tag, "open w");
  const ra8_err_t werr = ra8_fs_write(wf, s_payload, len);
  RA8_RETURN_ON_ERROR(ra8_fs_close(wf), s_tag, "close w");
  return werr;
}

/**
 * @brief Re-open a VFS path for reading and byte-compare against ::s_payload.
 *
 * @details Read-back half of the round-trip: `ra8_io_vfs_open(read)` ->
 *          `ra8_fs_read` -> `ra8_fs_close`, then a single compound verdict on both
 *          the returned length and the byte content. The handle is closed on every
 *          path.
 *
 * @param[in] path Full "<name>:/..." VFS path to read.
 * @param[in] len  Expected byte count (the length just written).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    Length and content both matched.
 * @retval k_ra8_err_null_ptr          @p path was NULL.
 * @retval k_ra8_err_checksum_mismatch The read-back length or bytes differed.
 * @retval (other)                    The first failing VFS / ra8_fs step's code.
 *
 * @pre @p path is non-NULL and its volume is mounted.
 * @pre @p len is at most ::k_swap_payload.
 * @post ::s_readback holds the bytes read from @p path.
 * @post No file handle is left open on any return path.
 *
 * @note Not thread-safe; mutates the shared read-back buffer.
 * @since 0.1.0
 */
static ra8_err_t swap_vfs_read_verify(const char* path, uint32_t len)
{
  RA8_CHECK_NULL_PTR(path, s_tag, "path");
  RA8_CHECK_RANGE_TAG(len, 0U, (uint32_t)k_swap_payload, k_ra8_err_invalid_size, s_tag);

  ra8_fs_file_t* rf = nullptr;
  RA8_RETURN_ON_ERROR(ra8_io_vfs_open(path, k_ra8_fs_mode_read, &rf), s_tag, "open r");
  (void)memset(s_readback, 0, sizeof(s_readback));
  uint32_t        got  = 0U;
  const ra8_err_t rerr = ra8_fs_read(rf, s_readback, len, &got);
  RA8_RETURN_ON_ERROR(ra8_fs_close(rf), s_tag, "close r");
  RA8_RETURN_ON_ERROR(rerr, s_tag, "read");
  if ((got != len) || (memcmp(s_payload, s_readback, (size_t)len) != 0)) {
    return k_ra8_err_checksum_mismatch;
  }
  return k_ra8_ok;
}

/**
 * @brief Run the identical FAT/VFS round-trip over one pre-bound block device.
 *
 * @details The backend-agnostic engine, called once per ::swap_backend_t row.
 *          Bridges @p b->bd to `ra8_fs`, formats a FAT12 volume with @p b->label,
 *          mounts it, registers it in the VFS under @p b->name, writes then reads
 *          back @p len bytes at @p b->file, then unregisters and unmounts so the
 *          two shared mount slots are free for the next backend. Progress is
 *          emitted to @p log -- the SAME calls land wherever @p log is bound.
 *
 * @param[in]  b   Backend row (device handle + FAT/VFS identity).
 * @param[in]  len Payload length to round-trip (<= ::k_swap_payload).
 * @param[out] log Stream to write human-readable progress into.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           The volume mounted and the payload round-tripped.
 * @retval k_ra8_err_null_ptr @p b or @p log was NULL.
 * @retval (other)           The first failing fabric step's code.
 *
 * @pre @p b, @p b->bd, and @p log are non-NULL and the device is reachable.
 * @pre @p len is at most ::k_swap_payload.
 * @post On success @p b->file holds the verified payload, then the mount is torn
 *       down (VFS name released, ra8_fs unmounted).
 * @post No file handle or mount is left open on any return path.
 *
 * @note Not thread-safe; single-caller boot context.
 * @since 0.1.0
 */
static ra8_err_t swap_run_one(const swap_backend_t* b, uint32_t len, ra8_io_stream_t* log)
{
  RA8_CHECK_NULL_PTR(b, s_tag, "backend");
  RA8_CHECK_NULL_PTR(log, s_tag, "log");

  (void)ra8_io_stream_puts(log, "swap[");
  (void)ra8_io_stream_puts(log, b->name);
  (void)ra8_io_stream_puts(log, "]: mount ");
  (void)ra8_io_stream_puts(log, b->label);
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_as_fs_backend(b->bd, &s_be), s_tag, "fs bridge");
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat12;
  opts.label                = b->label;
  RA8_RETURN_ON_ERROR(ra8_fs_format(&s_be, &opts), s_tag, "format");
  ra8_fs_mount_t* mnt = nullptr;
  RA8_RETURN_ON_ERROR(ra8_fs_mount(&s_be, &mnt), s_tag, "mount");
  RA8_RETURN_ON_ERROR(ra8_io_vfs_mount(b->name, mnt), s_tag, "vfs mount");

  swap_fill(len);
  RA8_RETURN_ON_ERROR(swap_vfs_write(b->file, len), s_tag, "write");
  RA8_RETURN_ON_ERROR(swap_vfs_read_verify(b->file, len), s_tag, "verify");

  RA8_RETURN_ON_ERROR(ra8_io_vfs_unmount(b->name), s_tag, "vfs unmount");
  RA8_RETURN_ON_ERROR(ra8_fs_unmount(mnt), s_tag, "unmount");
  (void)ra8_io_stream_puts(log, " -> ");
  (void)ra8_io_stream_put_u32(log, len);
  (void)ra8_io_stream_puts(log, " bytes ok\r\n");
  return k_ra8_ok;
}

/**
 * @brief Replay the RAM-captured stdio out of the UART console.
 *
 * @details Reads the byte count captured by the in-RAM stdio sink and writes the
 *          captured bytes to the UART sink, bracketed by human-readable markers.
 *          This is the visible proof that the engine's identical stream calls were
 *          retargeted into RAM and can be re-emitted to a second destination.
 *
 * @param[out] out_used Receives the captured byte count on success.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           The capture was replayed and its length reported.
 * @retval k_ra8_err_null_ptr @p out_used was NULL.
 * @retval (other)           The first failing stream step's code.
 *
 * @pre @p out_used is non-NULL and ::s_ram_log captured the swap phase.
 * @pre ::s_uart is initialised.
 * @post On success `*out_used` holds the replayed byte count.
 * @post The captured bytes have been queued on the UART stream.
 *
 * @note Not thread-safe; single-caller boot context.
 * @since 0.1.0
 */
static ra8_err_t swap_replay_capture(uint32_t* out_used)
{
  RA8_CHECK_NULL_PTR(out_used, s_tag, "out_used");
  RA8_VALIDATE_INIT(s_ram_log_state.buf != nullptr, s_tag, "ram log");

  uint32_t used = 0U;
  RA8_RETURN_ON_ERROR(ra8_io_stream_ram_used(&s_ram_log_state, &used), s_tag, "used");
  swap_uart_print("ra8_io_swap_demo: --- ram-captured stdio ---\r\n");
  RA8_RETURN_ON_ERROR(ra8_io_stream_write(&s_uart, s_ram_log_buf, used, nullptr), s_tag, "replay");
  swap_uart_print("ra8_io_swap_demo: --- end capture ---\r\n");
  *out_used = used;
  return k_ra8_ok;
}

/**
 * @brief Bind both block-device backends and run the swap over the table.
 *
 * @details Binds the in-SRAM RAM device and the OSPI NOR device (the only two
 *          per-medium bring-up lines), builds the two-row ::swap_backend_t table,
 *          and runs ::swap_run_one over each row with the in-RAM stdio sink as the
 *          progress log. Stops at the first failing backend and reports which one.
 *
 * @param[out] out_failed Receives the name of the first failing backend, or NULL
 *                        when every backend passed.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    Both backends round-tripped through the identical engine.
 * @retval k_ra8_err_* The first failing bind / round-trip step's code.
 *
 * @pre ::demo_setup_or_halt has run and the stdio sinks are initialised.
 * @pre @p out_failed is non-NULL.
 * @post On success `*out_failed` is NULL and both volumes round-tripped.
 * @post On failure `*out_failed` names the backend whose round-trip failed.
 *
 * @note Not thread-safe; single-caller boot context.
 * @since 0.1.0
 */
static ra8_err_t swap_run_all(const char** out_failed)
{
  RA8_CHECK_NULL_PTR(out_failed, s_tag, "out_failed");
  *out_failed = nullptr;

  RA8_RETURN_ON_ERROR(ra8_io_blockdev_ram_init(&s_ram_bd,
                                               &s_ram_state,
                                               s_ram_disk,
                                               (uint32_t)k_swap_ram_blocks,
                                               false),
                      s_tag,
                      "ram bind");
  RA8_RETURN_ON_ERROR(ra8_xspi_init((uint8_t)k_swap_xspi_inst, k_ra8_xspi_lio_1s1s1s),
                      s_tag,
                      "xspi init");
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_xspi_init(&s_xspi_bd,
                                                &s_xspi_state,
                                                (uint8_t)k_swap_xspi_inst,
                                                (uint32_t)k_swap_xspi_base,
                                                (uint32_t)k_swap_xspi_blocks,
                                                false),
                      s_tag,
                      "xspi bind");

  const swap_backend_t table[k_swap_backends] = {
    {.bd = &s_ram_bd, .name = "ram", .label = "RAIORAM", .file = "ram:/DATA.BIN"},
    {.bd = &s_xspi_bd, .name = "xs", .label = "RAIOXS", .file = "xs:/DATA.BIN"},
  };

  for (uint32_t i = 0U; i < (uint32_t)k_swap_backends; ++i) {
    const ra8_err_t e = swap_run_one(&table[i], (uint32_t)k_swap_payload, &s_ram_log);
    if (e != k_ra8_ok) {
      *out_failed = table[i].name;
      return e;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Bring up CGC + SysTick + the SCI8 console; halt forever on any failure.
 *
 * @details Initialises clocks, reads CPU/PCLKA rates, starts the millisecond
 *          time base, routes the SCI8 console pins, and opens the SCI8 UART. Any
 *          failure spins forever so a debugger can inspect the halt (there is no
 *          console yet to report on).
 *
 * @return None.
 *
 * @pre SystemInit configured VTOR / FPU / priority grouping.
 * @pre Runs single-threaded during early boot.
 * @post On success SCI8 is open at ::k_swap_uart_baud and the time base runs.
 * @post On any failure the function never returns (infinite halt loop).
 *
 * @note Not thread-safe; boot-context only.
 * @since 0.1.0
 */
static void demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) ||
      (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) ||
      (ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, &pclka_hz) != k_ra8_ok) ||
      (ra8_time_init(cpuclk0_hz) != k_ra8_ok) ||
      (ra8_pfs_route_peripheral(k_swap_txd, k_ra8_psel_sci_async, "swap.txd") != k_ra8_ok) ||
      (ra8_pfs_route_peripheral(k_swap_rxd, k_ra8_psel_sci_async, "swap.rxd") != k_ra8_ok)) {
    while (true) {
    }
  }
  const ra8_sci_cfg_t sci_cfg = {.baud      = (uint32_t)k_swap_uart_baud,
                                 .data_bits = k_ra8_sci_data_8,
                                 .parity    = k_ra8_sci_parity_none,
                                 .stop_bits = k_ra8_sci_stop_1,
                                 .pclk_hz   = pclka_hz};
  if (ra8_sci_init((uint8_t)k_swap_uart_chan, &sci_cfg) != k_ra8_ok) {
    while (true) {
    }
  }
}

/**
 * @brief Firmware entry point.
 *
 * @details Brings up the console and both stdio sinks, retargets the engine's
 *          stdio to the in-RAM capture sink, runs the two-backend swap, replays
 *          the RAM capture out of the UART, and prints a single PASS/FAIL verdict
 *          per abstraction before parking in an infinite loop.
 *
 * @return Never returns.
 * @retval (none) The function does not return (final `while (true)`).
 *
 * @pre SystemInit configured VTOR / FPU / priority grouping.
 * @pre The OSPI NOR array is present (modelled in board_sim, real on silicon).
 * @post A PASS or FAIL verdict line has been queued on SCI8 for each abstraction.
 * @post Control parks in an infinite loop; the function never returns.
 *
 * @note Single-threaded; runs to the park loop on the main stack.
 * @since 0.1.0
 */
int main(void)
{
  ra8_log_init();
  demo_setup_or_halt();
  (void)ra8_io_stream_uart_init(&s_uart, &s_uart_state, (uint8_t)k_swap_uart_chan);
  (void)ra8_io_log_attach(&s_uart); /* route ra8_log errors onto the UART too */
  swap_uart_print("ra8_io_swap_demo: boot\r\n");

  /* Retarget the engine's stdio into the in-RAM capture sink. */
  (void)
    ra8_io_stream_ram_init(&s_ram_log, &s_ram_log_state, s_ram_log_buf, (uint32_t)k_swap_log_cap);

  const char*     failed = nullptr;
  const ra8_err_t swap_e = swap_run_all(&failed);

  uint32_t        captured = 0U;
  const ra8_err_t rep_e    = swap_replay_capture(&captured);

  if (swap_e == k_ra8_ok) {
    swap_uart_print("ra8_io_swap_demo: two-backend swap (ram + xs) PASS\r\n");
  } else if (failed != nullptr) {
    swap_uart_print("ra8_io_swap_demo: backend ");
    swap_uart_print(failed);
    swap_uart_print(" FAIL\r\n");
  } else {
    swap_uart_print("ra8_io_swap_demo: swap FAIL\r\n");
  }

  if ((rep_e == k_ra8_ok) && (captured > 0U)) {
    swap_uart_print("ra8_io_swap_demo: ram-stream capture ");
    (void)ra8_io_stream_put_u32(&s_uart, captured);
    swap_uart_print(" bytes PASS\r\n");
  } else {
    swap_uart_print("ra8_io_swap_demo: ram-stream capture FAIL\r\n");
  }

  (void)ra8_sci_flush((uint8_t)k_swap_uart_chan);
  while (true) {
  }
}
