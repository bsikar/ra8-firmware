/**
 * @file emu_console.c
 * @brief Emulator console surfaces implementation (see emu_console.h)
 *
 * @details
 * The `[uart] SCIn:` line assembly fed by the SCI TX sink, the ITM/SWO
 * stimulus-port seed + echo (`[itm]` lines), and the CLI escape decoder --
 * moved verbatim out of the ra8_emulator main translation unit.
 *
 * `[itm] ...` lines are ra8_emulator's echo of the Arm CoreSight ITM stimulus
 * port 0. It is the direct analog of the `[uart] SCI8:` echo: where that
 * surfaces the firmware's UART console, this surfaces ra8_log's debug trace --
 * the same bytes that on real hardware leave through the ITM/SWO pin to the
 * J-Link SWO console. So `[itm]` == "what you would see on the SWO trace
 * console", and `[uart] SCI8:` == "what you would see on the serial console".
 *
 * ra8_log writes log bytes to ITM stimulus port 0 (0xE0000000) after checking
 * DEMCR.TRCENA + ITM TCR/TENR + a non-zero STIM0 "FIFO ready" read. With no
 * debugger attached those PPB bytes are all zero, so internal_itm_ready()
 * returns false and every byte is dropped -- which is why the e-reader (and
 * any ra8_log user) printed nothing in ra8_emulator. We seed the ready bits
 * into PPB RAM at boot (internal_itm_seed_ready: sets DEMCR.TRCENA + TCR.ITMENA + TENR
 * port 0 + a ready STIM0) and hook stimulus-port writes (internal_on_itm_stim_write) to
 * echo the bytes as `[itm] <line>` on injected output sink. This surfaces
 * ra8_log for every app, not just the TrustZone e-reader.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "emu_console.h"

#include <stdio.h>

#include "board_console.h"
#include "emu_host_io_internal.h"
#include "emu_memory_access.h"

/** @brief Accumulated current ITM line (flushed on newline or when full). */
static char s_itm_line[k_itm_line_max + 1U];

/** @brief Bytes currently buffered in ::s_itm_line. */
static uint32_t s_itm_len;

static char     s_uart_line[k_uart_line_max]; /**< Pending [uart] line text.   */
static uint32_t s_uart_line_len;              /**< Chars buffered in the line. */

/**
 * @brief UC_HOOK_MEM_WRITE handler for ITM stimulus port 0 -- echo the byte.
 *
 * @details Buffers the low byte of each stimulus write and prints `[itm]
 * <line>` to injected output sink on a newline (or when the line buffer fills),
 * so ra8_log output is visible in the emulator. Carriage returns are dropped so
 *          the `\r\n` ra8_log line ending yields one clean line.
 *
 * @param[in] uc    Unicorn engine (unused; the byte rides in @p value).
 * @param[in] type  Memory access type (write); unused.
 * @param[in] addr  Observed address (the stimulus port); unused.
 * @param[in] size  Access width in bytes; unused.
 * @param[in] value The value being written; its low byte is the log character.
 * @param[in] user  Hook user pointer; unused.
 * @return Nothing.
 *
 * @pre The hook is registered for the 4-byte STIM0 word only.
 * @pre @p value holds the character ra8_log is emitting.
 * @post On a newline the buffered line is printed and the buffer reset.
 * @post Non-newline printable bytes are appended to @ref s_itm_line.
 * @note Not thread-safe; ra8_emulator is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_on_itm_stim_write(uc_engine*  uc,
                                                    uc_mem_type type,
                                                    uint64_t    addr,
                                                    int         size,
                                                    int64_t     value,
                                                    void*       user)
{
  (void)uc;
  (void)type;
  (void)addr;
  (void)size;
  (void)user;
  const char c = (char)((uint32_t)value & 0xFFU); /* MAGIC-OK: low-byte mask */
  if (c == '\r') {
    return;
  }
  if ((c == '\n') || (s_itm_len >= (uint32_t)k_itm_line_max)) {
    s_itm_line[s_itm_len] = '\0';
    /* Emit one ra8_log line as `[itm] <line>` -- the CoreSight ITM/SWO-trace
     * analog of the `[uart] SCI8:` console echo (see the ITM model block
     * above). */
    (void)priv_emu_io_outf("[itm] %s\n", s_itm_line);
    /* Also route it to the board_console ITM channel so the tabbed board-view
     * console shows ITM/SWO trace in its own tab (a different endpoint than the
     * UART line). The injected output sink echo above is unchanged -- this is
     * purely additive. */
    board_console_push(k_board_console_ch_itm, s_itm_line);
    s_itm_len = 0U;
    if (c == '\n') {
      return;
    }
  }
  s_itm_line[s_itm_len] = c;
  s_itm_len++;
}

