/**
 * @file examples/ek_ra8d2/common/network_provision/tests/src/test_ra8_net_provision.c
 * @brief Host tests for bounded runtime network provisioning
 *
 * @details
 * Exercises valid and invalid version-one packets, every field bound, chunked
 * UART receive, prompt-before-read ordering, timeout, no-echo behavior, and
 * explicit credential erasure through the production implementation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_net_provision.h"
#include "unity_minimal.h"

/** @brief Fixed capacities and sentinels owned by the mock UART. */
typedef enum : uint16_t {
  k_test_uart_bytes       = 1536U, /**< Mock input/output storage capacity. */
  k_test_uart_chunk_bytes = 7U,    /**< Default bytes returned per read.    */
  k_test_fill             = 0xA5U, /**< Non-zero byte used before erasure.  */
} test_limit_t;

/** @brief Bit masks used by the packet encoder fixture. */
typedef enum : uint8_t {
  k_test_hex_low_nibble_mask = 0x0FU, /**< Selects a byte's low hexadecimal digit. */
} test_encoding_t;

/** @brief Deterministic UART state shared by the injected callbacks. */
typedef struct test_uart {
  uint8_t   input[k_test_uart_bytes];  /**< Bytes available to the receiver.   */
  uint8_t   output[k_test_uart_bytes]; /**< Bytes written by the receiver.     */
  size_t    input_length;              /**< Total readable input bytes.        */
  size_t    input_offset;              /**< Bytes already consumed.            */
  size_t    output_length;             /**< Bytes written by the receiver.     */
  size_t    chunk_bytes;               /**< Maximum bytes returned per read.   */
  size_t    report_extra;              /**< Extra bytes a hostile read claims. */
  uint32_t  waits;                     /**< Millisecond waits requested.       */
  uint32_t  reads;                     /**< Read calls observed.               */
  ra8_err_t write_result;              /**< Result returned by mock write.     */
  ra8_err_t read_result;               /**< Result returned by mock read.      */
  bool      read_before_write;         /**< A read preceded the prompt.        */
} test_uart_t;

/** @brief One mock UART used because callback signatures match the board API. */
static test_uart_t s_uart;

/**
 * @brief Reset the mock UART to successful, bounded operations.
 * @details Clears all recorded state, selects a small read chunk, and makes
 *          both injected operations succeed for the next scenario.
 * @return Nothing.
 * @pre No receiver callback is running concurrently.
 * @pre The file-local mock remains writable.
 * @post All counters and byte extents are zero.
 * @post Read and write results are ::k_ra8_ok.
 * @note Not thread-safe because the test executable owns one shared mock.
 * @since 0.1.0
 */
static void test_uart_reset(void)
{
  s_uart              = (test_uart_t){};
  s_uart.chunk_bytes  = k_test_uart_chunk_bytes;
  s_uart.write_result = k_ra8_ok;
  s_uart.read_result  = k_ra8_ok;
}

/**
 * @brief Record one prompt write without printing it.
 * @details Copies the supplied non-secret prompt into fixed mock output and
 *          returns the scenario-selected result without touching input.
 * @param[in] data Bytes supplied by the receiver.
 * @param[in] length Number of supplied bytes.
 * @return Configured mock result, or invalid size on overflow.
 * @retval k_ra8_ok The write fit and the configured result was success.
 * @retval k_ra8_err_invalid_size The write exceeded mock output storage.
 * @pre `data` addresses `length` readable bytes.
 * @pre Mock output state was initialized by ::test_uart_reset.
 * @post A fitting write is appended exactly once.
 * @post Input state remains unchanged.
 * @note Not thread-safe because it updates the file-local mock.
 * @since 0.1.0
 */
static ra8_err_t test_uart_write(const uint8_t* data, size_t length)
{
  if ((s_uart.output_length + length) > sizeof(s_uart.output)) {
    return k_ra8_err_invalid_size;
  }
  (void)memcpy(&s_uart.output[s_uart.output_length], data, length);
  s_uart.output_length += length;
  return s_uart.write_result;
}

