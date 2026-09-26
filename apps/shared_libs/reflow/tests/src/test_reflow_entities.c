/**
 * @file test_reflow_entities.c
 * @brief Unit tests for apps/shared_libs/reflow/src/reflow_tokenize_entities.c
 *
 * @details
 * Two halves. The first walks the whole XHTML 1.0 named character
 * reference table and asserts the invariants the binary search depends on
 * (byte-order sort, no duplicates, every name inside the tokenizer's
 * `&...;` scan window, every code point a valid scalar value), so a row
 * added out of order or with an unreachable name fails here rather than
 * silently never matching. The second drives
 * `priv_reflow_tok_lookup_entity` and `priv_reflow_tok_decode_entity` over
 * the references publisher prose actually uses, the five the tokenizer
 * knew before, the case-sensitivity rule, and the malformed shapes that
 * must fail open. See issue #686 and ADR-0010.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "reflow.h"
#include "reflow_tokenize_internal.h"
#include "unity_minimal.h"

/**
 * @enum t_entity_t
 * @brief Expected table size and the scan-window bound under test.
 */
typedef enum : uint32_t {
  k_t_entity_rows = 253U,      /**< XHTML 1.0 sets (252) plus XML apos. */
  k_t_cp_max      = 0x10FFFFU, /**< Highest valid Unicode code point.   */
} t_entity_t;

/**
 * @brief Assert the table's sort order, name bound and code-point range.
 *
 * @details Walks every row once, comparing each name with its predecessor
 * under plain byte order (the order the binary search assumes) and
 * checking the name is non-empty, within the scan window, and maps to a
 * valid scalar value.
 * @pre The table is linked into the test binary.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_table_invariants(void)
{
  TEST_BEGIN("entity table invariants");
  const size_t rows = priv_reflow_tok_entity_count();
  TEST_ASSERT_EQ((size_t)k_t_entity_rows, rows);

  const char* prev = nullptr;
  for (size_t i = 0U; i < rows; ++i) {
    const char* name = nullptr;
    uint32_t    cp   = 0U;
    TEST_ASSERT(priv_reflow_tok_entity_at(i, &name, &cp));
    const size_t len = strlen(name);
    TEST_ASSERT(len > 0U);
    TEST_ASSERT(len <= (size_t)k_priv_entity_name_max);
    TEST_ASSERT(cp > 0U);
    TEST_ASSERT(cp <= (uint32_t)k_t_cp_max);
    if (prev != nullptr) {
      TEST_ASSERT(strcmp(prev, name) < 0); /* sorted and no duplicates */
    }
    prev = name;
  }
  TEST_ASSERT(!priv_reflow_tok_entity_at(rows, &prev, &(uint32_t){0U}));
  TEST_END("entity table invariants");
}

/**
 * @brief Assert every table row is reachable through the lookup.
 *
 * @details Looks each name up by its own bytes and requires the code point
 * back, which is what proves the search agrees with the table across all
 * of it rather than at the handful of sampled names below.
 * @pre The table is linked into the test binary.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_every_row_found(void)
{
  TEST_BEGIN("every entity row is reachable");
  const size_t rows = priv_reflow_tok_entity_count();
  for (size_t i = 0U; i < rows; ++i) {
    const char* name = nullptr;
    uint32_t    want = 0U;
    TEST_ASSERT(priv_reflow_tok_entity_at(i, &name, &want));
    uint32_t got = 0U;
    TEST_ASSERT(priv_reflow_tok_lookup_entity(name, strlen(name), &got));
    TEST_ASSERT_EQ(want, got);
  }
  TEST_END("every entity row is reachable");
}

/**
 * @brief Drive the lookup over known names, rejects and the length bound.
 *
 * @details Covers the punctuation and Latin-1 references publisher prose
 * uses, exact case sensitivity in both directions, the empty name, an
 * unknown name, and a name longer than the scan window admits.
 * @pre The table is linked into the test binary.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_lookup(void)
{
  TEST_BEGIN("entity lookup");
  uint32_t cp = 0U;
  TEST_ASSERT(priv_reflow_tok_lookup_entity("nbsp", 4U, &cp));
  TEST_ASSERT_EQ(0x00A0U, cp);
  TEST_ASSERT(priv_reflow_tok_lookup_entity("mdash", 5U, &cp));
  TEST_ASSERT_EQ(0x2014U, cp);
  TEST_ASSERT(priv_reflow_tok_lookup_entity("rsquo", 5U, &cp));
  TEST_ASSERT_EQ(0x2019U, cp);
  TEST_ASSERT(priv_reflow_tok_lookup_entity("hellip", 6U, &cp));
  TEST_ASSERT_EQ(0x2026U, cp);
  TEST_ASSERT(priv_reflow_tok_lookup_entity("thetasym", 8U, &cp)); /* longest name */
  TEST_ASSERT_EQ(0x03D1U, cp);

  /* Case-sensitive in both directions. */
  TEST_ASSERT(priv_reflow_tok_lookup_entity("eacute", 6U, &cp));
  TEST_ASSERT_EQ(0x00E9U, cp);
  TEST_ASSERT(priv_reflow_tok_lookup_entity("Eacute", 6U, &cp));
  TEST_ASSERT_EQ(0x00C9U, cp);
  TEST_ASSERT(!priv_reflow_tok_lookup_entity("AMP", 3U, &cp));

  /* Rejects: empty, unknown, prefix of a real name, over the window bound. */
  TEST_ASSERT(!priv_reflow_tok_lookup_entity("nbsp", 0U, &cp));
  TEST_ASSERT(!priv_reflow_tok_lookup_entity("notaname", 8U, &cp));
  TEST_ASSERT(!priv_reflow_tok_lookup_entity("hell", 4U, &cp));
  TEST_ASSERT(!priv_reflow_tok_lookup_entity("thetasymbol", 11U, &cp));
  TEST_END("entity lookup");
}

