/**
 * @file mdl_search.h
 * @brief Pure search/discovery policy: query encoding, URL templating, and the
 *        honest zero-vs-broken result classifier.
 *
 * @details
 * The network-free half of `--search` / `--browse` (#304). Three small,
 * side-effect-free functions the discovery orchestrator (::mdl_discover_run)
 * composes with a governed page fetch and ::mdl_extract_hits:
 *
 *  - ::mdl_query_encode percent-encodes a raw search term so a request stays
 *    valid for spaces, `&`, `#`, `+` and non-ASCII UTF-8 bytes. The repo's
 *    pure-7-bit-ASCII rule constrains the *source*, not the runtime bytes: a
 *    term arriving from `argv` may hold UTF-8, and every non-unreserved byte is
 *    emitted as `%HH`.
 *  - ::mdl_search_build_url expands a site descriptor's query template by
 *    substituting the encoded term for the `{q}` placeholder.
 *  - ::mdl_search_classify turns a parsed ::mdl_hit_list_t into one of three
 *    honest outcomes, so an empty result never masquerades as a successful
 *    search and a results page whose markup changed is reported as such.
 *
 * Keeping these pure makes the whole discovery parse path unit-testable against
 * a captured fixture with no network, which is exactly what #304 requires.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mdl_extract.h"

/**
 * @brief The `{q}` placeholder a search-URL template must contain.
 * @return Borrowed pointer to the static placeholder token (`"{q}"`).
 * @retval non-NULL Always: the constant placeholder string.
 * @pre None.
 * @post No state is modified.
 * @note Thread-safe: returns a constant.
 * @since 0.1.0
 */
const char* mdl_search_placeholder(void);

/**
 * @enum mdl_search_outcome_t
 * @brief The honest outcome of a parsed search/browse results page.
 *
 * @details
 * The distinction #304 demands: an empty list must never look like a successful
 * search. ::mdl_search_classify maps the parsed hits plus the raw anchor tally
 * onto exactly one of these, and the CLI prints a distinct message for each.
 *
 * @see mdl_search_classify()
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_mdl_search_have_results   = 0, /**< One or more hits matched; print them.        */
  k_mdl_search_zero_results   = 1, /**< Links present, none matched: a real no-hit.  */
  k_mdl_search_markup_changed = 2, /**< No links at all: the page could not be read. */
} mdl_search_outcome_t;

/**
 * @brief Percent-encode a raw query term for safe inclusion in a URL.
 *
 * @details
 * RFC 3986 encoding: the unreserved set (`A-Z a-z 0-9 - . _ ~`) is copied
 * verbatim and every other byte -- space, `&`, `#`, `+`, `/`, `?`, and each
 * byte of a multi-byte UTF-8 sequence -- is written as `%HH` with upper-case
 * hex. Space becomes `%20` (valid in both path and query), never `+`, so the
 * result is unambiguous wherever the template places it.
 *
 * @param[in]  term Raw term (may contain UTF-8 bytes), NUL-terminated.
 * @param[out] out  Destination buffer for the NUL-terminated encoding.
 * @param[in]  cap  Capacity of @p out in bytes.
 *
 * @return Whether the encoded term (plus its NUL) fit in @p out.
 * @retval true  @p out holds the fully encoded term.
 * @retval false A NULL argument, `cap == 0`, or the encoding did not fit.
 *
 * @pre @p term and @p out are non-NULL; @p out has room for @p cap bytes.
 * @pre The caller treats false as "term too long", not "empty result".
 * @post On false with `cap > 0`, `out[0]` is `'\0'`.
 * @post On true, @p out contains only unreserved bytes and `%HH` triplets.
 *
 * @note Thread-safe: writes only caller-provided storage.
 * @see mdl_search_build_url
 * @since 0.1.0
 */
bool mdl_query_encode(const char* term, char* out, size_t cap);

/**
 * @brief Expand a query-URL template by substituting the encoded term for `{q}`.
 *
 * @details
 * Copies @p tmpl into @p out, replacing every `{q}` placeholder with
 * @p encoded_term (already percent-encoded by ::mdl_query_encode). A template
 * with no `{q}` is rejected -- it could never carry the term, so silently
 * fetching a fixed URL would be a lie about having searched.
 *
 * @param[in]  tmpl         Query-URL template holding at least one `{q}`.
 * @param[in]  encoded_term The percent-encoded term to substitute.
 * @param[out] out          Destination buffer for the NUL-terminated URL.
 * @param[in]  cap          Capacity of @p out in bytes.
 *
 * @return Whether a URL containing the term was produced and fit.
 * @retval true  @p out holds the expanded URL.
 * @retval false A NULL argument, `cap == 0`, no `{q}` in @p tmpl, or overflow.
 *
 * @pre @p tmpl, @p encoded_term and @p out are non-NULL.
 * @pre @p encoded_term came from ::mdl_query_encode (URL-safe bytes only).
 * @post On false with `cap > 0`, `out[0]` is `'\0'`.
 * @post On true, @p out contains @p encoded_term at each former `{q}`.
 *
 * @note Thread-safe: writes only caller-provided storage.
 * @see mdl_query_encode
 * @since 0.1.0
 */
bool mdl_search_build_url(const char* tmpl, const char* encoded_term, char* out, size_t cap);

/**
 * @brief Classify a parsed results page into an honest search outcome.
 *
 * @details
 * The whole point of #304's honesty criterion in one pure decision. With at
 * least one matched hit the outcome is ::k_mdl_search_have_results. With none,
 * the raw anchor tally decides: a page that rendered links but matched none is a
 * genuine ::k_mdl_search_zero_results, while a page carrying no resolvable links
 * at all is ::k_mdl_search_markup_changed -- the endpoint returned something we
 * could not read as a results page (its markup drifted, or the request was
 * blocked or answered with non-HTML). A NULL list is treated as unreadable.
 *
 * @param[in] hits Parsed hit list from ::mdl_extract_hits, or NULL.
 *
 * @return The outcome class for @p hits.
 * @retval k_mdl_search_have_results   `hits->count > 0`.
 * @retval k_mdl_search_zero_results   `count == 0` and `anchors_seen > 0`.
 * @retval k_mdl_search_markup_changed NULL, or `count == 0` and no anchors seen.
 *
 * @pre @p hits, when non-NULL, was filled by ::mdl_extract_hits.
 * @pre The caller prints a distinct message per returned class.
 * @post No state is modified.
 *
 * @note Thread-safe: reads only its argument.
 * @see mdl_extract_hits
 * @since 0.1.0
 */
mdl_search_outcome_t mdl_search_classify(const mdl_hit_list_t* hits);
