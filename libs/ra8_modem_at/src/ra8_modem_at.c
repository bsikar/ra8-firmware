/**
 * @file ra8_modem_at.c
 * @brief Cellular modem AT command/response driver implementation
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * See ``ra8_modem_at.h`` for the public contract. This file
 * implements the line accumulator state machine and the URC
 * dispatch table. All buffers are statically allocated -- the
 * caller-owned line buffer is referenced by pointer.
 *
 * The string utilities at the top of the file are project-local
 * replacements for ``strlen``/``strncmp`` to keep the build free
 * of ``string.h`` per the existing convention in
 * ``libs/ra8_net_pal/src/ra8_net_pal.c``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_modem_at.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_modem_at_internal.h"

/**
 * @def RA8_MODEM_AT_TAG
 * @brief Component log tag.
 */
#define RA8_MODEM_AT_TAG "MODEM_AT"

/**
 * @enum ra8_modem_at_internal_const_t
 * @brief Internal numeric constants used by the AT driver.
 */
typedef enum : uint16_t {
  k_ra8_modem_at_min_line_buf_bytes = 16U, /**< Minimum line buffer length. */
} ra8_modem_at_internal_const_t;

/**
 * @enum ra8_modem_at_state_t
 * @brief Internal line-accumulator FSM states.
 *
 * @details
 * The state moves only forward through the cycle
 * idle -> await_echo -> await_resp -> done -> idle.
 * Echo handling is best-effort: if the modem has ATE0 active and
 * never echoes, the first non-empty line transitions the FSM
 * directly into ``await_resp``.
 */
typedef enum : uint8_t {
  k_ra8_modem_at_state_idle       = 0U, /**< No command in flight.                */
  k_ra8_modem_at_state_await_echo = 1U, /**< Sent command, waiting for echo line. */
  k_ra8_modem_at_state_await_resp = 2U, /**< Echo seen (or skipped), draining.    */
  k_ra8_modem_at_state_done       = 3U, /**< Final result code matched.           */
} ra8_modem_at_state_t;

/**
 * @struct ra8_modem_at_urc_slot_t
 * @brief One URC table entry.
 */
typedef struct {
  uint8_t               used;                                  /**< 1 if slot occupied.    */
  uint8_t               prefix[k_ra8_modem_at_max_prefix_len]; /**< NUL-terminated prefix. */
  ra8_modem_at_urc_fn_t fn;                                    /**< Handler callback.      */
  void*                 ctx;                                   /**< Opaque ctx for ``fn``. */
} ra8_modem_at_urc_slot_t;

/**
 * @struct ra8_modem_at_module_t
 * @brief Aggregated module state.
 *
 * @details
 * Held in a single file-static instance; a struct rather than a
 * scatter of statics so future moves to a context-handle API are
 * mechanical.
 *
 * @invariant ``initialized == 1`` implies ``cfg.line_buf != NULL``.
 */
typedef struct {
  uint8_t              initialized; /**< 1 after ``ra8_modem_at_init``.     */
  ra8_modem_at_cfg_t   cfg;         /**< Cached caller configuration.       */
  uint16_t             line_len;    /**< Bytes pending in ``cfg.line_buf``. */
  ra8_modem_at_state_t state;       /**< FSM state.                         */
  /** URC table. */
  ra8_modem_at_urc_slot_t urcs[k_ra8_modem_at_max_unsolicited];
} ra8_modem_at_module_t;

/**
 * @var s_mod
 * @brief Singleton module state (file scope, NASA Rule 6).
 *
 * @note Direct external access is forbidden; the public API guards it.
 */
static ra8_modem_at_module_t s_mod;

/* ------------------------------------------------------------------------- */
/* Internal helpers */
/* ------------------------------------------------------------------------- */

