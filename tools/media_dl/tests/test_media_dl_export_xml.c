/**
 * @file test_media_dl_export_xml.c
 * @brief Export XML escaping boundary tests.
 * @details Isolates the pure XML utility contract from archive construction
 *          and storage-backed round-trip cases.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "mdl_sanitize.h"
#include "test_media_dl_export_xml_internal.h"
#include "unity_minimal.h"

/** @brief Escaped XML probe capacity. */
typedef enum : uint16_t {
  k_xml_escape_bytes = 256U, /**< Complete escaped probe bytes. */
} mdl_export_xml_limit_t;

/**
 * @test XML escaper replaces metacharacters and fails rather than truncating.
 * @brief Exercise the xml escape regression scenario.
 * @details Executes the xml escape scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_xml_escape(void)
{
  TEST_BEGIN("xml escape");
  char out[k_xml_escape_bytes];
  TEST_ASSERT(mdl_xml_escape("a&b<c>\"'", out, sizeof(out)));
  TEST_ASSERT(strcmp(out, "a&amp;b&lt;c&gt;&quot;&apos;") == 0);
  TEST_ASSERT(mdl_xml_escape("page_001.jpg", out, sizeof(out))); /* legal name kept */
  TEST_ASSERT(strcmp(out, "page_001.jpg") == 0);
  char tiny[4];
  TEST_ASSERT(!mdl_xml_escape("&&&", tiny, sizeof(tiny))); /* would not fit -> fail */
  TEST_END("xml escape");
}

/**
 * @brief Run the export XML escaping test group.
 * @pre The unity-minimal assertion process is initialized.
 * @pre The caller owns any process-wide fixture binding used by the group.
 * @post Normal return means every group assertion passed.
 * @post No fixture ownership transfers to the caller.
 * @note Test-target-private and synchronous.
 * @since 0.1.0
 */
RA8_PRIV void priv_test_mdl_export_xml_run(void)
{
  internal_test_xml_escape();
}
