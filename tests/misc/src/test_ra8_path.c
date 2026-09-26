/**
 * @file test_ra8_path.c
 * @brief Unit tests for the untrusted-name policy in libs/if.
 *
 * @details
 * Exercises the three predicates an archive reader's caller needs: the segment
 * sanitiser (traversal names, separators, control bytes, reserved device
 * names, truncation, the verbatim report), the containment-proving join (an
 * absolute segment, a `..` segment, a non-fitting result), and the standalone
 * lexical containment predicate (the `/a/bb` sibling trap, trailing slashes).
 *
 * Pure string policy: no filesystem is touched and no register window is
 * installed.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_path.h"
#include "unity_minimal.h"

/**
 * @enum path_test_size_t
 * @brief Fixture capacities used across the cases.
 */
typedef enum : uint16_t {
  k_path_test_cap  = 64U, /**< Comfortable buffer for a segment or join. */
  k_path_test_tiny = 5U,  /**< Buffer that forces truncation.            */
} path_test_size_t;

/**
 * @test test_path_sanitize_rejects_null_out
 * @brief A null destination is refused before anything is classified.
 * @pre None.
 * @post No caller storage is written.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_path_sanitize_rejects_null_out(void)
{
  TEST_BEGIN("path: sanitize null out rejected");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_path_sanitize_segment("ok", nullptr, 8U, nullptr));
  TEST_END("path: sanitize null out rejected");
}

/**
 * @test test_path_sanitize_rejects_tiny_cap
 * @brief A capacity below one character plus a NUL is refused, not truncated.
 * @pre None.
 * @post The destination keeps its poison byte.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_path_sanitize_rejects_tiny_cap(void)
{
  TEST_BEGIN("path: sanitize sub-minimum cap rejected");
  char out[2] = {'Z', 'Z'};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_path_sanitize_segment("ok", out, 1U, nullptr));
  TEST_ASSERT_EQ('Z', out[0]);
  TEST_END("path: sanitize sub-minimum cap rejected");
}

/**
 * @test test_path_sanitize_passes_clean_name
 * @brief An already-safe name is copied verbatim and reported as such.
 * @pre None.
 * @post The destination equals the input.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_path_sanitize_passes_clean_name(void)
{
  TEST_BEGIN("path: clean name copied verbatim");
  char out[k_path_test_cap];
  bool verbatim = false;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_path_sanitize_segment("chapter-012.cbz", out, sizeof(out), &verbatim));
  TEST_ASSERT(verbatim);
  TEST_ASSERT_EQ(0, strcmp(out, "chapter-012.cbz"));
  TEST_END("path: clean name copied verbatim");
}

/**
 * @test test_path_sanitize_neutralises_traversal
 * @brief Separators become underscores and a bare `..` falls back to a name.
 * @pre None.
 * @post No result contains a `/` and no result is `.` or `..`.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_path_sanitize_neutralises_traversal(void)
{
  TEST_BEGIN("path: traversal neutralised");
  char out[k_path_test_cap];
  bool verbatim = true;

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_path_sanitize_segment("../../etc/passwd", out, sizeof(out), &verbatim));
  TEST_ASSERT(!verbatim);
  TEST_ASSERT_NULL(strchr(out, '/'));
  TEST_ASSERT_EQ(0, strcmp(out, ".._.._etc_passwd"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_path_sanitize_segment("..", out, sizeof(out), &verbatim));
  TEST_ASSERT(!verbatim);
  TEST_ASSERT_EQ(0, strcmp(out, "item"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_path_sanitize_segment("", out, sizeof(out), &verbatim));
  TEST_ASSERT_EQ(0, strcmp(out, "item"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_path_sanitize_segment(nullptr, out, sizeof(out), &verbatim));
  TEST_ASSERT(!verbatim);
  TEST_ASSERT_EQ(0, strcmp(out, "item"));
  TEST_END("path: traversal neutralised");
}

/**
 * @test test_path_sanitize_prefixes_reserved_base
 * @brief A reserved device base is prefixed rather than passed through.
 * @pre None.
 * @post The result begins with an underscore.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_path_sanitize_prefixes_reserved_base(void)
{
  TEST_BEGIN("path: reserved device base prefixed");
  char out[k_path_test_cap];
  bool verbatim = true;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_path_sanitize_segment("NUL", out, sizeof(out), &verbatim));
  TEST_ASSERT(!verbatim);
  TEST_ASSERT_EQ(0, strcmp(out, "_NUL"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_path_sanitize_segment("com4.txt", out, sizeof(out), &verbatim));
  TEST_ASSERT_EQ(0, strcmp(out, "_com4.txt"));

  /* `com0` is not reserved: the suffix range is 1-9. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_path_sanitize_segment("com0", out, sizeof(out), &verbatim));
  TEST_ASSERT(verbatim);
  TEST_ASSERT_EQ(0, strcmp(out, "com0"));
  TEST_END("path: reserved device base prefixed");
}

/**
 * @test test_path_sanitize_truncates_and_reports
 * @brief An over-long candidate is truncated and never reported verbatim.
 * @pre None.
 * @post The result is NUL-terminated within the supplied capacity.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_path_sanitize_truncates_and_reports(void)
{
  TEST_BEGIN("path: over-long candidate truncated");
  char out[k_path_test_tiny];
  bool verbatim = true;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_path_sanitize_segment("abcdefghij", out, sizeof(out), &verbatim));
  TEST_ASSERT(!verbatim);
  TEST_ASSERT_EQ(0, strcmp(out, "abcd"));
  TEST_END("path: over-long candidate truncated");
}

/**
 * @test test_path_join_refuses_unsafe_segment
 * @brief An absolute, dotted or separator-bearing segment is refused outright.
 * @pre None.
 * @post The destination is emptied so no partial path can be used.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_path_join_refuses_unsafe_segment(void)
{
  TEST_BEGIN("path: unsafe join segment refused");
  char out[k_path_test_cap];

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_path_join_under("/books", "/etc/passwd", out, sizeof(out)));
  TEST_ASSERT_EQ('\0', out[0]);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_path_join_under("/books", "..", out, sizeof(out)));
  TEST_ASSERT_EQ('\0', out[0]);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_path_join_under("/books", "a/b", out, sizeof(out)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_path_join_under("/books", "", out, sizeof(out)));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_path_join_under("/books", nullptr, out, sizeof(out)));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_path_join_under("/books", "ok", nullptr, sizeof(out)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_path_join_under("/books", "ok", out, 0U));
  TEST_END("path: unsafe join segment refused");
}

/**
 * @test test_path_join_composes_and_bounds
 * @brief A safe segment composes, and a non-fitting result is refused.
 * @pre None.
 * @post A refused join leaves an empty destination.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_path_join_composes_and_bounds(void)
{
  TEST_BEGIN("path: join composes, refuses truncation");
  char out[k_path_test_cap];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_path_join_under("/books/incoming", "ch1.cbz", out, sizeof(out)));
  TEST_ASSERT_EQ(0, strcmp(out, "/books/incoming/ch1.cbz"));

  char tight[10];
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_path_join_under("/books", "chapter.cbz", tight,
                                                       sizeof(tight)));
  TEST_ASSERT_EQ('\0', tight[0]);
  TEST_END("path: join composes, refuses truncation");
}

/**
 * @test test_path_contained_honours_boundary
 * @brief Containment treats a directory boundary as significant.
 * @pre None.
 * @post Neither input string is modified.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_path_contained_honours_boundary(void)
{
  TEST_BEGIN("path: containment honours directory boundary");
  bool inside = false;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_path_contained("/a/b", "/a/b", &inside));
  TEST_ASSERT(inside);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_path_contained("/a/b", "/a/b/c", &inside));
  TEST_ASSERT(inside);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_path_contained("/a/b", "/a/bb", &inside));
  TEST_ASSERT(!inside);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_path_contained("/a/b///", "/a/b/c", &inside));
  TEST_ASSERT(inside);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_path_contained("/a/b", "/etc/passwd", &inside));
  TEST_ASSERT(!inside);
  TEST_END("path: containment honours directory boundary");
}

/**
 * @test test_path_contained_rejects_bad_arguments
 * @brief Null arguments and an all-slash parent are refused, not answered.
 * @pre None.
 * @post The verdict is left unwritten on every refusal.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_path_contained_rejects_bad_arguments(void)
{
  TEST_BEGIN("path: containment rejects bad arguments");
  bool inside = false;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_path_contained(nullptr, "/a", &inside));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_path_contained("/a", nullptr, &inside));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_path_contained("/a", "/a", nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_path_contained("", "/a", &inside));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_path_contained("///", "/a", &inside));
  TEST_END("path: containment rejects bad arguments");
}

/**
 * @test test_path_sanitised_segment_cannot_escape
 * @brief The two calls compose: a sanitised name always stays under its parent.
 * @pre None.
 * @post The joined result is contained under the parent.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_path_sanitised_segment_cannot_escape(void)
{
  TEST_BEGIN("path: sanitise then join cannot escape");
  static const char* const hostile[] = {"../../etc/passwd", "..", "/absolute", "a/b/c", "NUL"};

  for (size_t i = 0U; i < (sizeof(hostile) / sizeof(hostile[0])); ++i) {
    char leaf[k_path_test_cap];
    TEST_ASSERT_EQ(k_ra8_ok, ra8_path_sanitize_segment(hostile[i], leaf, sizeof(leaf), nullptr));

    char dest[k_ra8_path_cap];
    TEST_ASSERT_EQ(k_ra8_ok, ra8_path_join_under("/books/in", leaf, dest, sizeof(dest)));

    bool inside = false;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_path_contained("/books/in", dest, &inside));
    TEST_ASSERT(inside);
  }
  TEST_END("path: sanitise then join cannot escape");
}

/**
 * @brief Test binary entry point.
 * @return 0 on success; a failing assertion exits non-zero first.
 * @pre None.
 * @post Every case above has run in order.
 * @note Not thread-safe; single-threaded test runner.
 * @since 0.1.0
 */
int main(void)
{
  test_path_sanitize_rejects_null_out();
  test_path_sanitize_rejects_tiny_cap();
  test_path_sanitize_passes_clean_name();
  test_path_sanitize_neutralises_traversal();
  test_path_sanitize_prefixes_reserved_base();
  test_path_sanitize_truncates_and_reports();
  test_path_join_refuses_unsafe_segment();
  test_path_join_composes_and_bounds();
  test_path_contained_honours_boundary();
  test_path_contained_rejects_bad_arguments();
  test_path_sanitised_segment_cannot_escape();
  return 0;
}