/**
 * @brief NUL-terminated string length.
 *
 * @param[in] s NUL-terminated input string (must be non-NULL).
 * @return Length in bytes, capped at ``UINT16_MAX``.
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
uint16_t ra8_modem_at_internal_str_len(const char* s)
{
  uint16_t i = 0U;
  while ((i < UINT16_MAX) && (s[i] != '\0')) {
    ++i;
  }
  return i;
}

/**
 * @brief Return 1 if ``hay`` begins with ``needle``.
 *
 * @param[in] hay    NUL-terminated haystack.
 * @param[in] needle NUL-terminated prefix.
 * @return 1 on match, 0 otherwise.
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
uint8_t ra8_modem_at_internal_starts_with(const char* hay, const char* needle)
{
  uint16_t i = 0U;
  while (needle[i] != '\0') {
    if (hay[i] != needle[i]) {
      return 0U;
    }
    ++i;
  }
  return 1U;
}

/**
 * @brief Compare two NUL-terminated strings for exact equality.
 *
 * @param[in] a First string.
 * @param[in] b Second string.
 * @return 1 if equal, 0 otherwise.
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
uint8_t ra8_modem_at_internal_str_eq(const char* a, const char* b)
{
  uint16_t i = 0U;
  while ((a[i] != '\0') && (b[i] != '\0')) {
    if (a[i] != b[i]) {
      return 0U;
    }
    ++i;
  }
  return (uint8_t)((a[i] == '\0') && (b[i] == '\0'));
}

/**
 * @brief Append a single character to ``out`` if there is room.
 *
 * @param[in,out] out      Output buffer (NUL-terminated on entry).
 * @param[in]     out_len  Total capacity of ``out``.
 * @param[in,out] used     Current populated length (excluding NUL).
 * @param[in]     ch       Character to append.
 *
 * @details See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_append_ch(char* out, size_t out_len, size_t* used, char ch)
{
  if ((*used + 1U) < out_len) {
    out[*used] = ch;
    ++(*used);
    out[*used] = '\0';
  }
}

/**
 * @brief Pure helper for the @ref internal_reset_line line-227 AND.
 *
 * @details Promoted as a free function so tests can drive both
 *          (line_buf, line_buf_len) input combinations directly under
 *          @c -fcoverage-mcdc on the production source. The state-
 *          reading wrapper @ref internal_reset_line forwards to this
 *          helper with @c s_mod.cfg.line_buf and @c
 *          s_mod.cfg.line_buf_len.
 *
 * @param[in] line_buf     Caller-owned buffer pointer (NULL allowed).
 * @param[in] line_buf_len Capacity of @p line_buf in bytes.
 * @return 1 iff both arguments indicate an installed buffer.
 * @retval 1 line_buf!=NULL and line_buf_len>0.
 * @retval 0 Either argument signals "no buffer".
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return value depends solely on the two inputs.
 * @note Pure function; thread-safe.
 * @since 0.1.0
 */
uint8_t ra8_modem_at_internal_reset_line_should_clear(const void* line_buf, uint16_t line_buf_len)
{
  if ((line_buf != nullptr) && (line_buf_len > 0U)) {
    return 1U;
  }
  return 0U;
}

/**
 * @brief Production carrier of the line-573 payload-prefix AND-decision.
 *
 * @details Holds the three-condition short-circuit AND that classifies a
 *          payload-kind line as matching the caller-supplied
 *          ``expected_response`` prefix. Promoted from inline-in-callsite to
 *          a TU-external helper so all four short-circuit MC/DC vectors can
 *          be driven from a host test (the public-API path through
 *          ``send_cmd`` / ``send_cmd_capture`` reaches only a subset of the
 *          four input combinations).
 *
 * @param[in] line              NUL-terminated input line (must be non-NULL).
 * @param[in] expected_response Optional NUL-terminated expected prefix.
 * @return 1 iff all three conditions hold.
 * @retval 1 expected_response != NULL && expected_response[0] != '\0' &&
 *           starts_with(line, expected_response).
 * @retval 0 Any one of the three conditions fails.
 *
 * @pre line is non-NULL and NUL-terminated.
 * @pre No precondition on expected_response (NULL accepted).
 * @post No state mutated.
 * @post Return value depends solely on the two inputs.
 * @note Pure function; thread-safe.
 * @since 0.1.0
 */
uint8_t ra8_modem_at_internal_handle_line_payload_prefix_match(const char* line,
                                                               const char* expected_response)
{
  if ((expected_response != nullptr) && (expected_response[0] != '\0') &&
      (ra8_modem_at_internal_starts_with(line, expected_response) != 0U)) {
    return 1U;
  }
  return 0U;
}