/**
 * @brief Drain one deterministic chunk of mock input.
 * @details Records ordering, honors the selected error, and otherwise copies
 *          the minimum of available bytes, configured chunk size, and capacity.
 * @param[out] data Receiver-owned destination.
 * @param[in] capacity Destination capacity.
 * @param[out] out_length Bytes copied this call.
 * @return Configured mock result.
 * @retval k_ra8_ok A possibly empty bounded chunk was returned.
 * @retval k_ra8_err_protocol_error The scenario requested a read failure.
 * @pre `data` has `capacity` writable bytes.
 * @pre `out_length` is non-null and mock offsets are internally consistent.
 * @post `out_length` equals the copied extent plus the configured hostile
 *       over-report used to exercise receiver-side contract enforcement.
 * @post The read count advances exactly once.
 * @note Not thread-safe because it updates the file-local mock.
 * @since 0.1.0
 */
static ra8_err_t test_uart_read(uint8_t* data, size_t capacity, size_t* out_length)
{
  s_uart.reads++;
  if (s_uart.output_length == 0U) {
    s_uart.read_before_write = true;
  }
  *out_length = 0U;
  if (s_uart.read_result != k_ra8_ok) {
    return s_uart.read_result;
  }
  size_t available = s_uart.input_length - s_uart.input_offset;
  if (available > s_uart.chunk_bytes) {
    available = s_uart.chunk_bytes;
  }
  if (available > capacity) {
    available = capacity;
  }
  if (available != 0U) {
    (void)memcpy(data, &s_uart.input[s_uart.input_offset], available);
    s_uart.input_offset += available;
  }
  *out_length = available + s_uart.report_extra;
  return k_ra8_ok;
}

/**
 * @brief Record one bounded receiver wait.
 * @details Adds the requested delay to the deterministic elapsed-time counter;
 *          the host test performs no wall-clock sleep.
 * @param[in] delay_ms Milliseconds requested by the receiver.
 * @return Nothing.
 * @pre `delay_ms` is the receiver's one-millisecond poll interval.
 * @pre The mock wait counter cannot overflow in a bounded test scenario.
 * @post The wait counter increases by `delay_ms`.
 * @post No input or output byte changes.
 * @note Not thread-safe because it updates the file-local mock.
 * @since 0.1.0
 */
static void test_uart_wait(uint32_t delay_ms)
{
  s_uart.waits += delay_ms;
}

/**
 * @brief Build one version-one packet by hex-encoding three text fields.
 * @details Writes the fixed prefix, two lowercase hex characters per source
 *          byte, two field separators, and the required final newline.
 * @param[in] ssid SSID text.
 * @param[in] psk PSK text.
 * @param[in] url Optional URL text.
 * @param[out] output Packet destination.
 * @param[in] capacity Packet destination capacity.
 * @return Complete packet length, or zero on overflow.
 * @retval 0 The destination could not hold the complete packet.
 * @retval >0 The complete packet length, including its newline.
 * @pre All three input strings are NUL-terminated.
 * @pre `output` has `capacity` writable bytes.
 * @post Success produces one complete version-one record.
 * @post Failure never writes beyond `capacity`.
 * @note Thread-safe for distinct input and output buffers.
 * @since 0.1.0
 */
static size_t test_make_packet(const char* ssid,
                               const char* psk,
                               const char* url,
                               uint8_t*    output,
                               size_t      capacity)
{
  static const char hex[]     = "0123456789abcdef";
  static const char prefix[]  = "RA8NET1:";
  const char*       fields[3] = {ssid, psk, url};
  size_t            used      = sizeof(prefix) - 1U;
  if (used > capacity) {
    return 0U;
  }
  (void)memcpy(output, prefix, used);
  for (uint8_t field = 0U; field < 3U; ++field) {
    const size_t length = strlen(fields[field]);
    if ((used + (length * 2U) + 1U) > capacity) {
      return 0U;
    }
    for (size_t index = 0U; index < length; ++index) {
      const uint8_t byte = (uint8_t)fields[field][index];
      output[used++]     = (uint8_t)hex[byte >> 4U];
      output[used++]     = (uint8_t)hex[byte & k_test_hex_low_nibble_mask];
    }
    output[used++] = (field == 2U) ? (uint8_t)'\n' : (uint8_t)':';
  }
  return used;
}

/**
 * @brief Assert a rejected packet also clears a pre-filled output record.
 * @details Fills a credential record with a non-zero sentinel, invokes the
 *          production parser, and checks rejection plus byte-for-byte erasure.
 * @param[in] packet Candidate packet bytes.
 * @param[in] length Candidate packet length.
 * @return Nothing.
 * @pre `packet` addresses `length` readable bytes.
 * @pre The Unity test context is active.
 * @post The parser returned a non-success status.
 * @post Every credential byte was observed as zero.
 * @note Thread-safe because all state is automatic.
 * @since 0.1.0
 */
