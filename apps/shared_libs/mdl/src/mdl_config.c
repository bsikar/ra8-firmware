/**
 * @file mdl_config.c
 * @brief Flat key=value site-descriptor parser over injected portable storage.
 * @details Validates bounded descriptor fields and converts policy values from
 *          storage-backed text without retaining external buffers.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_config.h"

#include <string.h>

#include "mdl_politeness.h"
#include "ra8_attributes.h"

/** @brief Local parser limits. */
typedef enum : uint16_t {
  k_line_max = 512, /**< Max config line bytes including NUL. */
} mdl_parse_limits_t;

/** @brief Complete descriptor and backend-progress ceilings. */
typedef enum : uint32_t {
  k_config_file_max   = 64U * 1024U,            /**< Largest accepted descriptor file. */
  k_config_read_calls = k_config_file_max + 1U, /**< One-byte reads plus EOF.          */
  k_config_dec_base   = 10U,                    /**< Base-10 integer fields.           */
} mdl_config_io_limit_t;

/** @brief Buffered reader borrowing one initialized storage binding. */
typedef struct {
  mdl_storage_t* storage;    /**< Injected filesystem and scratch. */
  fw_fs_file_t   file;       /**< Caller-owned portable handle.    */
  uint32_t       position;   /**< Next unread scratch offset.      */
  uint32_t       available;  /**< Readable scratch byte count.     */
  uint32_t       read_calls; /**< Bounded backend read calls.      */
  bool           eof;        /**< Clean zero-byte read observed.   */
} internal_config_reader_t;

/** @brief Result of applying one descriptor key. */
typedef enum : uint8_t {
  k_kv_unknown = 0, /**< Key is not owned by this helper.   */
  k_kv_applied = 1, /**< Key and value were accepted.       */
  k_kv_invalid = 2, /**< Key is known but value is invalid. */
} mdl_kv_result_t;

/** @brief Default politeness bounds (ms) applied before the file is read. */
typedef enum : uint16_t {
  k_def_img_delay_min     = 500,  /**< Per-image spacing floor.       */
  k_def_img_delay_max     = 1200, /**< Per-image spacing ceiling.     */
  k_def_chapter_delay_min = 1500, /**< Inter-chapter spacing floor.   */
  k_def_chapter_delay_max = 3000, /**< Inter-chapter spacing ceiling. */
} mdl_config_defaults_t;

/** @brief `--polite` per-request delay floors (milliseconds). */
typedef enum : uint16_t {
  k_polite_img_min_ms  = 2000,  /**< Polite per-image floor.       */
  k_polite_img_max_ms  = 4000,  /**< Polite per-image ceiling.     */
  k_polite_chap_min_ms = 5000,  /**< Polite inter-chapter floor.   */
  k_polite_chap_max_ms = 10000, /**< Polite inter-chapter ceiling. */
} mdl_config_polite_floor_t;

/** @brief Trim leading/trailing ASCII whitespace in place; return start. */
RA8_INTERNAL static char* internal_config_trim(char* s)
{
  while ((*s == ' ') || (*s == '\t') || (*s == '\r') || (*s == '\n')) {
    ++s;
  }
  size_t n = strlen(s);
  while ((n > 0U) && ((s[n - 1U] == ' ') || (s[n - 1U] == '\t') || (s[n - 1U] == '\r') ||
                      (s[n - 1U] == '\n'))) {
    s[n - 1U] = '\0';
    --n;
  }
  return s;
}