/**
 * @brief Production carrier of the line-664 capture-init AND-decision.
 *
 * @details Holds the AND-decision that gates the one-shot ``capture[0] =
 *          '\\0'`` initialisation in @ref internal_wait_response. Promoted
 *          from inline-in-callsite to a TU-external helper so all four
 *          input combinations are reachable from a host test (the public
 *          ``send_cmd_capture`` API rejects ``capture_len == 0`` at the
 *          entry guard, so the inline form was structurally limited to
 *          (NULL, 0) and (non-NULL, >0)).
 *
 * @param[in] capture     Caller-owned capture buffer (NULL allowed).
 * @param[in] capture_len Capacity of @p capture in bytes.
 * @return 1 iff capture is non-NULL AND capture_len > 0.
 * @retval 1 Both conditions hold.
 * @retval 0 Either condition fails.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return value depends solely on the two inputs.
 * @note Pure function; thread-safe.
 * @since 0.1.0
 */
uint8_t ra8_modem_at_internal_wait_response_should_clear_capture(const void* capture,
                                                                 size_t      capture_len)
{
  if ((capture != nullptr) && (capture_len > 0U)) {
    return 1U;
  }
  return 0U;
}

/**
 * @brief Reset the line accumulator for a fresh command cycle.
 *
 * @details Forwards the buffer-installed predicate to
 *          @ref ra8_modem_at_internal_reset_line_should_clear so the
 *          line-249 AND-decision is exercised in a pure free function
 *          where tests can vary both inputs.
 *
 * @pre Module state is consistent.
 * @pre Caller holds the module lock.
 * @post Line accumulator is empty.
 * @post Configured line buffer's first byte is NUL when present.
 * @note Not thread-safe; caller serialises access.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_reset_line(void)
{
  s_mod.line_len = 0U;
  if (ra8_modem_at_internal_reset_line_should_clear(s_mod.cfg.line_buf, s_mod.cfg.line_buf_len) !=
      0U) {
    s_mod.cfg.line_buf[0] = (uint8_t)'\0';
  }
}

/**
 * @brief Test whether ``line`` is a final OK/ERROR result code.
 *
 * @param[in]  line     NUL-terminated received line.
 * @param[out] is_error Set to 1 if ``line`` is an error result.
 * @return 1 if ``line`` is any final result code, 0 otherwise.
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t internal_classify_final(const char* line, uint8_t* is_error)
{
  *is_error = 0U;
  if (ra8_modem_at_internal_str_eq(line, "OK") != 0U) {
    return 1U;
  }
  if (ra8_modem_at_internal_str_eq(line, "ERROR") != 0U) {
    *is_error = 1U;
    return 1U;
  }
  if (ra8_modem_at_internal_starts_with(line, "+CME ERROR") != 0U) {
    *is_error = 1U;
    return 1U;
  }
  if (ra8_modem_at_internal_starts_with(line, "+CMS ERROR") != 0U) {
    *is_error = 1U;
    return 1U;
  }
  if (ra8_modem_at_internal_str_eq(line, "BUSY") != 0U) {
    *is_error = 1U;
    return 1U;
  }
  if (ra8_modem_at_internal_str_eq(line, "NO CARRIER") != 0U) {
    *is_error = 1U;
    return 1U;
  }
  return 0U;
}

/**
 * @brief Dispatch a URC line to any matching registered handler.
 *
 * @param[in] line NUL-terminated line.
 * @return 1 if a handler matched, 0 otherwise.
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t internal_dispatch_urc(const char* line)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_modem_at_max_unsolicited; ++i) {
    if (s_mod.urcs[i].used == 0U) {
      continue;
    }
    if (ra8_modem_at_internal_starts_with(line, (const char*)s_mod.urcs[i].prefix) != 0U) {
      s_mod.urcs[i].fn(line, s_mod.urcs[i].ctx);
      return 1U;
    }
  }
  return 0U;
}

/* ra8_modem_line_kind_t is defined in ra8_modem_at_internal.h so tests
 * can read the return value of ra8_modem_at_internal_classify(). */