static void test_expect_rejected(const uint8_t* packet, size_t length)
{
  ra8_net_credentials_t credentials;
  (void)memset(&credentials, (int)k_test_fill, sizeof(credentials));
  TEST_ASSERT(ra8_net_provision_parse(packet, length, &credentials) != k_ra8_ok);
  const uint8_t* bytes = (const uint8_t*)&credentials;
  for (size_t index = 0U; index < sizeof(credentials); ++index) {
    TEST_ASSERT_EQ(0, bytes[index]);
  }
}

/**
 * @brief Assert null parser dependencies fail and clear writable output.
 * @details Exercises null input and null output independently while proving the
 *          null-input path erases a pre-filled credential record.
 * @param[in] packet Non-null packet used for the null-output assertion.
 * @param[in] length Length of `packet` in bytes.
 * @return Nothing.
 * @pre `packet` addresses `length` readable bytes.
 * @pre The Unity test context is active.
 * @post Null input returns ::k_ra8_err_null_ptr and clears every output byte.
 * @post Null output returns ::k_ra8_err_null_ptr.
 * @note Thread-safe because all state is automatic.
 * @since 0.1.0
 */
static void test_expect_null_parse_dependencies(const uint8_t* packet, size_t length)
{
  ra8_net_credentials_t credentials;
  (void)memset(&credentials, (int)k_test_fill, sizeof(credentials));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_net_provision_parse(nullptr, 1U, &credentials));
  const uint8_t* bytes = (const uint8_t*)&credentials;
  for (size_t index = 0U; index < sizeof(credentials); ++index) {
    TEST_ASSERT_EQ(0, bytes[index]);
  }
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_net_provision_parse(packet, length, nullptr));
}

/**
 * @brief Assert malformed framing and hexadecimal spellings are rejected.
 * @details Exercises prefix, minimum length, newline, separator, even-length,
 *          and every hexadecimal-alphabet boundary independently.
 * @param[in] wrong_prefix Packet carrying an unsupported version prefix.
 * @param[in] wrong_prefix_length Length of `wrong_prefix` in bytes.
 * @return Nothing.
 * @pre `wrong_prefix` addresses `wrong_prefix_length` readable bytes.
 * @pre The Unity test context is active.
 * @post Every malformed packet is rejected.
 * @post Every rejected credential record is fully zero.
 * @note Thread-safe because all vectors and parser outputs are automatic or immutable.
 * @since 0.1.0
 */
static void test_expect_malformed_framing(const uint8_t* wrong_prefix, size_t wrong_prefix_length)
{
  static const uint8_t before_numeric[]   = "RA8NET1:/1:70617373776f7264:\n";
  static const uint8_t after_numeric[]    = "RA8NET1:;1:70617373776f7264:\n";
  static const uint8_t before_lowercase[] = "RA8NET1:`1:70617373776f7264:\n";
  static const uint8_t before_uppercase[] = "RA8NET1:@1:70617373776f7264:\n";
  static const uint8_t after_uppercase[]  = "RA8NET1:G1:70617373776f7264:\n";
  static const uint8_t short_line[]       = "RA8NET1:";
  static const uint8_t no_newline[]       = "RA8NET1:61:70617373776f7264:";
  static const uint8_t one_separator[]    = "RA8NET1:6170617373776f7264\n";
  static const uint8_t three_separators[] = "RA8NET1:61:70617373776f7264::\n";
  static const uint8_t odd_hex[]          = "RA8NET1:6:70617373776f7264:\n";
  static const uint8_t bad_hex_low[]      = "RA8NET1:6g:70617373776f7264:\n";
  static const uint8_t bad_hex_high[]     = "RA8NET1:g1:70617373776f7264:\n";

  test_expect_rejected(wrong_prefix, wrong_prefix_length);
  test_expect_rejected(short_line, sizeof(short_line) - 1U);
  test_expect_rejected(no_newline, sizeof(no_newline) - 1U);
  test_expect_rejected(one_separator, sizeof(one_separator) - 1U);
  test_expect_rejected(three_separators, sizeof(three_separators) - 1U);
  test_expect_rejected(odd_hex, sizeof(odd_hex) - 1U);
  test_expect_rejected(before_numeric, sizeof(before_numeric) - 1U);
  test_expect_rejected(after_numeric, sizeof(after_numeric) - 1U);
  test_expect_rejected(before_lowercase, sizeof(before_lowercase) - 1U);
  test_expect_rejected(before_uppercase, sizeof(before_uppercase) - 1U);
  test_expect_rejected(after_uppercase, sizeof(after_uppercase) - 1U);
  test_expect_rejected(bad_hex_low, sizeof(bad_hex_low) - 1U);
  test_expect_rejected(bad_hex_high, sizeof(bad_hex_high) - 1U);
}

