/**
 * @file examples/ek_ra8d2/common/network_provision/src/ra8_net_provision.c
 * @brief Bounded runtime network provisioning parser and UART receiver
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Decodes one versioned ASCII-hex record into caller-owned fixed storage and
 * collects that record through injected non-blocking UART operations. Raw and
 * decoded transient storage is explicitly erased on all return paths.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_net_provision.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_secure.h"

/** @brief Exact protocol prefix, including the first field separator. */
static const uint8_t s_prefix[k_ra8_net_provision_prefix_bytes] = {
  'R',
  'A',
  '8',
  'N',
  'E',
  'T',
  '1',
  ':',
};

/**
 * @enum ra8_net_provision_text_byte_t
 * @brief Control-byte boundaries rejected from decoded text fields
 * @details Mirrors the host provisioner's text policy so a direct UART sender
 *          cannot inject C0 or DEL control bytes into C-string consumers.
 * @invariant Printable ASCII begins at ::k_ra8_net_provision_text_printable_min.
 * @invariant ::k_ra8_net_provision_text_delete is the only C0-adjacent control
 *            byte above the printable ASCII range.
 * @par Example:
 * @code
 * if (byte < k_ra8_net_provision_text_printable_min) { reject(); }
 * @endcode
 * @see internal_has_no_controls
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_net_provision_text_printable_min = 0x20U, /**< First printable ASCII byte. */
  k_ra8_net_provision_text_delete        = 0x7FU, /**< ASCII DEL control byte.     */
} ra8_net_provision_text_byte_t;

/**
 * @enum ra8_net_provision_hex_t
 * @brief Numeric base of hexadecimal alphabet digits
 * @details Converts an alphabet index beginning at `A` or `a` into the
 *          corresponding nibble value beginning at decimal ten.
 * @invariant The value fits in one decoded nibble.
 * @invariant Both accepted ASCII letter cases use the same offset.
 * @par Example:
 * @code
 * nibble = letter_index + k_ra8_net_provision_hex_alpha_offset;
 * @endcode
 * @see internal_hex_nibble
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_net_provision_hex_alpha_offset = 10U, /**< Value represented by `A` or `a`. */
} ra8_net_provision_hex_t;

const char k_ra8_net_provision_ready_prompt[] = "ra8_net_provision: READY v1\r\n";