/**
 * @brief Seed the ITM "ready" bits into PPB RAM so ra8_log emits.
 *
 * @details On hardware a debugger sets DEMCR.TRCENA and enables the ITM; with
 *          none attached those PPB registers read zero and ra8_log drops every
 *          byte. ra8_emulator maps the PPB as plain RAM, so writing the enable
 * bits here makes internal_itm_ready() see a live ITM. The firmware only ever
 *          reads these registers (it never re-disables the ITM), so the seed
 *          persists for the run.
 *
 * @param[in] uc Initialised Unicorn engine with the PPB region mapped.
 * @return Nothing.
 *
 * @pre @p uc has the PPB region (0xE0000000) mapped as RAM.
 * @pre Called once before emulation starts.
 * @post DEMCR.TRCENA, ITM TCR.ITMENA, TENR port-0, and a ready STIM0 are set.
 * @post ra8_log's internal_itm_ready() returns true for the run.
 * @note Not thread-safe; call during single-threaded setup.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_itm_seed_ready(uc_engine* uc)
{
  const uint32_t demcr = (uint32_t)k_scb_demcr_trcena;
  const uint32_t tcr   = (uint32_t)k_itm_tcr_itmena;
  const uint32_t tenr  = (uint32_t)k_itm_tenr_port0;
  const uint32_t stim  = (uint32_t)k_itm_stim_ready;
  (void)emu_mem_write(uc, (uint64_t)k_scb_demcr_addr, &demcr, sizeof(demcr));
  (void)emu_mem_write(uc, (uint64_t)k_itm_tcr_addr, &tcr, sizeof(tcr));
  (void)emu_mem_write(uc, (uint64_t)k_itm_tenr_addr, &tenr, sizeof(tenr));
  (void)emu_mem_write(uc, (uint64_t)k_itm_stim0_addr, &stim, sizeof(stim));
}

/** @brief Implementation of `emu_console_install()` -- ITM seed + STIM0 echo
 * hook. */
void emu_console_install(uc_engine* uc)
{
  internal_itm_seed_ready(uc);
  static uc_hook s_h_itm;
  (void)uc_hook_add(uc,
                    &s_h_itm,
                    UC_HOOK_MEM_WRITE,
                    (void*)internal_on_itm_stim_write,
                    nullptr,
                    (uint64_t)k_itm_stim0_addr,
                    (uint64_t)k_itm_stim0_addr + 3U);
}

void console_flush_line(uint8_t channel)
{
  if (s_uart_line_len == 0U) {
    return;
  }
  s_uart_line[s_uart_line_len] = '\0';
  (void)priv_emu_io_outf("[uart] SCI%u: %s\n", channel, s_uart_line);
  s_uart_line_len = 0U;
}

void console_tx_sink(uint8_t channel, uint8_t byte)
{
  if (byte == (uint8_t)'\n') {
    console_flush_line(channel);
    return;
  }
  if ((byte != (uint8_t)'\r') && (s_uart_line_len < (uint32_t)(k_uart_line_max - 1U))) {
    s_uart_line[s_uart_line_len++] = (char)byte;
  }
  if (s_uart_line_len == (uint32_t)(k_uart_line_max - 1U)) {
    console_flush_line(channel); /* avoid an unbounded line on a stream with no LF */
  }
}

uint32_t decode_escapes(const char* in, uint8_t* out, uint32_t cap)
{
  uint32_t n = 0U;
  for (uint32_t i = 0U; (in[i] != '\0') && (n < cap); i++) {
    char c = in[i];
    if ((c == '\\') && (in[i + 1U] != '\0')) {
      i++;
      switch (in[i]) {
        case 'n':
          c = '\n';
          break;
        case 'r':
          c = '\r';
          break;
        case 't':
          c = '\t';
          break;
        case '0':
          c = '\0';
          break;
        default:
          c = in[i]; /* \\ -> \, and any other escape is taken literally */
          break;
      }
    }
    out[n++] = (uint8_t)c;
  }
  return n;
}

/** @brief Implementation of `emu_console_reset()` -- drop the pending ITM line.
 */
void emu_console_reset(void)
{
  s_itm_len = 0U;
}