/**
 * @brief Assert decoded NUL and control bytes are rejected in every field.
 * @details Supplies one NUL and one disallowed control byte independently in
 *          the SSID, PSK, and optional URL fields.
 * @return Nothing.
 * @pre The production parser is linked.
 * @pre The Unity test context is active.
 * @post All six decoded-control packets are rejected.
 * @post Every rejected credential record is fully zero.
 * @note Thread-safe because every vector is immutable.
 * @since 0.1.0
 */
static void test_expect_rejected_decoded_controls(void)
{
  static const uint8_t nul_ssid[]     = "RA8NET1:00:70617373776f7264:\n";
  static const uint8_t nul_psk[]      = "RA8NET1:61:00617373776f7264:\n";
  static const uint8_t nul_url[]      = "RA8NET1:61:70617373776f7264:00\n";
  static const uint8_t control_ssid[] = "RA8NET1:1f:70617373776f7264:\n";
  static const uint8_t control_psk[]  = "RA8NET1:61:1f617373776f7264:\n";
  static const uint8_t control_url[]  = "RA8NET1:61:70617373776f7264:7f\n";

  test_expect_rejected(nul_ssid, sizeof(nul_ssid) - 1U);
  test_expect_rejected(nul_psk, sizeof(nul_psk) - 1U);
  test_expect_rejected(nul_url, sizeof(nul_url) - 1U);
  test_expect_rejected(control_ssid, sizeof(control_ssid) - 1U);
  test_expect_rejected(control_psk, sizeof(control_psk) - 1U);
  test_expect_rejected(control_url, sizeof(control_url) - 1U);
}

/**
 * @test Accept representative, 63-byte-PSK, and maximum packet forms.
 * @brief Verify valid provisioning record forms.
 * @details Exercises an empty optional URL, the 63-byte passphrase boundary,
 *          and every maximum including the hexadecimal-only 64-byte PSK form.
 * @return Nothing.
 * @pre The packet helper and production parser are linked.
 * @pre Fixed-capacity automatic test storage is available.
 * @post Every record is accepted with exact decoded lengths.
 * @post The successful credential record is explicitly erased.
 * @note Thread-safe when the test executable runs this case serially.
 * @since 0.1.0
 * @par MC/DC:
 * Valid vectors drive both outcomes of the empty URL, 64-byte hexadecimal PSK,
 * and maximum-field checks without coupling any invalid syntax condition.
 */
static void test_parse_accepts_valid_packets(void)
{
  TEST_BEGIN("net provision parser accepts valid packets");
  static const uint8_t  uppercase_hex[] = "RA8NET1:6A:70617373776F7264:\n";
  uint8_t               packet[k_ra8_net_provision_line_bytes_max] = {};
  ra8_net_credentials_t credentials                                = {};

  size_t length = test_make_packet("bench", "password", "", packet, sizeof(packet));
  TEST_ASSERT(length != 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_net_provision_parse(packet, length, &credentials));
  TEST_ASSERT(strcmp(credentials.ssid, "bench") == 0);
  TEST_ASSERT(strcmp(credentials.psk, "password") == 0);
  TEST_ASSERT_EQ(0, credentials.url_len);

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_net_provision_parse(uppercase_hex, sizeof(uppercase_hex) - 1U, &credentials));
  TEST_ASSERT(strcmp(credentials.ssid, "j") == 0);
  TEST_ASSERT(strcmp(credentials.psk, "password") == 0);

  char psk_63[k_ra8_net_provision_psk_bytes_max];
  (void)memset(psk_63, 'p', sizeof(psk_63) - 1U);
  psk_63[sizeof(psk_63) - 1U] = '\0';
  length                      = test_make_packet("bench", psk_63, "", packet, sizeof(packet));
  TEST_ASSERT(length != 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_net_provision_parse(packet, length, &credentials));
  TEST_ASSERT_EQ(k_ra8_net_provision_psk_bytes_max - 1U, credentials.psk_len);

  char ssid[k_ra8_net_provision_ssid_bytes_max + 1U];
  char psk[k_ra8_net_provision_psk_bytes_max + 1U];
  char url[k_ra8_net_provision_url_bytes_max + 1U];
  (void)memset(ssid, 's', sizeof(ssid) - 1U);
  (void)memset(psk, 'a', sizeof(psk) - 1U);
  (void)memset(url, 'u', sizeof(url) - 1U);
  ssid[sizeof(ssid) - 1U] = '\0';
  psk[sizeof(psk) - 1U]   = '\0';
  url[sizeof(url) - 1U]   = '\0';
  length                  = test_make_packet(ssid, psk, url, packet, sizeof(packet));
  TEST_ASSERT_EQ(sizeof(packet), length);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_net_provision_parse(packet, length, &credentials));
  TEST_ASSERT_EQ(k_ra8_net_provision_ssid_bytes_max, credentials.ssid_len);
  TEST_ASSERT_EQ(k_ra8_net_provision_psk_bytes_max, credentials.psk_len);
  TEST_ASSERT_EQ(k_ra8_net_provision_url_bytes_max, credentials.url_len);
  ra8_net_provision_clear(&credentials);
  TEST_END("net provision parser accepts valid packets");
}