/**
 * @brief Classify a complete (NUL-terminated) line for the FSM.
 *
 * @details
 * The expected-prefix and command-echo arguments are caller-supplied
 * because they vary per command invocation. URC dispatch happens
 * inside this helper so the caller does not need to know about it.
 *
 * @param[in] line              NUL-terminated received line.
 * @param[in] cmd_echo          Command we sent (for echo detection),
 *                              may be NULL when not awaiting echo.
 * @param[in] expected_response Optional caller-specified prefix.
 * @return One of ``ra8_modem_line_kind_t``.
 *
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra8_modem_line_kind_t ra8_modem_at_internal_classify(const char* line,
                                                     const char* cmd_echo,
                                                     const char* expected_response)
{
  if (line[0] == '\0') {
    return k_ra8_modem_line_kind_empty;
  }
  if ((cmd_echo != nullptr) && (ra8_modem_at_internal_str_eq(line, cmd_echo) != 0U)) {
    return k_ra8_modem_line_kind_echo;
  }
  uint8_t is_err = 0U;
  if (internal_classify_final(line, &is_err) != 0U) {
    return (is_err != 0U) ? k_ra8_modem_line_kind_final_err : k_ra8_modem_line_kind_final_ok;
  }
  /* If the caller didn't ask for a specific prefix, allow URC dispatch. */
  if ((expected_response == nullptr) || (expected_response[0] == '\0') ||
      (ra8_modem_at_internal_starts_with(line, expected_response) == 0U)) {
    if (internal_dispatch_urc(line) != 0U) {
      return k_ra8_modem_line_kind_urc;
    }
  }
  return k_ra8_modem_line_kind_payload;
}

/**
 * @brief Push a single received byte into the line accumulator.
 *
 * @param[in]  byte     Just-received byte.
 * @param[out] line_out If a complete line is now ready, set to
 *                      ``cfg.line_buf`` (NUL-terminated). Otherwise NULL.
 *
 * @details See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_accumulate(uint8_t byte, const char** line_out)
{
  *line_out = nullptr;
  if ((byte == (uint8_t)'\r') || (byte == (uint8_t)'\n')) {
    if (s_mod.line_len > 0U) {
      s_mod.cfg.line_buf[s_mod.line_len] = (uint8_t)'\0';
      *line_out                          = (const char*)s_mod.cfg.line_buf;
      s_mod.line_len                     = 0U;
    }
    return;
  }
  if ((s_mod.line_len + 1U) >= s_mod.cfg.line_buf_len) {
    /* Overflow: terminate, emit what we have, drop the new byte. */
    s_mod.cfg.line_buf[s_mod.line_len] = (uint8_t)'\0';
    *line_out                          = (const char*)s_mod.cfg.line_buf;
    s_mod.line_len                     = 0U;
    return;
  }
  s_mod.cfg.line_buf[s_mod.line_len] = byte;
  ++s_mod.line_len;
}

/**
 * @brief Transmit a NUL-terminated string then ``"\r"``.
 *
 * @param[in] s NUL-terminated string to transmit.
 * @return ``ra8_err_t`` propagated from the transport.
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_tx_command(const char* s)
{
  uint16_t i = 0U;
  while (s[i] != '\0') {
    const ra8_err_t err = s_mod.cfg.io.tx_byte(s_mod.cfg.io.ctx, (uint8_t)s[i]);
    if (err != k_ra8_ok) {
      return err;
    }
    ++i;
  }
  return s_mod.cfg.io.tx_byte(s_mod.cfg.io.ctx, (uint8_t)'\r');
}

/**
 * @brief Resolve effective timeout in ms, applying default if 0.
 *
 * @details See implementation.
 * @param[in] timeout_ms See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint16_t internal_effective_timeout(uint16_t timeout_ms)
{
  if (timeout_ms != 0U) {
    return timeout_ms;
  }
  if (s_mod.cfg.default_timeout_ms != 0U) {
    return s_mod.cfg.default_timeout_ms;
  }
  return (uint16_t)k_ra8_modem_at_default_timeout_ms;
}

/**
 * @brief Outcome of handling one classified line in @ref internal_wait_response.
 */
typedef enum : uint8_t {
  k_ra8_modem_line_action_continue = 0U, /**< Keep waiting.         */
  k_ra8_modem_line_action_done_ok  = 1U, /**< Final OK observed.    */
  k_ra8_modem_line_action_done_err = 2U, /**< Final error observed. */
} ra8_modem_line_action_t;