/**
 * @brief Copy one descriptor value without truncation.
 * @details Measures the complete source before copying it into fixed storage.
 * @param[out] dst Destination character array.
 * @param[in]  cap Destination capacity including NUL.
 * @param[in]  val NUL-terminated source value.
 * @return Whether the complete value fit and was copied.
 * @retval true  @p dst contains an exact copy of @p val.
 * @retval false @p val did not fit and @p dst is unchanged.
 * @pre @p dst and @p val are non-NULL.
 * @pre @p cap is greater than zero and describes writable @p dst storage.
 * @post On true, @p dst is NUL-terminated.
 * @post @p val is unchanged.
 * @note Not thread-safe when source and destination storage are shared.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_config_set_str(char* dst, size_t cap, const char* val)
{
  const size_t len = strlen(val);
  if (len >= cap) {
    return false;
  }
  memcpy(dst, val, len + 1U);
  return true;
}

/**
 * @brief Apply a string-valued key and distinguish unknown from invalid.
 * @details Searches the fixed descriptor-field table and copies only complete,
 *          bounded values into the selected field.
 * @param[in,out] s Descriptor receiving a recognized string field.
 * @param[in] key NUL-terminated configuration key.
 * @param[in] val NUL-terminated configuration value.
 * @return Three-way key application result.
 * @retval k_kv_applied A recognized key was copied without truncation.
 * @retval k_kv_invalid A recognized value exceeded its destination bound.
 * @retval k_kv_unknown The key is not a string-valued descriptor field.
 * @pre All pointers are non-NULL.
 * @pre @p key and @p val are NUL-terminated.
 * @post Applied values are complete and NUL-terminated.
 * @post Unknown or invalid keys do not modify a descriptor field.
 * @note Not thread-safe for concurrent writes to the same descriptor.
 * @since 0.1.0
 */
RA8_INTERNAL static mdl_kv_result_t
internal_config_apply_string(mdl_site_t* s, const char* key, const char* val)
{
  const struct {
    const char* key; /**< Config key spelling.       */
    char*       dst; /**< Descriptor field it fills. */
    size_t      cap; /**< Capacity of that field.    */
  } fields[] = {
    {"name", s->name, sizeof(s->name)},
    {"host", s->host, sizeof(s->host)},
    {"kind", s->kind, sizeof(s->kind)},
    {"contact", s->contact, sizeof(s->contact)},
    {"chapter_url_contains", s->chapter_url_contains, sizeof(s->chapter_url_contains)},
    {"chapter_url_prefix", s->chapter_url_prefix, sizeof(s->chapter_url_prefix)},
    {"page_img_attr", s->page_img_attr, sizeof(s->page_img_attr)},
    {"page_img_url_contains", s->page_img_url_contains, sizeof(s->page_img_url_contains)},
    {"search_url", s->search_url, sizeof(s->search_url)},
    {"search_result_contains", s->search_result_contains, sizeof(s->search_result_contains)},
    {"browse_url", s->browse_url, sizeof(s->browse_url)},
    {"series_title_selector", s->series_title_selector, sizeof(s->series_title_selector)},
    {"series_summary_selector", s->series_summary_selector, sizeof(s->series_summary_selector)},
    {"series_author_selector", s->series_author_selector, sizeof(s->series_author_selector)},
    {"series_artist_selector", s->series_artist_selector, sizeof(s->series_artist_selector)},
    {"series_cover_selector", s->series_cover_selector, sizeof(s->series_cover_selector)},
    {"chapter_title_selector", s->chapter_title_selector, sizeof(s->chapter_title_selector)},
    {"chapter_number_selector", s->chapter_number_selector, sizeof(s->chapter_number_selector)},
    {"language", s->language, sizeof(s->language)},
    {"reading_direction", s->reading_direction, sizeof(s->reading_direction)},
  };
  for (size_t i = 0U; i < (sizeof(fields) / sizeof(fields[0])); ++i) {
    if (strcmp(key, fields[i].key) == 0) {
      return internal_config_set_str(fields[i].dst, fields[i].cap, val) ? k_kv_applied
                                                                        : k_kv_invalid;
    }
  }
  return k_kv_unknown;
}