/**
 * @test Reject malformed framing and hex while leaving output quiet.
 * @brief Verify malformed framing and syntax are rejected quietly.
 * @details Varies null dependencies, line length, version prefix, newline,
 *          separator count, hex syntax, and decoded controls independently.
 * @return Nothing.
 * @pre The production parser is linked.
 * @pre Every fixture length includes only initialized bytes.
 * @post Every absent dependency and malformed vector is rejected.
 * @post Every rejected output record is fully zero.
 * @note Thread-safe because all test vectors are immutable or automatic.
 * @since 0.1.0
 * @par MC/DC:
 * Each framing condition independently changes rejection: prefix, newline,
 * separator count, even hex length, and hexadecimal alphabet.
 */
static void test_parse_rejects_malformed_packets(void)
{
  TEST_BEGIN("net provision parser rejects malformed packets");
  static const uint8_t wrong_prefix[] = "RA8NET2:61:70617373776f7264:\n";
  uint8_t              overlong[k_ra8_net_provision_line_bytes_max + 1U] = {};
  test_expect_null_parse_dependencies(wrong_prefix, sizeof(wrong_prefix) - 1U);
  test_expect_malformed_framing(wrong_prefix, sizeof(wrong_prefix) - 1U);
  test_expect_rejected_decoded_controls();
  test_expect_rejected(overlong, sizeof(overlong));
  TEST_END("net provision parser rejects malformed packets");
}

/**
 * @test Reject every decoded field immediately above or below its bound.
 * @brief Verify every decoded field bound.
 * @details Constructs independent underflow and overflow vectors for SSID,
 *          PSK, and URL, including the special invalid 64-byte PSK form.
 * @return Nothing.
 * @pre The packet helper can hold records above production maxima.
 * @pre The Unity test context is active.
 * @post Every out-of-bound record is rejected.
 * @post Every rejected credential record is fully zero.
 * @note Thread-safe because all state is automatic.
 * @since 0.1.0
 * @par MC/DC:
 * Independent vectors vary SSID emptiness/overflow, PSK underflow/non-hex
 * 64-byte form, and URL overflow while all other fields remain valid.
 */
static void test_parse_rejects_field_bounds(void)
{
  TEST_BEGIN("net provision parser rejects field bounds");
  uint8_t packet[k_test_uart_bytes] = {};
  char    ssid[k_ra8_net_provision_ssid_bytes_max + 2U];
  char    psk[k_ra8_net_provision_psk_bytes_max + 1U];
  char    psk_too_long[k_ra8_net_provision_psk_bytes_max + 2U];
  char    url[k_ra8_net_provision_url_bytes_max + 2U];
  (void)memset(ssid, 's', sizeof(ssid) - 1U);
  (void)memset(psk, 'p', sizeof(psk) - 1U);
  (void)memset(psk_too_long, 'a', sizeof(psk_too_long) - 1U);
  (void)memset(url, 'u', sizeof(url) - 1U);
  ssid[sizeof(ssid) - 1U]                 = '\0';
  psk[sizeof(psk) - 1U]                   = '\0';
  psk_too_long[sizeof(psk_too_long) - 1U] = '\0';
  url[sizeof(url) - 1U]                   = '\0';

  size_t length = test_make_packet("", "password", "", packet, sizeof(packet));
  test_expect_rejected(packet, length);
  length = test_make_packet(ssid, "password", "", packet, sizeof(packet));
  test_expect_rejected(packet, length);
  length = test_make_packet("bench", "1234567", "", packet, sizeof(packet));
  test_expect_rejected(packet, length);
  length = test_make_packet("bench", psk, "", packet, sizeof(packet));
  test_expect_rejected(packet, length);
  length = test_make_packet("bench", psk_too_long, "", packet, sizeof(packet));
  test_expect_rejected(packet, length);
  length = test_make_packet("bench", "password", url, packet, sizeof(packet));
  test_expect_rejected(packet, length);
  TEST_END("net provision parser rejects field bounds");
}

