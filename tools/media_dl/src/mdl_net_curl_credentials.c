/**
 * @file mdl_net_curl_credentials.c
 * @brief Path-free libcurl cookie and CA credential binding.
 * @details Validates bounded caller-owned bytes before importing cookies and
 *          binds complete CA PEM bytes with the NOCOPY blob contract.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <curl/curl.h>
#include <string.h>

#include "mdl_net.h"
#include "mdl_net_curl_internal.h"
#include "ra8_attributes.h"

/** @brief Maximum imported cookie row including its terminating NUL. */
typedef enum : uint16_t {
  k_cookie_line_max = 4096U, /**< Honest fixed per-row importer bound. */
} mdl_cookie_limit_t;

/** @brief Cookie-line disposition after strict syntax validation. */
typedef enum : uint8_t {
  k_cookie_row_reject = 0, /**< Reject the complete credential input. */
  k_cookie_row_ignore,     /**< Recognized blank/comment framing.     */
  k_cookie_row_accept,     /**< Safe cookie row to import.            */
} cookie_row_disposition_t;

/**
 * @brief Lower-case one ASCII byte without locale state.
 * @param[in] value Byte to map.
 * @return Lower-case ASCII byte or the unchanged input.
 * @retval 0 The input byte was NUL.
 * @retval other Lower-case mapping or unchanged input byte.
 * @since 0.1.0

 * @details Parses only bounded caller bytes and rejects command-like cookie rows.
 *          Only validated borrowed credential storage reaches libcurl.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 */
RA8_INTERNAL static char internal_ascii_lower(char value)
{
  return ((value >= 'A') && (value <= 'Z')) ? (char)(value - 'A' + 'a') : value;
}

/**
 * @brief Test one bounded value for an ASCII case-insensitive prefix.
 * @param[in] data Bounded bytes to inspect.
 * @param[in] length Readable byte count.
 * @param[in] prefix Lower-case NUL-terminated ASCII prefix.
 * @return Whether the complete prefix matched.
 * @since 0.1.0

 * @details Parses only bounded caller bytes and rejects command-like cookie rows.
 *          Only validated borrowed credential storage reaches libcurl.
 * @retval true The documented predicate holds or the requested operation completed.
 * @retval false The predicate does not hold or validation rejected the operation.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 */
RA8_INTERNAL static bool
internal_ascii_has_prefix(const char* data, size_t length, const char* prefix)
{
  size_t index = 0U;
  while ((index < length) && (prefix[index] != '\0')) {
    if (internal_ascii_lower(data[index]) != prefix[index]) {
      return false;
    }
    ++index;
  }
  return prefix[index] == '\0';
}

/**
 * @brief Compare one bounded field with an exact ASCII literal.
 * @param[in] data Bounded field bytes.
 * @param[in] length Readable field byte count.
 * @param[in] literal NUL-terminated comparison literal.
 * @return Whether the field and literal are byte-identical.
 * @since 0.1.0

 * @details Parses only bounded caller bytes and rejects command-like cookie rows.
 *          Only validated borrowed credential storage reaches libcurl.
 * @retval true The documented predicate holds or the requested operation completed.
 * @retval false The predicate does not hold or validation rejected the operation.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 */
RA8_INTERNAL static bool
internal_cookie_field_is(const char* data, size_t length, const char* literal)
{
  const size_t literal_length = strlen(literal);
  return (length == literal_length) && (memcmp(data, literal, length) == 0);
}

/**
 * @brief Validate one decimal Netscape-cookie expiry field.
 * @param[in] data Field bytes.
 * @param[in] length Field byte count.
 * @return Whether the nonempty field contains only decimal digits.
 * @since 0.1.0

 * @details Parses only bounded caller bytes and rejects command-like cookie rows.
 *          Only validated borrowed credential storage reaches libcurl.
 * @retval true The documented predicate holds or the requested operation completed.
 * @retval false The predicate does not hold or validation rejected the operation.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 */