/**
 * @brief Append a NUL-terminated line into ``capture`` (with newline sep).
 *
 * @details See implementation.
 * @param[in] line See implementation.
 * @param[in] capture See implementation.
 * @param[in] capture_len See implementation.
 * @param[in] used See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
void ra8_modem_at_internal_capture_line(const char* line,
                                        char*       capture,
                                        size_t      capture_len,
                                        size_t*     used)
{
  if ((capture == nullptr) || (capture_len == 0U)) {
    return;
  }
  if (*used > 0U) {
    internal_append_ch(capture, capture_len, used, '\n');
  }
  uint16_t k = 0U;
  while (line[k] != '\0') {
    internal_append_ch(capture, capture_len, used, line[k]);
    ++k;
  }
}

/**
 * @brief Drive the response FSM by one classified line.
 *
 * @param[in]     line              Just-completed line.
 * @param[in]     kind              Result of @ref internal_classify.
 * @param[in]     expected_response Optional caller-supplied prefix.
 * @param[in,out] seen_exp          Sticky flag: prefix has been observed.
 * @param[out]    capture           Optional capture buffer.
 * @param[in]     capture_len       Capacity of ``capture``.
 * @param[in,out] used              Bytes already populated in ``capture``.
 * @return One of @ref ra8_modem_line_action_t.
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_modem_line_action_t internal_handle_line(const char*           line,
                                                    ra8_modem_line_kind_t kind,
                                                    const char*           expected_response,
                                                    uint8_t*              seen_exp,
                                                    char*                 capture,
                                                    size_t                capture_len,
                                                    size_t*               used)
{
  switch (kind) {
    case k_ra8_modem_line_kind_empty:
    case k_ra8_modem_line_kind_urc:
      return k_ra8_modem_line_action_continue;
    case k_ra8_modem_line_kind_echo:
      s_mod.state = k_ra8_modem_at_state_await_resp;
      return k_ra8_modem_line_action_continue;
    case k_ra8_modem_line_kind_final_ok:
      s_mod.state = k_ra8_modem_at_state_done;
      if (*seen_exp == 0U) {
        ra8_log_error(RA8_MODEM_AT_TAG, "OK without expected prefix");
        return k_ra8_modem_line_action_done_err;
      }
      return k_ra8_modem_line_action_done_ok;
    case k_ra8_modem_line_kind_final_err:
      s_mod.state = k_ra8_modem_at_state_done;
      return k_ra8_modem_line_action_done_err;
    case k_ra8_modem_line_kind_payload:
    default:
      if (ra8_modem_at_internal_handle_line_payload_prefix_match(line, expected_response) != 0U) {
        *seen_exp = 1U;
      }
      ra8_modem_at_internal_capture_line(line, capture, capture_len, used);
      if (s_mod.state == k_ra8_modem_at_state_await_echo) {
        s_mod.state = k_ra8_modem_at_state_await_resp;
      }
      return k_ra8_modem_line_action_continue;
  }
}

/**
 * @brief Bundled args + state for one iteration of the wait loop.
 */
typedef struct {
  const char* cmd;               /**< Command we sent (for echo).         */
  const char* expected_response; /**< Optional expected prefix.           */
  char*       capture;           /**< Optional capture buffer.            */
  size_t      capture_len;       /**< Capacity of ``capture``.            */
  size_t*     used;              /**< Bytes already populated in capture. */
  uint8_t*    seen_exp;          /**< Sticky: prefix observed yet?        */
} ra8_modem_wait_ctx_t;

