/**
 * @file test_ra_reflow_xml_shim.cpp
 * @brief MC/DC vector tests for libs/ra_reflow/src/ra_reflow_xml_shim.cpp.
 *
 * @par Tag
 * [Ring 4 / Reflow] {World: NS}
 *
 * @details
 * Drives the three internal helpers exposed via
 * @c libs/ra_reflow/src/ra_reflow_xml_shim_internal.hpp directly so
 * MC/DC vectors land on the production decisions at xml_shim.cpp
 * lines 70, 72, 232 and 334 -- the four reachable rows listed in
 * @c docs/MCDC_GAPS.csv.
 *
 * Per CLAUDE.md the new compound decisions live inside
 * @c lowercase_truncated_copy, @c is_xml_whitespace and
 * @c should_reject_null_args. The production source has been refactored
 * to delegate so the original lines 70/72/232/334 still cite the
 * correct file:line via the helper's body.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ra_reflow_xml_shim_internal.hpp"

namespace {

/**
 * @test test_mcdc_lowercase_loop_bound
 *
 * @par MC/DC:
 * Decision at libs/ra_reflow/src/ra_reflow_xml_shim.cpp:70 (helper):
 *   ``while (tail[i] != '\0' && i + 1U < dst_cap)`` (2-cond AND).
 * - V1: src="ab", cap=8     -> exits on '\0'  (C1=F at end) [C1-pair].
 * - V2: src="aa..a"(8), cap=4 -> exits on cap (C1=T C2=F)  [C2-pair].
 * - V3 (control): single char src="a", cap=8 -> stays in loop once.
 * V1+V3 isolate C1; V2+V3 isolate C2. N+1 = 3.
 */
void test_mcdc_lowercase_loop_bound()
{
  char dst[16];
  /* V1: short string, cap large -> exit via NUL. */
  std::size_t n1 = ra_reflow_xml_shim_internal::lowercase_truncated_copy("ab", dst, sizeof(dst));
  if (n1 != 2U || std::strcmp(dst, "ab") != 0) {
    std::fprintf(stderr, "[FAIL] V1 lowercase_loop_bound: got '%s'\n", dst);
    std::abort();
  }
  /* V2: long string, cap small -> exit via cap (write cap-1 chars). */
  char        small[4];
  std::size_t n2 =
    ra_reflow_xml_shim_internal::lowercase_truncated_copy("aaaaaaaa", small, sizeof(small));
  if (n2 != 3U || std::strcmp(small, "aaa") != 0) {
    std::fprintf(stderr, "[FAIL] V2 lowercase_loop_bound: got '%s'\n", small);
    std::abort();
  }
  /* V3: single char control. */
  std::size_t n3 = ra_reflow_xml_shim_internal::lowercase_truncated_copy("a", dst, sizeof(dst));
  if (n3 != 1U || std::strcmp(dst, "a") != 0) {
    std::fprintf(stderr, "[FAIL] V3 lowercase_loop_bound: got '%s'\n", dst);
    std::abort();
  }
}

/**
 * @test test_mcdc_uppercase_check
 *
 * @par MC/DC:
 * Decision at libs/ra_reflow/src/ra_reflow_xml_shim.cpp:72 (helper):
 *   ``if (c >= 'A' && c <= 'Z')`` (2-cond AND).
 * - V1: c='A' -> C1=T C2=T -> lowercased.
 * - V2: c='@' -> C1=F      -> kept as-is (varies C1).
 * - V3: c='[' -> C1=T C2=F -> kept as-is (varies C2).
 * V1+V2 isolate C1; V1+V3 isolate C2. N+1 = 3.
 */
void test_mcdc_uppercase_check()
{
  char dst[8];
  ra_reflow_xml_shim_internal::lowercase_truncated_copy("A", dst, sizeof(dst));
  if (dst[0] != 'a') {
    std::fprintf(stderr, "[FAIL] V1 uppercase_check: 'A' -> '%c'\n", dst[0]);
    std::abort();
  }
  ra_reflow_xml_shim_internal::lowercase_truncated_copy("@", dst, sizeof(dst));
  if (dst[0] != '@') {
    std::fprintf(stderr, "[FAIL] V2 uppercase_check: '@' -> '%c'\n", dst[0]);
    std::abort();
  }
  ra_reflow_xml_shim_internal::lowercase_truncated_copy("[", dst, sizeof(dst));
  if (dst[0] != '[') {
    std::fprintf(stderr, "[FAIL] V3 uppercase_check: '[' -> '%c'\n", dst[0]);
    std::abort();
  }
}

/**
 * @test test_mcdc_is_xml_whitespace
 *
 * @par MC/DC:
 * Decision at libs/ra_reflow/src/ra_reflow_xml_shim.cpp:232 (helper):
 *   ``c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'
 *     || c == '\v'`` (6-cond OR). N+1 = 7 vectors:
 *  - V1: 'a'   -> all-false control.
 *  - V2: ' '   -> C1=T (varies C1 vs V1).
 *  - V3: '\t'  -> C2=T (varies C2 vs V1).
 *  - V4: '\n'  -> C3=T.
 *  - V5: '\r'  -> C4=T.
 *  - V6: '\f'  -> C5=T.
 *  - V7: '\v'  -> C6=T.
 * V1+V_i isolates C_i for each i in 1..6.
 */
void test_mcdc_is_xml_whitespace()
{
  if (ra_reflow_xml_shim_internal::is_xml_whitespace('a')) {
    std::fprintf(stderr, "[FAIL] V1 is_xml_whitespace: 'a' classified as ws\n");
    std::abort();
  }
  const char ws[] = {' ', '\t', '\n', '\r', '\f', '\v'};
  for (std::size_t i = 0U; i < sizeof(ws); ++i) {
    if (!ra_reflow_xml_shim_internal::is_xml_whitespace(ws[i])) {
      std::fprintf(stderr,
                   "[FAIL] V%zu is_xml_whitespace: 0x%02x not classified\n",
                   i + 2U,
                   static_cast<unsigned>(ws[i]));
      std::abort();
    }
  }
}

/**
 * @test test_mcdc_should_reject_null_args
 *
 * @par MC/DC:
 * Decision at libs/ra_reflow/src/ra_reflow_xml_shim.cpp:334 (helper):
 *   ``engine == nullptr || xhtml_buf == nullptr`` (2-cond OR).
 * - V1: both non-null -> false (control).
 * - V2: engine=null   -> true  (varies engine).
 * - V3: buf=null      -> true  (varies buf).
 * V1+V2 isolate engine; V1+V3 isolate buf. N+1 = 3.
 */
void test_mcdc_should_reject_null_args()
{
  if (ra_reflow_xml_shim_internal::should_reject_null_args(false, false)) {
    std::fprintf(stderr, "[FAIL] V1 should_reject: both non-null but rejected\n");
    std::abort();
  }
  if (!ra_reflow_xml_shim_internal::should_reject_null_args(true, false)) {
    std::fprintf(stderr, "[FAIL] V2 should_reject: engine null but accepted\n");
    std::abort();
  }
  if (!ra_reflow_xml_shim_internal::should_reject_null_args(false, true)) {
    std::fprintf(stderr, "[FAIL] V3 should_reject: buf null but accepted\n");
    std::abort();
  }
}

} /* namespace */

int main()
{
  test_mcdc_lowercase_loop_bound();
  test_mcdc_uppercase_check();
  test_mcdc_is_xml_whitespace();
  test_mcdc_should_reject_null_args();
  std::fprintf(stderr, "[OK ] test_ra_reflow_xml_shim.cpp\n");
  return 0;
}