RA8_INTERNAL static bool internal_cookie_expiry_valid(const char* data, size_t length)
{
  if (length == 0U) {
    return false;
  }
  for (size_t index = 0U; index < length; ++index) {
    if ((data[index] < '0') || (data[index] > '9')) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Validate an RFC token-style cookie name.
 * @param[in] data Bounded candidate name bytes.
 * @param[in] length Candidate byte count.
 * @return Whether the nonempty name contains only permitted ASCII token bytes.
 * @since 0.1.0

 * @details Parses only bounded caller bytes and rejects command-like cookie rows.
 *          Only validated borrowed credential storage reaches libcurl.
 * @retval true The documented predicate holds or the requested operation completed.
 * @retval false The predicate does not hold or validation rejected the operation.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 */
RA8_INTERNAL static bool internal_cookie_name_valid(const char* data, size_t length)
{
  static const char k_symbols[] = "!#$%&'*+-.^_`|~";
  if (length == 0U) {
    return false;
  }
  for (size_t index = 0U; index < length; ++index) {
    const char value         = data[index];
    const bool alpha_numeric = ((value >= 'A') && (value <= 'Z')) ||
                               ((value >= 'a') && (value <= 'z')) ||
                               ((value >= '0') && (value <= '9'));
    if (!alpha_numeric && (strchr(k_symbols, value) == nullptr)) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Validate one Netscape cookie-file row without accepting commands.
 * @param[in] line Bounded cookie row without a line ending.
 * @param[in] length Readable row bytes.
 * @return Whether the row has the required seven tab-separated fields.
 * @since 0.1.0

 * @details Parses only bounded caller bytes and rejects command-like cookie rows.
 *          Only validated borrowed credential storage reaches libcurl.
 * @retval true The documented predicate holds or the requested operation completed.
 * @retval false The predicate does not hold or validation rejected the operation.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 */
RA8_INTERNAL static bool internal_netscape_cookie_valid(const char* line, size_t length)
{
  size_t field_start = 0U;
  size_t field_index = 0U;
  for (size_t index = 0U; index <= length; ++index) {
    if ((index != length) && (line[index] != '\t')) {
      continue;
    }
    const size_t field_length = index - field_start;
    if (((field_index == 0U) || (field_index == 2U) || (field_index == 5U)) &&
        (field_length == 0U)) {
      return false;
    }
    if (((field_index == 1U) || (field_index == 3U)) &&
        !internal_cookie_field_is(line + field_start, field_length, "TRUE") &&
        !internal_cookie_field_is(line + field_start, field_length, "FALSE")) {
      return false;
    }
    if ((field_index == 4U) && !internal_cookie_expiry_valid(line + field_start, field_length)) {
      return false;
    }
    if ((field_index == 5U) && !internal_cookie_name_valid(line + field_start, field_length)) {
      return false;
    }
    ++field_index;
    field_start = index + 1U;
  }
  return field_index == 7U;
}

/**
 * @brief Test whether a Set-Cookie attribute has a nonempty Domain value.
 * @param[in] attribute Bounded attribute bytes.
 * @param[in] length Attribute byte count.
 * @return Whether this is an explicit nonempty Domain attribute.
 * @since 0.1.0

 * @details Parses only bounded caller bytes and rejects command-like cookie rows.
 *          Only validated borrowed credential storage reaches libcurl.
 * @retval true The documented predicate holds or the requested operation completed.
 * @retval false The predicate does not hold or validation rejected the operation.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 */
RA8_INTERNAL static bool internal_cookie_domain_valid(const char* attribute, size_t length)
{
  while ((length > 0U) && ((attribute[length - 1U] == ' ') || (attribute[length - 1U] == '\t'))) {
    --length;
  }
  const size_t prefix_length = sizeof("Domain=") - 1U;
  return internal_ascii_has_prefix(attribute, length, "domain=") && (length > prefix_length);
}

/**
 * @brief Validate a Set-Cookie row and require an explicit Domain attribute.
 * @param[in] line Bounded cookie row without a line ending.
 * @param[in] length Readable row bytes.
 * @return Whether the row has a cookie pair and a nonempty Domain attribute.
 * @since 0.1.0

 * @details Parses only bounded caller bytes and rejects command-like cookie rows.
 *          Only validated borrowed credential storage reaches libcurl.
 * @retval true The documented predicate holds or the requested operation completed.
 * @retval false The predicate does not hold or validation rejected the operation.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 */
RA8_INTERNAL static bool internal_set_cookie_valid(const char* line, size_t length)
{
  const size_t prefix_length = sizeof("Set-Cookie:") - 1U;
  size_t       pair_end      = prefix_length;
  while ((pair_end < length) && (line[pair_end] != ';')) {
    ++pair_end;
  }
  size_t name_start = prefix_length;
  while ((name_start < pair_end) && ((line[name_start] == ' ') || (line[name_start] == '\t'))) {
    ++name_start;
  }
  size_t equals = name_start;
  while ((equals < pair_end) && (line[equals] != '=')) {
    ++equals;
  }
  if ((equals == pair_end) || !internal_cookie_name_valid(line + name_start, equals - name_start)) {
    return false;
  }
  for (size_t start = pair_end; start < length;) {
    ++start;
    while ((start < length) && ((line[start] == ' ') || (line[start] == '\t'))) {
      ++start;
    }
    size_t end = start;
    while ((end < length) && (line[end] != ';')) {
      ++end;
    }
    if (internal_cookie_domain_valid(line + start, end - start)) {
      return true;
    }
    start = end;
  }
  return false;
}

/**
 * @brief Classify a bounded cookie-file line for safe libcurl import.
 * @param[in] line Bounded row without CR/LF.
 * @param[in] length Readable row bytes.
 * @return Reject, ignore, or accept disposition.
 * @retval k_cookie_row_reject The row is unsafe or malformed.
 * @retval k_cookie_row_ignore The row is recognized framing only.
 * @retval k_cookie_row_accept The complete row is safe to import.
 * @since 0.1.0

 * @details Parses only bounded caller bytes and rejects command-like cookie rows.
 *          Only validated borrowed credential storage reaches libcurl.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 */
RA8_INTERNAL static cookie_row_disposition_t internal_cookie_classify(const char* line,
                                                                      size_t      length)
{
  for (size_t index = 0U; index < length; ++index) {
    const uint8_t value = (uint8_t)line[index];
    if ((value >= 0x7FU) || ((value < 0x20U) && (value != (uint8_t)'\t'))) {
      return k_cookie_row_reject;
    }
  }
  while ((length > 0U) && ((line[length - 1U] == ' ') || (line[length - 1U] == '\t'))) {
    --length;
  }
  while ((length > 0U) && ((*line == ' ') || (*line == '\t'))) {
    ++line;
    --length;
  }
  if (length == 0U) {
    return k_cookie_row_ignore;
  }
  if ((line[0] == '#') && !internal_ascii_has_prefix(line, length, "#httponly_")) {
    return k_cookie_row_ignore;
  }
  const char* const commands[] = {"ALL", "SESS", "FLUSH", "RELOAD"};
  for (size_t index = 0U; index < (sizeof(commands) / sizeof(commands[0])); ++index) {
    if (internal_cookie_field_is(line, length, commands[index])) {
      return k_cookie_row_reject;
    }
  }
  const bool valid = internal_ascii_has_prefix(line, length, "set-cookie:")
                       ? internal_set_cookie_valid(line, length)
                       : internal_netscape_cookie_valid(line, length);
  return valid ? k_cookie_row_accept : k_cookie_row_reject;
}

/**
 * @brief Import one validated bounded cookie row.
 * @param[in,out] curl Fresh easy handle.
 * @param[in] data Row bytes without CR/LF.
 * @param[in] length Row byte count.
 * @return Canonical validation or option status.
 * @since 0.1.0

 * @details Parses only bounded caller bytes and rejects command-like cookie rows.
 *          Only validated borrowed credential storage reaches libcurl.
 * @retval k_ra8_ok The operation completed successfully.
 * @retval other The originating validation, storage, stream, or network error.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 */
RA8_INTERNAL static ra8_err_t
internal_apply_cookie_line(CURL* curl, const uint8_t* data, size_t length)
{
  if (length >= (size_t)k_cookie_line_max) {
    return k_ra8_err_invalid_size;
  }
  char line[k_cookie_line_max];
  memcpy(line, data, length);
  line[length]                               = '\0';
  const cookie_row_disposition_t disposition = internal_cookie_classify(line, length);
  if (disposition == k_cookie_row_reject) {
    return k_ra8_err_invalid_arg;
  }
  if ((disposition == k_cookie_row_accept) &&
      (curl_easy_setopt(curl, CURLOPT_COOKIELIST, line) != CURLE_OK)) {
    return k_ra8_fail;
  }
  return k_ra8_ok;
}

ra8_err_t priv_mdl_net_curl_apply_cookies(CURL* curl, const mdl_net_bytes_t* cookies)
{
  if ((curl == nullptr) || (cookies == nullptr) ||
      ((cookies->length == 0U) != (cookies->data == nullptr))) {
    return k_ra8_err_invalid_arg;
  }
  if (curl_easy_setopt(curl, CURLOPT_COOKIELIST, "ALL") != CURLE_OK) {
    return k_ra8_fail;
  }
  size_t offset = 0U;
  while (offset < cookies->length) {
    size_t end = offset;
    while ((end < cookies->length) && (cookies->data[end] != (uint8_t)'\n') &&
           (cookies->data[end] != 0U)) {
      ++end;
    }
    if ((end < cookies->length) && (cookies->data[end] == 0U)) {
      return k_ra8_err_invalid_arg;
    }
    size_t length = end - offset;
    if ((length > 0U) && (cookies->data[offset + length - 1U] == (uint8_t)'\r')) {
      --length;
    }
    const ra8_err_t error = internal_apply_cookie_line(curl, cookies->data + offset, length);
    if (error != k_ra8_ok) {
      return error;
    }
    offset = (end < cookies->length) ? end + 1U : end;
  }
  return k_ra8_ok;
}

ra8_err_t
priv_mdl_net_curl_apply_ca_blob(CURL* curl, const mdl_net_bytes_t* ca_pem, struct curl_blob* blob)
{
  if ((curl == nullptr) || (ca_pem == nullptr) ||
      ((ca_pem->length == 0U) != (ca_pem->data == nullptr))) {
    return k_ra8_err_invalid_arg;
  }
  if (ca_pem->length == 0U) {
    return k_ra8_ok;
  }
  for (size_t index = 0U; index < ca_pem->length; ++index) {
    if (ca_pem->data[index] == 0U) {
      return k_ra8_err_invalid_arg;
    }
  }
#if LIBCURL_VERSION_NUM >= 0x074D00
  if (blob == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  *blob               = (struct curl_blob){.data  = (void*)ca_pem->data,
                                           .len   = ca_pem->length,
                                           .flags = CURL_BLOB_NOCOPY};
  const CURLcode code = curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, blob);
  if (code == CURLE_OK) {
    return k_ra8_ok;
  }
  return ((code == CURLE_NOT_BUILT_IN) || (code == CURLE_UNKNOWN_OPTION)) ? k_ra8_err_not_supported
                                                                          : k_ra8_fail;
#else
  (void)blob;
  return k_ra8_err_not_supported;
#endif
}