/**
 * @brief Parse one complete unsigned 32-bit decimal field.
 * @details Rejects signs, empty text, overflow, and trailing characters.
 * @param[in]  val NUL-terminated decimal text.
 * @param[out] out Parsed integer destination.
 * @return Whether one complete in-range integer was parsed.
 * @retval true  @p out received the parsed value.
 * @retval false The text was empty, signed, malformed, or out of range.
 * @pre @p val and @p out are non-NULL.
 * @pre @p val is NUL-terminated.
 * @post On true, @p out contains the exact parsed value.
 * @post On false, @p out is unchanged.
 * @note Thread-safe across disjoint input and output storage.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_config_parse_u32(const char* val, uint32_t* out)
{
  if ((val[0] == '\0') || (val[0] == '+') || (val[0] == '-')) {
    return false;
  }
  uint32_t parsed = 0U;
  for (size_t index = 0U; val[index] != '\0'; ++index) {
    if ((val[index] < '0') || (val[index] > '9')) {
      return false;
    }
    const uint32_t digit = (uint32_t)(val[index] - '0');
    if (parsed > ((UINT32_MAX - digit) / (uint32_t)k_config_dec_base)) {
      return false;
    }
    parsed = (parsed * (uint32_t)k_config_dec_base) + digit;
  }
  *out = parsed;
  return true;
}

/**
 * @brief Apply an unsigned-integer key (jitter delay or governor bound).
 * @details Searches the fixed integer-field table and delegates complete,
 *          overflow-safe decimal conversion to ::internal_config_parse_u32.
 * @param[in,out] s Descriptor receiving a recognized integer field.
 * @param[in] key NUL-terminated configuration key.
 * @param[in] val NUL-terminated unsigned decimal value.
 * @return Three-way key application result.
 * @retval k_kv_applied A recognized key received a valid integer.
 * @retval k_kv_invalid A recognized key carried an invalid integer.
 * @retval k_kv_unknown The key is not an integer-valued descriptor field.
 * @pre All pointers are non-NULL.
 * @pre @p key and @p val are NUL-terminated.
 * @post Applied values exactly equal the parsed decimal text.
 * @post Unknown or invalid keys do not modify an integer field.
 * @note Not thread-safe for concurrent writes to the same descriptor.
 * @since 0.1.0
 */
RA8_INTERNAL static mdl_kv_result_t
internal_config_apply_u32(mdl_site_t* s, const char* key, const char* val)
{
  const struct {
    const char* key; /**< Config key spelling.       */
    uint32_t*   dst; /**< Descriptor field it fills. */
  } fields[] = {
    {"img_delay_min", &s->img_delay_min},
    {"img_delay_max", &s->img_delay_max},
    {"chapter_delay_min", &s->chapter_delay_min},
    {"chapter_delay_max", &s->chapter_delay_max},
    {"rate_per_min", &s->rate_per_min},
    {"burst", &s->burst},
    {"backoff_base_ms", &s->backoff_base_ms},
    {"backoff_max_ms", &s->backoff_max_ms},
    {"max_inflight", &s->max_inflight},
  };
  for (size_t i = 0U; i < (sizeof(fields) / sizeof(fields[0])); ++i) {
    if (strcmp(key, fields[i].key) == 0) {
      return internal_config_parse_u32(val, fields[i].dst) ? k_kv_applied : k_kv_invalid;
    }
  }
  return k_kv_unknown;
}