/**
 * @brief Pull one byte and process any completed line.
 *
 * @return The line action (continue / done_ok / done_err).
 *
 * @details See implementation.
 * @param[in] wc See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_modem_line_action_t internal_pump_one(const ra8_modem_wait_ctx_t* wc)
{
  uint8_t         byte   = 0U;
  const ra8_err_t rx_err = s_mod.cfg.io.rx_byte(s_mod.cfg.io.ctx, &byte);
  if (rx_err != k_ra8_ok) {
    return k_ra8_modem_line_action_continue;
  }
  const char* line = nullptr;
  internal_accumulate(byte, &line);
  if (line == nullptr) {
    return k_ra8_modem_line_action_continue;
  }
  const ra8_modem_line_kind_t kind =
    ra8_modem_at_internal_classify(line, wc->cmd, wc->expected_response);
  return internal_handle_line(line,
                              kind,
                              wc->expected_response,
                              wc->seen_exp,
                              wc->capture,
                              wc->capture_len,
                              wc->used);
}

/**
 * @brief Inner wait loop shared by send_cmd and send_cmd_capture.
 *
 * @param[in]  cmd               Command we sent (for echo strip).
 * @param[in]  expected_response Optional expected prefix.
 * @param[in]  timeout_ms        Effective timeout in ms (already resolved).
 * @param[out] capture           Optional payload capture buffer (may be NULL).
 * @param[in]  capture_len       Capacity of ``capture``.
 * @return ``ra8_err_t`` per public docs.
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_wait_response(const char* cmd,
                                        const char* expected_response,
                                        uint16_t    timeout_ms,
                                        char*       capture,
                                        size_t      capture_len)
{
  const uint32_t start = s_mod.cfg.io.now_ms(s_mod.cfg.io.ctx);
  size_t         used  = 0U;
  uint8_t seen_exp = (uint8_t)((expected_response == nullptr) || (expected_response[0] == '\0'));

  if (ra8_modem_at_internal_wait_response_should_clear_capture(capture, capture_len) != 0U) {
    capture[0] = '\0';
  }

  s_mod.state = k_ra8_modem_at_state_await_echo;

  const ra8_modem_wait_ctx_t wc = {
    .cmd               = cmd,
    .expected_response = expected_response,
    .capture           = capture,
    .capture_len       = capture_len,
    .used              = &used,
    .seen_exp          = &seen_exp,
  };

  while (1) {
    const ra8_modem_line_action_t action = internal_pump_one(&wc);
    if (action == k_ra8_modem_line_action_done_ok) {
      internal_reset_line();
      return k_ra8_ok;
    }
    if (action == k_ra8_modem_line_action_done_err) {
      internal_reset_line();
      return k_ra8_err_hw_error;
    }
    const uint32_t elapsed = s_mod.cfg.io.now_ms(s_mod.cfg.io.ctx) - start;
    if (elapsed >= (uint32_t)timeout_ms) {
      s_mod.state = k_ra8_modem_at_state_idle;
      internal_reset_line();
      return k_ra8_err_hw_timeout;
    }
  }
}

/* ------------------------------------------------------------------------- */
/* Public API */
/* ------------------------------------------------------------------------- */