/**
 * @brief Drive the decoder over whole `&...;` sequences.
 *
 * @details Checks the code point and the consumed length together, keeps
 * the five references the tokenizer knew before working, holds the numeric
 * forms unchanged, and requires every malformed shape to fail open so the
 * caller emits a literal '&'.
 * @pre The table is linked into the test binary.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_decode_named(void)
{
  TEST_BEGIN("decode named references");
  uint32_t cp   = 0U;
  size_t   used = 0U;

  TEST_ASSERT(priv_reflow_tok_decode_entity("&nbsp;x", 7U, &cp, &used));
  TEST_ASSERT_EQ(0x00A0U, cp);
  TEST_ASSERT_EQ((size_t)6U, used);
  TEST_ASSERT(priv_reflow_tok_decode_entity("&eacute;", 8U, &cp, &used));
  TEST_ASSERT_EQ(0x00E9U, cp);
  TEST_ASSERT_EQ((size_t)8U, used);
  TEST_ASSERT(priv_reflow_tok_decode_entity("&ldquo;", 7U, &cp, &used));
  TEST_ASSERT_EQ(0x201CU, cp);

  /* The five the tokenizer knew before this table. */
  TEST_ASSERT(priv_reflow_tok_decode_entity("&amp;", 5U, &cp, &used));
  TEST_ASSERT_EQ((uint32_t)'&', cp);
  TEST_ASSERT_EQ((size_t)5U, used);
  TEST_ASSERT(priv_reflow_tok_decode_entity("&lt;", 4U, &cp, &used));
  TEST_ASSERT_EQ((uint32_t)'<', cp);
  TEST_ASSERT(priv_reflow_tok_decode_entity("&gt;", 4U, &cp, &used));
  TEST_ASSERT_EQ((uint32_t)'>', cp);
  TEST_ASSERT(priv_reflow_tok_decode_entity("&quot;", 6U, &cp, &used));
  TEST_ASSERT_EQ((uint32_t)'"', cp);
  TEST_ASSERT(priv_reflow_tok_decode_entity("&apos;", 6U, &cp, &used));
  TEST_ASSERT_EQ((uint32_t)'\'', cp);

  /* Numeric forms are untouched by this change. */
  TEST_ASSERT(priv_reflow_tok_decode_entity("&#65;", 5U, &cp, &used));
  TEST_ASSERT_EQ((uint32_t)'A', cp);
  TEST_ASSERT(priv_reflow_tok_decode_entity("&#x41;", 6U, &cp, &used));
  TEST_ASSERT_EQ((uint32_t)'A', cp);

  /* Fail-open shapes. */
  TEST_ASSERT(!priv_reflow_tok_decode_entity("&nbsp", 5U, &cp, &used));
  TEST_ASSERT(!priv_reflow_tok_decode_entity("&;xx", 4U, &cp, &used));
  TEST_ASSERT(!priv_reflow_tok_decode_entity("&Nbsp;", 6U, &cp, &used));
  TEST_ASSERT(!priv_reflow_tok_decode_entity("&notaname;", 10U, &cp, &used));
  /* Name longer than the 12-byte window: no ';' is seen, so no match. */
  TEST_ASSERT(!priv_reflow_tok_decode_entity("&averylongname;", 15U, &cp, &used));
  TEST_END("decode named references");
}

int main(void)
{
  internal_test_table_invariants();
  internal_test_every_row_found();
  internal_test_lookup();
  internal_test_decode_named();
  return 0;
}