/**
 * @brief Parse the configured chapter-order spelling.
 * @details Accepts only `reverse`, `asc`, or `doc`; there is no silent default.
 * @param[in]  val NUL-terminated order spelling.
 * @param[out] out Parsed order destination.
 * @return Whether @p val is one supported spelling.
 * @retval true  @p out received the corresponding enum value.
 * @retval false @p val is unsupported and @p out is unchanged.
 * @pre @p val and @p out are non-NULL.
 * @pre @p val is NUL-terminated.
 * @post On true, @p out is a valid ::mdl_chapter_order_t value.
 * @post @p val is unchanged.
 * @note Thread-safe: reads only caller storage.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_config_parse_order(const char* val, mdl_chapter_order_t* out)
{
  if (strcmp(val, "reverse") == 0) {
    *out = k_mdl_order_reverse;
    return true;
  }
  if (strcmp(val, "asc") == 0) {
    *out = k_mdl_order_asc;
    return true;
  }
  if (strcmp(val, "doc") == 0) {
    *out = k_mdl_order_doc;
    return true;
  }
  return false;
}

/**
 * @brief Apply one descriptor key/value pair.
 * @details Dispatches string, unsigned-integer, and chapter-order keys while
 *          rejecting unknown names and malformed values.
 * @param[in,out] s   Site descriptor being populated.
 * @param[in]     key NUL-terminated key spelling.
 * @param[in]     val NUL-terminated value text.
 * @return Whether the key is known and its complete value is valid.
 * @retval true  The matching descriptor field was updated.
 * @retval false The key is unknown or its value is invalid.
 * @pre @p s, @p key, and @p val are non-NULL.
 * @pre Both string arguments are NUL-terminated.
 * @post On true, exactly the selected field is updated.
 * @post On false, no truncated string is stored.
 * @note Not thread-safe: mutates caller-owned descriptor storage.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_config_apply_pair(mdl_site_t* s, const char* key, const char* val)
{
  const mdl_kv_result_t str_result = internal_config_apply_string(s, key, val);
  if (str_result != k_kv_unknown) {
    return str_result == k_kv_applied;
  }
  const mdl_kv_result_t int_result = internal_config_apply_u32(s, key, val);
  if (int_result != k_kv_unknown) {
    return int_result == k_kv_applied;
  }
  if (strcmp(key, "chapter_order") == 0) {
    return internal_config_parse_order(val, &s->chapter_order);
  }
  return false;
}

/**
 * @brief Apply the polite default descriptor before the file overrides it.
 * @details Clears the complete descriptor, installs bounded textual defaults,
 *          and copies the single governor baseline into numeric fields.
 * @param[out] out Descriptor receiving deterministic defaults.
 * @pre @p out is non-NULL.
 * @pre @p out addresses writable storage for one complete descriptor.
 * @post Every fixed string field is NUL-terminated.
 * @post Governor fields equal ::mdl_gov_cfg_default.
 * @note Not thread-safe for concurrent access to @p out.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_config_set_defaults(mdl_site_t* out)
{
  *out = (mdl_site_t){};
  (void)internal_config_set_str(out->name, sizeof(out->name), "site");
  (void)internal_config_set_str(out->kind, sizeof(out->kind), "manhwa");
  (void)internal_config_set_str(out->chapter_url_contains,
                                sizeof(out->chapter_url_contains),
                                "chapter");
  (void)internal_config_set_str(out->page_img_attr, sizeof(out->page_img_attr), "data-src");
  (void)internal_config_set_str(out->language, sizeof(out->language), "en");
  (void)internal_config_set_str(out->reading_direction, sizeof(out->reading_direction), "ltr");
  out->chapter_order     = k_mdl_order_asc;
  out->img_delay_min     = (uint32_t)k_def_img_delay_min;
  out->img_delay_max     = (uint32_t)k_def_img_delay_max;
  out->chapter_delay_min = (uint32_t)k_def_chapter_delay_min;
  out->chapter_delay_max = (uint32_t)k_def_chapter_delay_max;

  /* Governor bounds default to the conservative single source of truth. */
  const mdl_gov_cfg_t gov = mdl_gov_cfg_default();
  out->rate_per_min       = gov.rate_per_min;
  out->burst              = gov.burst;
  out->backoff_base_ms    = gov.backoff_base_ms;
  out->backoff_max_ms     = gov.backoff_max_ms;
  out->max_inflight       = (uint32_t)gov.max_inflight;
}