/**
 * @brief Decode one hexadecimal digit.
 * @details Accepts decimal and either case of ASCII hexadecimal letters.
 * @param[in] digit ASCII byte to decode.
 * @param[out] out Decoded nibble on success.
 * @return Whether `digit` was hexadecimal.
 * @retval true `out` contains a value in 0..15.
 * @retval false `digit` was not hexadecimal and `out` is zero.
 * @pre `out` is non-null.
 * @pre `digit` is one byte from the bounded input line.
 * @post Success initializes `out` with one nibble.
 * @post Failure initializes `out` to zero.
 * @note Pure helper; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_hex_nibble(uint8_t digit, uint8_t* out)
{
  *out = 0U;
  if ((digit >= (uint8_t)'0') && (digit <= (uint8_t)'9')) {
    *out = (uint8_t)(digit - (uint8_t)'0');
    return true;
  }
  if ((digit >= (uint8_t)'a') && (digit <= (uint8_t)'f')) {
    *out = (uint8_t)(digit - (uint8_t)'a' + (uint8_t)k_ra8_net_provision_hex_alpha_offset);
    return true;
  }
  if ((digit >= (uint8_t)'A') && (digit <= (uint8_t)'F')) {
    *out = (uint8_t)(digit - (uint8_t)'A' + (uint8_t)k_ra8_net_provision_hex_alpha_offset);
    return true;
  }
  return false;
}

/**
 * @brief Decode one bounded hexadecimal field.
 * @details Processes exactly two input characters per output byte and appends
 *          one NUL terminator after the decoded extent.
 * @param[in] line Complete provisioning line.
 * @param[in] begin First hex byte of the field.
 * @param[in] end One-past-last hex byte of the field.
 * @param[out] output Destination text buffer.
 * @param[in] capacity Payload capacity, excluding the trailing NUL.
 * @param[out] out_length Decoded payload length.
 * @return Repository error code.
 * @retval k_ra8_ok The field decoded completely.
 * @retval k_ra8_err_invalid_size The encoded extent was odd or too large.
 * @retval k_ra8_err_protocol_error A character was not hexadecimal.
 * @pre Input and output extents are valid and non-overlapping.
 * @pre `output` has `capacity + 1` writable bytes.
 * @post Success writes `*out_length` bytes and a trailing NUL.
 * @post Failure leaves only a caller-owned transient that will be erased.
 * @note The loop is bounded by `capacity` (NASA Rule 2).
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_decode_field(const uint8_t* line,
                                                    size_t         begin,
                                                    size_t         end,
                                                    char*          output,
                                                    size_t         capacity,
                                                    size_t*        out_length)
{
  const size_t encoded = end - begin;
  if ((encoded % 2U) != 0U) {
    return k_ra8_err_invalid_size;
  }
  const size_t decoded = encoded / 2U;
  if (decoded > capacity) {
    return k_ra8_err_invalid_size;
  }
  for (size_t index = 0U; index < decoded; ++index) {
    uint8_t high = 0U;
    uint8_t low  = 0U;
    if (!internal_hex_nibble(line[begin + (index * 2U)], &high)) {
      return k_ra8_err_protocol_error;
    }
    if (!internal_hex_nibble(line[begin + (index * 2U) + 1U], &low)) {
      return k_ra8_err_protocol_error;
    }
    output[index] = (char)((uint8_t)(high << 4U) | low);
  }
  output[decoded] = '\0';
  *out_length     = decoded;
  return k_ra8_ok;
}

/**
 * @brief Check a decoded text field for C0 and DEL control bytes.
 * @details Scans the caller-bounded decoded extent and stops at the first C0
 *          or DEL byte because downstream network APIs require printable text.
 * @param[in] text Decoded text storage.
 * @param[in] length Bytes to examine, excluding the appended terminator.
 * @return Whether every payload byte is free of control bytes.
 * @retval true No C0 or DEL control byte was present.
 * @retval false A decoded payload byte was C0 or DEL.
 * @pre `text` addresses at least `length` bytes.
 * @pre `length` is bounded by one protocol field maximum.
 * @post Input remains unchanged.
 * @post Exactly `length` bytes were examined unless a control byte was found.
 * @note Pure helper; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_has_no_controls(const char* text, size_t length)
{
  for (size_t index = 0U; index < length; ++index) {
    const uint8_t byte = (uint8_t)text[index];
    if ((byte < (uint8_t)k_ra8_net_provision_text_printable_min) ||
        (byte == (uint8_t)k_ra8_net_provision_text_delete)) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Check whether a decoded 64-byte PSK contains only ASCII hex.
 * @details Reuses the protocol nibble classifier for every decoded PSK byte;
 *          this distinguishes a raw 256-bit hexadecimal key from passphrases.
 * @param[in] psk Decoded PSK text.
 * @return Whether all 64 bytes are hexadecimal characters.
 * @retval true Every byte is ASCII hexadecimal.
 * @retval false At least one byte is not ASCII hexadecimal.
 * @pre `psk` addresses at least 64 bytes.
 * @pre The caller invokes this only for a 64-byte PSK.
 * @post Input remains unchanged.
 * @post At most 64 bytes were examined.
 * @note Pure helper; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_psk_is_hex(const char* psk)
{
  for (size_t index = 0U; index < (size_t)k_ra8_net_provision_psk_bytes_max; ++index) {
    uint8_t ignored = 0U;
    if (!internal_hex_nibble((uint8_t)psk[index], &ignored)) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Validate decoded field lengths and string compatibility.
 * @details Applies required-field bounds first, then rejects C0 and DEL
 *          controls and enforces the hexadecimal rule for 64-byte PSKs.
 * @param[in] candidate Fully decoded candidate record.
 * @return Repository error code.
 * @retval k_ra8_ok Every field satisfies the version-one contract.
 * @retval k_ra8_err_invalid_size A required field length was invalid.
 * @retval k_ra8_err_protocol_error A field contained a control byte or a
 *                                  64-byte PSK was not hexadecimal.
 * @pre Candidate length fields describe their corresponding arrays.
 * @pre Every array has a trailing NUL after its declared length.
 * @post Candidate remains unchanged.
 * @post Success guarantees compatibility with existing C-string consumers.
 * @note Pure helper; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_validate_candidate(const ra8_net_credentials_t* candidate)
{
  if (candidate->ssid_len == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (candidate->psk_len < 8U) {
    return k_ra8_err_invalid_size;
  }
  if (!internal_has_no_controls(candidate->ssid, candidate->ssid_len)) {
    return k_ra8_err_protocol_error;
  }
  if (!internal_has_no_controls(candidate->psk, candidate->psk_len)) {
    return k_ra8_err_protocol_error;
  }
  if (!internal_has_no_controls(candidate->url, candidate->url_len)) {
    return k_ra8_err_protocol_error;
  }
  if ((candidate->psk_len == (uint8_t)k_ra8_net_provision_psk_bytes_max) &&
      !internal_psk_is_hex(candidate->psk)) {
    return k_ra8_err_protocol_error;
  }
  return k_ra8_ok;
}

/**
 * @brief Decode the three fields between already-validated framing bytes.
 * @details Finds exactly two field separators, decodes each bounded hex extent
 *          into the candidate record, and validates the completed candidate.
 * @param[in] line Complete input line.
 * @param[in] line_length Complete input length.
 * @param[out] candidate Cleared candidate record to populate.
 * @return Repository error code from separator, decode, or field validation.
 * @retval k_ra8_ok All three fields decoded and validated.
 * @retval k_ra8_err_invalid_size A field exceeded its capacity.
 * @retval k_ra8_err_protocol_error Separator or hex syntax was invalid.
 * @pre Prefix and trailing newline were validated by the caller.
 * @pre `candidate` was zero-initialized.
 * @post Success fully initializes the candidate.
 * @post Failure may partially initialize the candidate for caller erasure.
 * @note Each scan is bounded by the fixed line maximum.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_decode_line(const uint8_t* line, size_t line_length, ra8_net_credentials_t* candidate)
{
  size_t  separators[2]   = {};
  uint8_t separator_count = 0U;
  for (size_t index = (size_t)k_ra8_net_provision_prefix_bytes; index < (line_length - 1U);
       ++index) {
    if (line[index] == (uint8_t)':') {
      if (separator_count >= 2U) {
        return k_ra8_err_protocol_error;
      }
      separators[separator_count] = index;
      separator_count++;
    }
  }
  if (separator_count != 2U) {
    return k_ra8_err_protocol_error;
  }

  size_t    decoded_length = 0U;
  ra8_err_t err            = internal_decode_field(line,
                                                   (size_t)k_ra8_net_provision_prefix_bytes,
                                                   separators[0],
                                                   candidate->ssid,
                                                   (size_t)k_ra8_net_provision_ssid_bytes_max,
                                                   &decoded_length);
  candidate->ssid_len      = (uint8_t)decoded_length;
  if (err == k_ra8_ok) {
    err                = internal_decode_field(line,
                                               separators[0] + 1U,
                                               separators[1],
                                               candidate->psk,
                                               (size_t)k_ra8_net_provision_psk_bytes_max,
                                               &decoded_length);
    candidate->psk_len = (uint8_t)decoded_length;
  }
  if (err == k_ra8_ok) {
    err                = internal_decode_field(line,
                                               separators[1] + 1U,
                                               line_length - 1U,
                                               candidate->url,
                                               (size_t)k_ra8_net_provision_url_bytes_max,
                                               &decoded_length);
    candidate->url_len = (uint16_t)decoded_length;
  }
  return (err == k_ra8_ok) ? internal_validate_candidate(candidate) : err;
}

/**
 * @brief Check the fixed version prefix without a libc comparison.
 * @details Compares each byte against immutable prefix storage so the caller
 *          needs no temporary NUL terminator or unbounded string operation.
 * @param[in] line Input line with at least the prefix length.
 * @return Whether every fixed prefix byte matched.
 * @retval true The line begins with `RA8NET1:`.
 * @retval false At least one prefix byte differed.
 * @pre `line` addresses at least eight readable bytes.
 * @pre Prefix storage is initialized.
 * @post Neither input nor prefix storage changes.
 * @post At most eight bytes were examined.
 * @note Pure helper; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_prefix_matches(const uint8_t* line)
{
  for (size_t index = 0U; index < (size_t)k_ra8_net_provision_prefix_bytes; ++index) {
    if (line[index] != s_prefix[index]) {
      return false;
    }
  }
  return true;
}

void ra8_net_provision_clear(ra8_net_credentials_t* credentials)
{
  if (credentials != nullptr) {
    ra8_secure_memzero(credentials, sizeof(*credentials));
  }
}

ra8_err_t
ra8_net_provision_parse(const uint8_t* line, size_t line_length, ra8_net_credentials_t* out)
{
  if (out == nullptr) {
    return k_ra8_err_null_ptr;
  }
  ra8_net_provision_clear(out);
  if (line == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if ((line_length <= (size_t)k_ra8_net_provision_prefix_bytes) ||
      (line_length > (size_t)k_ra8_net_provision_line_bytes_max)) {
    return k_ra8_err_invalid_size;
  }
  if (!internal_prefix_matches(line)) {
    return k_ra8_err_protocol_error;
  }
  if (line[line_length - 1U] != (uint8_t)'\n') {
    return k_ra8_err_protocol_error;
  }

  ra8_net_credentials_t candidate = {};
  const ra8_err_t       err       = internal_decode_line(line, line_length, &candidate);
  if (err == k_ra8_ok) {
    *out = candidate;
  }
  ra8_net_provision_clear(&candidate);
  return err;
}

/**
 * @brief Validate the injected receiver dependencies and timeout.
 * @details Rejects an absent operation row before any callback can run, then
 *          constrains the caller's timeout to the protocol's fixed maximum.
 * @param[in] uart UART operation table to validate.
 * @param[in] timeout_ms Requested timeout.
 * @return Repository error code.
 * @retval k_ra8_ok Every dependency and bound is valid.
 * @retval k_ra8_err_null_ptr The table or one row was null.
 * @retval k_ra8_err_invalid_arg The timeout was outside 1..60000.
 * @pre No operation row is invoked by this helper.
 * @pre The timeout is an untrusted caller input.
 * @post UART state remains unchanged.
 * @post Success permits the bounded receiver loop to start.
 * @note Pure validation helper; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_validate_receiver(const ra8_net_provision_uart_t* uart,
                                                         uint32_t                        timeout_ms)
{
  if (uart == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (uart->write == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (uart->read == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (uart->wait_ms == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if ((timeout_ms == 0U) || (timeout_ms > (uint32_t)k_ra8_net_provision_timeout_ms_max)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Locate a newline in one newly received chunk.
 * @details Performs a forward bounded scan and reports only the first newline,
 *          allowing the receiver to reject any bytes that follow one record.
 * @param[in] chunk Newly received bytes.
 * @param[in] length Number of bytes in the chunk.
 * @param[out] newline_offset Offset of the first newline when found.
 * @return Whether a newline was found.
 * @retval true `newline_offset` names the first newline.
 * @retval false No newline was present and `newline_offset` is zero.
 * @pre `chunk` addresses `length` readable bytes.
 * @pre `newline_offset` is non-null.
 * @post At most `length` bytes were examined.
 * @post Input remains unchanged.
 * @note Pure helper; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_find_newline(const uint8_t* chunk, size_t length, size_t* newline_offset)
{
  *newline_offset = 0U;
  for (size_t index = 0U; index < length; ++index) {
    if (chunk[index] == (uint8_t)'\n') {
      *newline_offset = index;
      return true;
    }
  }
  return false;
}

/**
 * @brief Receive exactly one newline-terminated provisioning record.
 * @details Polls the injected UART for at most @p timeout_ms iterations,
 *          rejects bytes after the first newline, and bounds all writes by the
 *          supplied line capacity.
 * @param[in] uart Validated UART binding.
 * @param[in] timeout_ms Maximum one-millisecond polling iterations.
 * @param[out] line Caller-owned receive buffer.
 * @param[in] line_capacity Writable bytes in @p line.
 * @param[out] line_length Received bytes through the newline on success.
 * @return Bounded receive status.
 * @retval k_ra8_ok One complete record was received.
 * @retval k_ra8_err_timeout No complete record arrived before the deadline.
 * @retval k_ra8_err_invalid_size The record filled the buffer without a newline.
 * @retval k_ra8_err_protocol_error A read over-reported bytes or included trailing data.
 * @pre @p uart contains non-NULL read and wait callbacks.
 * @pre @p line and @p line_length are non-NULL and @p line_capacity is nonzero.
 * @post Success sets @p line_length to the complete record size.
 * @post The helper performs no reads or waits after detecting a newline.
 * @note Synchronous and not thread-safe for a shared UART binding.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_receive_line(const ra8_net_provision_uart_t* uart,
                                                    uint32_t                        timeout_ms,
                                                    uint8_t*                        line,
                                                    size_t                          line_capacity,
                                                    size_t*                         line_length)
{
  ra8_err_t err      = k_ra8_ok;
  size_t    used     = 0U;
  bool      complete = false;
  for (uint32_t elapsed = 0U; elapsed < timeout_ms; ++elapsed) {
    size_t       received  = 0U;
    const size_t remaining = line_capacity - used;
    err                    = uart->read(&line[used], remaining, &received);
    if (err != k_ra8_ok) {
      break;
    }
    if (received > remaining) {
      err = k_ra8_err_protocol_error;
      break;
    }
    size_t newline_offset = 0U;
    if (internal_find_newline(&line[used], received, &newline_offset)) {
      used += newline_offset + 1U;
      if ((newline_offset + 1U) != received) {
        err = k_ra8_err_protocol_error;
      }
      complete = true;
      break;
    }
    used += received;
    if (used == line_capacity) {
      err = k_ra8_err_invalid_size;
      break;
    }
    uart->wait_ms(1U);
  }
  if ((err == k_ra8_ok) && !complete) {
    err = k_ra8_err_timeout;
  }
  *line_length = used;
  return err;
}

ra8_err_t ra8_net_provision_receive(const ra8_net_provision_uart_t* uart,
                                    uint32_t                        timeout_ms,
                                    ra8_net_credentials_t*          out)
{
  if (out == nullptr) {
    return k_ra8_err_null_ptr;
  }
  ra8_net_provision_clear(out);
  ra8_err_t err = internal_validate_receiver(uart, timeout_ms);
  if (err != k_ra8_ok) {
    return err;
  }

  const size_t prompt_length = sizeof(k_ra8_net_provision_ready_prompt) - 1U;
  err = uart->write((const uint8_t*)k_ra8_net_provision_ready_prompt, prompt_length);
  uint8_t line[k_ra8_net_provision_line_bytes_max] = {};
  size_t  used                                     = 0U;
  if (err == k_ra8_ok) {
    err = internal_receive_line(uart, timeout_ms, line, sizeof(line), &used);
  }
  if (err == k_ra8_ok) {
    err = ra8_net_provision_parse(line, used, out);
  }
  ra8_secure_memzero(line, sizeof(line));
  if (err != k_ra8_ok) {
    ra8_net_provision_clear(out);
  }
  return err;
}