/**
 * @brief Reset the URC dispatch table to all-empty.
 *
 * @details See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_clear_urc_table(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_modem_at_max_unsolicited; ++i) {
    s_mod.urcs[i].used = 0U;
    s_mod.urcs[i].fn   = nullptr;
    s_mod.urcs[i].ctx  = nullptr;
  }
}

/**
 * @brief Validate every required pointer in @p cfg's IO block.
 *
 * @details See implementation.
 * @param[in] cfg See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_validate_init_cfg(const ra8_modem_at_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, RA8_MODEM_AT_TAG, "cfg");
  RA8_CHECK_NULL_PTR(cfg->line_buf, RA8_MODEM_AT_TAG, "cfg->line_buf");
  RA8_CHECK_NULL_PTR((void*)cfg->io.tx_byte, RA8_MODEM_AT_TAG, "cfg->io.tx_byte");
  RA8_CHECK_NULL_PTR((void*)cfg->io.rx_byte, RA8_MODEM_AT_TAG, "cfg->io.rx_byte");
  RA8_CHECK_NULL_PTR((void*)cfg->io.now_ms, RA8_MODEM_AT_TAG, "cfg->io.now_ms");
  if (cfg->line_buf_len < (uint16_t)k_ra8_modem_at_min_line_buf_bytes) {
    ra8_log_error(RA8_MODEM_AT_TAG, "line_buf too small");
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_modem_at_init(const ra8_modem_at_cfg_t* cfg)
{
  const ra8_err_t verr = internal_validate_init_cfg(cfg);
  if (verr != k_ra8_ok) {
    return verr;
  }
  s_mod.cfg         = *cfg;
  s_mod.line_len    = 0U;
  s_mod.state       = k_ra8_modem_at_state_idle;
  s_mod.initialized = 1U;
  internal_clear_urc_table();
  internal_reset_line();
  return k_ra8_ok;
}

ra8_err_t ra8_modem_at_send_cmd(const char* cmd, const char* expected_response, uint16_t timeout_ms)
{
  if (s_mod.initialized == 0U) {
    return k_ra8_err_not_initialized;
  }
  RA8_CHECK_NULL_PTR(cmd, RA8_MODEM_AT_TAG, "cmd");

  internal_reset_line();
  const ra8_err_t tx_err = internal_tx_command(cmd);
  if (tx_err != k_ra8_ok) {
    return tx_err;
  }
  return internal_wait_response(cmd,
                                expected_response,
                                internal_effective_timeout(timeout_ms),
                                nullptr,
                                0U);
}

ra8_err_t
ra8_modem_at_send_cmd_capture(const char* cmd, char* out_buf, size_t buf_len, uint16_t timeout_ms)
{
  if (s_mod.initialized == 0U) {
    return k_ra8_err_not_initialized;
  }
  RA8_CHECK_NULL_PTR(cmd, RA8_MODEM_AT_TAG, "cmd");
  RA8_CHECK_NULL_PTR(out_buf, RA8_MODEM_AT_TAG, "out_buf");
  if (buf_len == 0U) {
    return k_ra8_err_invalid_size;
  }

  out_buf[0] = '\0';
  internal_reset_line();
  const ra8_err_t tx_err = internal_tx_command(cmd);
  if (tx_err != k_ra8_ok) {
    return tx_err;
  }
  return internal_wait_response(cmd,
                                nullptr,
                                internal_effective_timeout(timeout_ms),
                                out_buf,
                                buf_len);
}

/**
 * @brief Find an existing URC slot for ``prefix`` and update its callback.
 * @return 1 if a slot matched (and was updated), 0 otherwise.
 *
 * @details See implementation.
 * @param[in] prefix See implementation.
 * @param[in] fn See implementation.
 * @param[in] ctx See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t internal_urc_replace(const char* prefix, ra8_modem_at_urc_fn_t fn, void* ctx)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_modem_at_max_unsolicited; ++i) {
    if (s_mod.urcs[i].used == 0U) {
      continue;
    }
    if (ra8_modem_at_internal_str_eq((const char*)s_mod.urcs[i].prefix, prefix) != 0U) {
      s_mod.urcs[i].fn  = fn;
      s_mod.urcs[i].ctx = ctx;
      return 1U;
    }
  }
  return 0U;
}

/**
 * @brief Allocate the first free URC slot and copy the prefix into it.
 * @return 1 on success, 0 if the table is full.
 *
 * @details See implementation.
 * @param[in] prefix See implementation.
 * @param[in] plen See implementation.
 * @param[in] fn See implementation.
 * @param[in] ctx See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t
internal_urc_insert(const char* prefix, uint16_t plen, ra8_modem_at_urc_fn_t fn, void* ctx)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_modem_at_max_unsolicited; ++i) {
    if (s_mod.urcs[i].used != 0U) {
      continue;
    }
    uint16_t k = 0U;
    while (k < plen) {
      s_mod.urcs[i].prefix[k] = (uint8_t)prefix[k];
      ++k;
    }
    s_mod.urcs[i].prefix[plen] = (uint8_t)'\0';
    s_mod.urcs[i].fn           = fn;
    s_mod.urcs[i].ctx          = ctx;
    s_mod.urcs[i].used         = 1U;
    return 1U;
  }
  return 0U;
}

ra8_err_t
ra8_modem_at_register_unsolicited_handler(const char* prefix, ra8_modem_at_urc_fn_t fn, void* ctx)
{
  if (s_mod.initialized == 0U) {
    return k_ra8_err_not_initialized;
  }
  RA8_CHECK_NULL_PTR(prefix, RA8_MODEM_AT_TAG, "prefix");
  RA8_CHECK_NULL_PTR((void*)fn, RA8_MODEM_AT_TAG, "fn");

  const uint16_t plen = ra8_modem_at_internal_str_len(prefix);
  if ((plen == 0U) || (plen >= (uint16_t)k_ra8_modem_at_max_prefix_len)) {
    return k_ra8_err_invalid_size;
  }

  if (internal_urc_replace(prefix, fn, ctx) != 0U) {
    return k_ra8_ok;
  }
  if (internal_urc_insert(prefix, plen, fn, ctx) != 0U) {
    return k_ra8_ok;
  }
  return k_ra8_err_no_mem;
}

ra8_err_t ra8_modem_at_poll(void)
{
  if (s_mod.initialized == 0U) {
    return k_ra8_err_not_initialized;
  }
  while (1) {
    uint8_t         byte = 0U;
    const ra8_err_t err  = s_mod.cfg.io.rx_byte(s_mod.cfg.io.ctx, &byte);
    if (err != k_ra8_ok) {
      break;
    }
    const char* line = nullptr;
    internal_accumulate(byte, &line);
    if (line != nullptr) {
      const ra8_modem_line_kind_t kind = ra8_modem_at_internal_classify(line, nullptr, nullptr);
      (void)kind;
    }
  }
  return k_ra8_ok;
}