/**
 * @test Receive a chunked packet only after emitting the exact prompt.
 * @brief Verify prompt ordering and no-echo receive behavior.
 * @details Supplies a valid record in several read chunks and proves the only
 *          transmitted bytes are the exact versioned readiness prompt.
 * @return Nothing.
 * @pre The mock UART is reset and has one complete input record.
 * @pre The production receiver is linked.
 * @post The record is decoded successfully.
 * @post Output equals only the prompt and no read precedes it.
 * @note Not thread-safe because it uses the file-local mock UART.
 * @since 0.1.0
 * @par MC/DC:
 * Chunked input drives incomplete and complete non-empty reads and proves no
 * received byte is written back to the UART.
 */
static void test_receive_prompts_without_echo(void)
{
  TEST_BEGIN("net provision receiver prompts without echo");
  test_uart_reset();
  s_uart.input_length                 = test_make_packet("bench",
                                                         "password",
                                                         "https://example.invalid/a",
                                                         s_uart.input,
                                                         sizeof(s_uart.input));
  const ra8_net_provision_uart_t uart = {
    .write   = test_uart_write,
    .read    = test_uart_read,
    .wait_ms = test_uart_wait,
  };
  ra8_net_credentials_t credentials = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_net_provision_receive(&uart, 100U, &credentials));
  const size_t prompt_length = strlen(k_ra8_net_provision_ready_prompt);
  TEST_ASSERT_EQ(prompt_length, s_uart.output_length);
  TEST_ASSERT(memcmp(s_uart.output, k_ra8_net_provision_ready_prompt, prompt_length) == 0);
  TEST_ASSERT(!s_uart.read_before_write);
  TEST_ASSERT(s_uart.reads > 1U);
  TEST_ASSERT(strcmp(credentials.url, "https://example.invalid/a") == 0);
  ra8_net_provision_clear(&credentials);
  TEST_END("net provision receiver prompts without echo");
}

/**
 * @test Time out after the exact bounded number of silent polls.
 * @brief Verify bounded quiet timeout behavior.
 * @details Leaves mock input empty for a four-millisecond budget and checks the
 *          exact read/wait counts plus failure-side credential erasure.
 * @return Nothing.
 * @pre The mock UART is reset with no input bytes.
 * @pre The production receiver is linked.
 * @post The receiver returns ::k_ra8_err_timeout after four polls.
 * @post The credential record is fully zero.
 * @note Not thread-safe because it uses the file-local mock UART.
 * @since 0.1.0
 * @par MC/DC:
 * The stay-quiet vector supplies no bytes for every poll; the separate success
 * test varies only input availability and terminates before the timeout.
 */
static void test_receive_times_out_quietly(void)
{
  TEST_BEGIN("net provision receiver times out quietly");
  test_uart_reset();
  const ra8_net_provision_uart_t uart = {
    .write   = test_uart_write,
    .read    = test_uart_read,
    .wait_ms = test_uart_wait,
  };
  ra8_net_credentials_t credentials;
  (void)memset(&credentials, (int)k_test_fill, sizeof(credentials));
  TEST_ASSERT_EQ(k_ra8_err_timeout, ra8_net_provision_receive(&uart, 4U, &credentials));
  TEST_ASSERT_EQ(4, s_uart.reads);
  TEST_ASSERT_EQ(4, s_uart.waits);
  const uint8_t* bytes = (const uint8_t*)&credentials;
  for (size_t index = 0U; index < sizeof(credentials); ++index) {
    TEST_ASSERT_EQ(0, bytes[index]);
  }
  TEST_END("net provision receiver times out quietly");
}