/**
 * @brief Read one byte through a bounded buffered portable stream.
 * @details Refills the storage binding's caller-owned scratch when exhausted;
 *          a successful zero-byte read is represented only through @p out_eof.
 * @param[in,out] reader Open descriptor reader.
 * @param[out] out_byte Next byte when one is available.
 * @param[out] out_eof Whether clean end-of-file was reached.
 * @return Canonical stream status.
 * @retval k_ra8_ok A byte or clean EOF was reported.
 * @retval k_ra8_err_invalid_size The bounded read-call ceiling was exhausted.
 * @retval other A backend read failure propagated unchanged.
 * @pre All pointers are non-NULL and @p reader owns an open file.
 * @pre The storage scratch is exclusively borrowed for the parse.
 * @post Success initializes @p out_eof and initializes @p out_byte unless EOF.
 * @post No backend read is issued after clean EOF.
 * @note Not thread-safe for a shared storage binding.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_config_next(internal_config_reader_t* reader, uint8_t* out_byte, bool* out_eof)
{
  *out_eof = false;
  while (reader->position == reader->available) {
    if (reader->eof) {
      *out_eof = true;
      return k_ra8_ok;
    }
    if (reader->read_calls >= (uint32_t)k_config_read_calls) {
      return k_ra8_err_invalid_size;
    }
    uint32_t        count = 0U;
    const ra8_err_t err   = fw_fs_read(&reader->file,
                                       reader->storage->io_buffer,
                                       reader->storage->io_buffer_bytes,
                                       &count);
    ++reader->read_calls;
    if (err != k_ra8_ok) {
      return err;
    }
    reader->position  = 0U;
    reader->available = count;
    reader->eof       = count == 0U;
  }
  *out_byte = reader->storage->io_buffer[reader->position];
  ++reader->position;
  return k_ra8_ok;
}

/**
 * @brief Read one complete bounded descriptor line.
 * @details Accepts a final line without newline and rejects the first non-newline
 *          byte that would leave no room for the required terminator.
 * @param[in,out] reader Open descriptor reader.
 * @param[out] line Destination for one NUL-terminated line without newline.
 * @param[in] capacity Writable bytes at @p line.
 * @param[out] out_present Whether a line, including an empty one, was read.
 * @return Canonical read or line-validation status.
 * @retval k_ra8_ok A line or clean EOF was reported.
 * @retval k_ra8_err_invalid_state A line exceeded the fixed parser bound.
 * @retval other A stream error propagated unchanged.
 * @pre All pointers are non-NULL and @p capacity is nonzero.
 * @pre @p reader owns an open read-only file.
 * @post @p line is always NUL-terminated on success.
 * @post Clean EOF before any byte sets @p out_present false.
 * @note Not thread-safe for a shared reader.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_config_read_line(internal_config_reader_t* reader,
                                                        char*                     line,
                                                        uint32_t                  capacity,
                                                        bool*                     out_present)
{
  uint32_t length = 0U;
  *out_present    = false;
  for (;;) {
    uint8_t   byte = 0U;
    bool      eof  = false;
    ra8_err_t err  = internal_config_next(reader, &byte, &eof);
    if (err != k_ra8_ok) {
      return err;
    }
    if (eof) {
      line[length] = '\0';
      *out_present = length > 0U;
      return k_ra8_ok;
    }
    *out_present = true;
    if (byte == (uint8_t)'\n') {
      line[length] = '\0';
      return k_ra8_ok;
    }
    if ((length + 1U) >= capacity) {
      return k_ra8_err_invalid_state;
    }
    line[length] = (char)byte;
    ++length;
  }
}

/**
 * @brief Apply one already bounded descriptor record.
 * @details Trims whitespace, ignores blank/comment/section records, and applies
 *          exactly one complete key/value pair for every active record.
 * @param[in,out] site Descriptor receiving the record.
 * @param[in,out] line Mutable NUL-terminated record text.
 * @return Record validation status.
 * @retval k_ra8_ok A blank/comment/section line was ignored or a pair applied.
 * @retval k_ra8_err_invalid_state The record was malformed or unsupported.
 * @pre Both pointers are non-NULL and @p line is NUL-terminated.
 * @pre @p line is writable because the separator is replaced in place.
 * @post Success preserves descriptor invariants for all applied fields.
 * @post Failure never stores a truncated field.
 * @note Not thread-safe for a shared descriptor.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_config_apply_line(mdl_site_t* site, char* line)
{
  char* text = internal_config_trim(line);
  if ((text[0] == '\0') || (text[0] == '#') || (text[0] == '[')) {
    return k_ra8_ok;
  }
  char* equals = strchr(text, '=');
  if (equals == nullptr) {
    return k_ra8_err_invalid_state;
  }
  *equals         = '\0';
  const char* key = internal_config_trim(text);
  const char* val = internal_config_trim(equals + 1);
  return ((key[0] != '\0') && internal_config_apply_pair(site, key, val)) ? k_ra8_ok
                                                                          : k_ra8_err_invalid_state;
}

/**
 * @brief Parse every descriptor record from an open portable stream.
 * @details Reuses one fixed stack line and stops only at clean EOF or the first
 *          bounded reader or record-validation failure.
 * @param[in,out] reader Open descriptor reader.
 * @param[in,out] out Descriptor receiving parsed overrides.
 * @return Complete parsing status.
 * @retval k_ra8_ok Every record was valid and EOF was clean.
 * @retval k_ra8_err_invalid_state A line or key/value was invalid.
 * @retval other A bounded stream failure propagated unchanged.
 * @pre Both pointers are non-NULL and the reader is positioned at byte zero.
 * @pre The descriptor contains defaults and is exclusively owned.
 * @post Success applies every active record exactly once.
 * @post Failure leaves a rejected partially populated descriptor.
 * @note Not thread-safe for shared storage or descriptor state.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_config_parse(internal_config_reader_t* reader,
                                                    mdl_site_t*               out)
{
  for (;;) {
    char      line[k_line_max];
    bool      present = false;
    ra8_err_t err     = internal_config_read_line(reader, line, sizeof(line), &present);
    if ((err != k_ra8_ok) || !present) {
      return err;
    }
    err = internal_config_apply_line(out, line);
    if (err != k_ra8_ok) {
      return err;
    }
  }
}

/**
 * @brief Validate descriptor cross-field and selector invariants.
 * @details Checks selector grammar, required static metadata, delay ordering,
 *          governor bounds, and reading direction after parsing completes.
 * @param[in] site Fully populated site descriptor.
 * @return Whether every descriptor invariant is satisfied.
 * @retval true  The descriptor is safe for fetch/search use.
 * @retval false A required value, selector, or cross-field bound is invalid.
 * @pre @p site is non-NULL.
 * @pre Every fixed string field in @p site is NUL-terminated.
 * @post @p site is unchanged.
 * @post No filesystem or network state is accessed.
 * @note Thread-safe: reads only caller storage.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_config_valid(const mdl_site_t* site)
{
  const char* selectors[] = {site->series_title_selector,
                             site->series_summary_selector,
                             site->series_author_selector,
                             site->series_artist_selector,
                             site->series_cover_selector,
                             site->chapter_title_selector,
                             site->chapter_number_selector};
  for (size_t i = 0U; i < (sizeof(selectors) / sizeof(selectors[0])); ++i) {
    const char* selector = selectors[i];
    if (selector[0] == '\0') {
      continue;
    }
    const char* prefixes[] = {"meta:", "class:", "label:", "literal:"};
    bool        valid      = false;
    for (size_t prefix_index = 0U; prefix_index < (sizeof(prefixes) / sizeof(prefixes[0]));
         ++prefix_index) {
      const size_t prefix_len = strlen(prefixes[prefix_index]);
      if ((strncmp(selector, prefixes[prefix_index], prefix_len) == 0) &&
          (selector[prefix_len] != '\0')) {
        valid = true;
        break;
      }
    }
    if (!valid) {
      return false;
    }
  }
  return (site->host[0] != '\0') && (site->language[0] != '\0') &&
         ((strcmp(site->reading_direction, "ltr") == 0) ||
          (strcmp(site->reading_direction, "rtl") == 0)) &&
         (site->img_delay_min <= site->img_delay_max) &&
         (site->chapter_delay_min <= site->chapter_delay_max) && (site->burst > 0U) &&
         (site->max_inflight > 0U) && (site->backoff_base_ms <= site->backoff_max_ms);
}

ra8_err_t mdl_config_load(mdl_storage_t* storage, const char* path, mdl_site_t* out)
{
  if ((storage == nullptr) || (storage->fs == nullptr) || (storage->file_workspace == nullptr) ||
      (storage->io_buffer == nullptr) || (storage->io_buffer_bytes == 0U) || (path == nullptr) ||
      (out == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  internal_config_set_defaults(out);

  internal_config_reader_t reader = {.storage = storage};
  ra8_err_t                err    = fw_fs_open(&storage->fs->streams,
                                               path,
                                               k_fw_fs_open_read,
                                               &reader.file,
                                               storage->file_workspace,
                                               storage->file_workspace_bytes);
  if (err != k_ra8_ok) {
    return err;
  }
  uint64_t file_bytes = 0U;
  err                 = fw_fs_file_size(&reader.file, &file_bytes);
  if ((err == k_ra8_ok) && (file_bytes > (uint64_t)k_config_file_max)) {
    err = k_ra8_err_invalid_size;
  }
  if (err == k_ra8_ok) {
    err = internal_config_parse(&reader, out);
  }
  const ra8_err_t close_err = fw_fs_close(&reader.file);
  if (close_err != k_ra8_ok) {
    return close_err;
  }
  if (err != k_ra8_ok) {
    return err;
  }
  return internal_config_valid(out) ? k_ra8_ok : k_ra8_err_invalid_state;
}

mdl_gov_cfg_t mdl_config_gov_cfg(const mdl_site_t* site)
{
  mdl_gov_cfg_t cfg   = mdl_gov_cfg_default();
  cfg.rate_per_min    = site->rate_per_min;
  cfg.burst           = site->burst;
  cfg.backoff_base_ms = site->backoff_base_ms;
  cfg.backoff_max_ms  = site->backoff_max_ms;
  cfg.max_inflight    = (uint16_t)site->max_inflight;
  return cfg;
}

/**
 * @brief Return the larger of two unsigned values.
 * @details Performs one comparison without arithmetic or overflow risk.
 * @param[in] a First candidate.
 * @param[in] b Second candidate.
 * @return The greater candidate, or their shared value when equal.
 * @retval a @p a is greater than or equal to @p b.
 * @retval b @p b is greater than @p a.
 * @pre Both arguments are valid `uint32_t` values.
 * @pre No external state is required.
 * @post The result is greater than or equal to both inputs.
 * @post Neither input nor global state is modified.
 * @note Thread-safe and side-effect free.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_config_max_u32(uint32_t a, uint32_t b)
{
  return (a > b) ? a : b;
}

void mdl_config_apply_polite(mdl_site_t* site)
{
  site->img_delay_min = internal_config_max_u32(site->img_delay_min, (uint32_t)k_polite_img_min_ms);
  site->img_delay_max = internal_config_max_u32(site->img_delay_max, (uint32_t)k_polite_img_max_ms);
  site->chapter_delay_min =
    internal_config_max_u32(site->chapter_delay_min, (uint32_t)k_polite_chap_min_ms);
  site->chapter_delay_max =
    internal_config_max_u32(site->chapter_delay_max, (uint32_t)k_polite_chap_max_ms);
}