/**
 * @test Reject every absent receiver dependency and invalid timeout.
 * @brief Verify receiver dependency and timeout guards.
 * @details Varies the UART table, each callback, output, and both timeout
 *          boundaries independently before any operation is allowed to run.
 * @return Nothing.
 * @pre The mock UART is reset and the output record is writable.
 * @pre The production receiver is linked.
 * @post Every invalid dependency returns ::k_ra8_err_null_ptr.
 * @post Invalid timeout bounds return ::k_ra8_err_invalid_arg and output is zero.
 * @note Not thread-safe because it uses the file-local mock UART.
 * @since 0.1.0
 * @par MC/DC:
 * One vector independently removes the table and each of its three callbacks;
 * separate vectors cross the lower and upper timeout bounds.
 */
static void test_receive_rejects_invalid_dependencies(void)
{
  TEST_BEGIN("net provision receiver rejects invalid dependencies");
  test_uart_reset();
  ra8_net_credentials_t credentials;
  (void)memset(&credentials, (int)k_test_fill, sizeof(credentials));
  ra8_net_provision_uart_t uart = {
    .write   = test_uart_write,
    .read    = test_uart_read,
    .wait_ms = test_uart_wait,
  };

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_net_provision_receive(nullptr, 1U, &credentials));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_net_provision_receive(&uart, 1U, nullptr));
  uart.write = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_net_provision_receive(&uart, 1U, &credentials));
  uart.write = test_uart_write;
  uart.read  = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_net_provision_receive(&uart, 1U, &credentials));
  uart.read    = test_uart_read;
  uart.wait_ms = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_net_provision_receive(&uart, 1U, &credentials));
  uart.wait_ms = test_uart_wait;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_net_provision_receive(&uart, 0U, &credentials));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_net_provision_receive(&uart,
                                           (uint32_t)k_ra8_net_provision_timeout_ms_max + 1U,
                                           &credentials));
  const uint8_t* bytes = (const uint8_t*)&credentials;
  for (size_t index = 0U; index < sizeof(credentials); ++index) {
    TEST_ASSERT_EQ(0, bytes[index]);
  }
  TEST_ASSERT_EQ(0, s_uart.output_length);
  TEST_ASSERT_EQ(0, s_uart.reads);
  TEST_END("net provision receiver rejects invalid dependencies");
}

/**
 * @test Propagate prompt-write and UART-read failures without retaining output.
 * @brief Verify receiver I/O failure handling.
 * @details Injects one write failure before reads begin and one read failure
 *          after the prompt, then observes exact ordering and cleared output.
 * @return Nothing.
 * @pre The mock UART callbacks return the configured repository errors.
 * @pre The production receiver is linked.
 * @post A failed prompt prevents every read.
 * @post A failed first read returns immediately and credentials remain zero.
 * @note Not thread-safe because it uses the file-local mock UART.
 * @since 0.1.0
 * @par MC/DC:
 * Independent write and read failures drive each I/O-error exit while the
 * successful receiver test holds both callback results at ::k_ra8_ok.
 */
static void test_receive_propagates_io_failures(void)
{
  TEST_BEGIN("net provision receiver propagates I/O failures");
  const ra8_net_provision_uart_t uart = {
    .write   = test_uart_write,
    .read    = test_uart_read,
    .wait_ms = test_uart_wait,
  };
  ra8_net_credentials_t credentials;

  test_uart_reset();
  s_uart.write_result = k_ra8_err_protocol_error;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_net_provision_receive(&uart, 2U, &credentials));
  TEST_ASSERT_EQ(0, s_uart.reads);

  test_uart_reset();
  s_uart.read_result = k_ra8_err_protocol_error;
  (void)memset(&credentials, (int)k_test_fill, sizeof(credentials));
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_net_provision_receive(&uart, 2U, &credentials));
  TEST_ASSERT_EQ(1, s_uart.reads);
  TEST_ASSERT_EQ(0, s_uart.waits);
  const uint8_t* bytes = (const uint8_t*)&credentials;
  for (size_t index = 0U; index < sizeof(credentials); ++index) {
    TEST_ASSERT_EQ(0, bytes[index]);
  }
  TEST_END("net provision receiver propagates I/O failures");
}

/**
 * @test Reject hostile read lengths, trailing bytes, full lines, and bad lines.
 * @brief Verify receiver framing and capacity defenses.
 * @details Drives four independent hostile callback or input behaviors through
 *          the production receive state machine without exposing received data.
 * @return Nothing.
 * @pre The mock input storage can hold the maximum protocol line.
 * @pre The production receiver is linked.
 * @post Every hostile scenario returns a non-success status.
 * @post The final rejected scenario leaves every output byte zero.
 * @note Not thread-safe because it uses the file-local mock UART.
 * @since 0.1.0
 * @par MC/DC:
 * Separate vectors vary over-reporting, bytes after newline, capacity without
 * newline, and complete-line syntax while valid chunked input is the control.
 */
static void test_receive_rejects_hostile_input(void)
{
  TEST_BEGIN("net provision receiver rejects hostile input");
  const ra8_net_provision_uart_t uart = {
    .write   = test_uart_write,
    .read    = test_uart_read,
    .wait_ms = test_uart_wait,
  };
  ra8_net_credentials_t credentials;

  test_uart_reset();
  s_uart.input_length =
    test_make_packet("bench", "password", "", s_uart.input, sizeof(s_uart.input));
  s_uart.input[s_uart.input_length++] = (uint8_t)'x';
  s_uart.chunk_bytes                  = s_uart.input_length;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_net_provision_receive(&uart, 2U, &credentials));

  test_uart_reset();
  s_uart.report_extra = sizeof(s_uart.input);
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_net_provision_receive(&uart, 2U, &credentials));

  test_uart_reset();
  (void)memset(s_uart.input, '1', (size_t)k_ra8_net_provision_line_bytes_max);
  s_uart.input_length = (size_t)k_ra8_net_provision_line_bytes_max;
  s_uart.chunk_bytes  = s_uart.input_length;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_net_provision_receive(&uart, 2U, &credentials));

  static const uint8_t malformed[] = "RA8NET1:61:00:\n";
  test_uart_reset();
  (void)memcpy(s_uart.input, malformed, sizeof(malformed) - 1U);
  s_uart.input_length = sizeof(malformed) - 1U;
  s_uart.chunk_bytes  = s_uart.input_length;
  (void)memset(&credentials, (int)k_test_fill, sizeof(credentials));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_net_provision_receive(&uart, 2U, &credentials));
  const uint8_t* bytes = (const uint8_t*)&credentials;
  for (size_t index = 0U; index < sizeof(credentials); ++index) {
    TEST_ASSERT_EQ(0, bytes[index]);
  }
  TEST_END("net provision receiver rejects hostile input");
}

/**
 * @test Explicit clearing overwrites the complete credential record.
 * @brief Verify explicit credential-record erasure.
 * @details Fills every record byte with a sentinel, clears the record, checks
 *          every byte, and exercises null cleanup as a defensive no-op.
 * @return Nothing.
 * @pre The secure-memory implementation is linked.
 * @pre The Unity test context is active.
 * @post Every byte of the non-null record is zero.
 * @post The null cleanup call completes without access.
 * @note Thread-safe because the credential record is automatic.
 * @since 0.1.0
 * @par MC/DC:
 * Non-null clearing changes every sentinel byte; the null call exercises the
 * defensive no-op branch without a memory access.
 */
static void test_clear_zeroizes_record(void)
{
  TEST_BEGIN("net provision clear zeroizes record");
  ra8_net_credentials_t credentials;
  (void)memset(&credentials, (int)k_test_fill, sizeof(credentials));
  ra8_net_provision_clear(&credentials);
  const uint8_t* bytes = (const uint8_t*)&credentials;
  for (size_t index = 0U; index < sizeof(credentials); ++index) {
    TEST_ASSERT_EQ(0, bytes[index]);
  }
  ra8_net_provision_clear(nullptr);
  TEST_END("net provision clear zeroizes record");
}

/**
 * @brief Run all runtime provisioning tests.
 * @details Invokes each focused parser, receive, and erasure scenario once in a
 *          deterministic order and returns after the final Unity assertion.
 * @return Zero when every assertion passes.
 * @retval 0 Every test function returned normally.
 * @pre The test executable linked all listed test functions.
 * @pre No other thread mutates the file-local UART mock.
 * @post Every focused scenario was invoked exactly once.
 * @post Process status is zero when assertions remain satisfied.
 * @note Not thread-safe because the suite shares one mock UART.
 * @since 0.1.0
 */
int main(void)
{
  test_parse_accepts_valid_packets();
  test_parse_rejects_malformed_packets();
  test_parse_rejects_field_bounds();
  test_receive_prompts_without_echo();
  test_receive_times_out_quietly();
  test_receive_rejects_invalid_dependencies();
  test_receive_propagates_io_failures();
  test_receive_rejects_hostile_input();
  test_clear_zeroizes_record();
  return 0;
}
