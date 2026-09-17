/**
 * @file test_mdl_sites.c
 * @brief Qualification fixtures for every shipped mdl site descriptor.
 * @details Exercises discovery, chapter/image extraction, and descriptor-driven
 * metadata selectors against captured current-site HTML for every descriptor.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "mdl_config.h"
#include "mdl_extract.h"
#include "mdl_search.h"
#include "mdl_test_storage.h"
#include "mdl_urlname.h"
#include "ra8_test_output.h"
#include "unity_minimal.h"

#ifndef MDL_SITE_CONFIG_DIR
#error "MDL_SITE_CONFIG_DIR must name the shipped descriptor directory"
#endif
#ifndef MDL_SITE_FIXTURE_DIR
#error "MDL_SITE_FIXTURE_DIR must name the captured fixture directory"
#endif
#ifndef MDL_SHIPPED_SITE_COUNT
#error "MDL_SHIPPED_SITE_COUNT must count checked-in descriptors"
#endif

typedef enum : uint32_t {
  k_fixture_cap          = 32768U, /**< Captured HTML scratch capacity.       */
  k_tiny_metadata_cap    = 8U,     /**< Deliberately undersized value buffer. */
  k_peppercarrot_rate_pm = 20U,    /**< Shipped nonzero governor rate.        */
} site_test_limit_t;
static char           s_html[k_fixture_cap];
static mdl_hit_list_t s_hits;
static mdl_url_list_t s_urls;

/**
 * @brief Read the fixture fixture.
 * @details Executes the read fixture scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @param[in] leaf Leaf value for this operation.
 * @return The bounded result computed from the supplied input.
 * @retval other The computed result in the function's declared domain.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t internal_read_fixture(const char* leaf)
{
  char path[1024];
  TEST_ASSERT(__builtin_snprintf(path, sizeof(path), "%s/%s", MDL_SITE_FIXTURE_DIR, leaf) > 0);
  const int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  TEST_ASSERT(descriptor >= 0);
  size_t got = 0U;
  while (got < (sizeof(s_html) - 1U)) {
    const ssize_t read_bytes = read(descriptor, &s_html[got], sizeof(s_html) - 1U - got);
    if (read_bytes > 0) {
      got += (size_t)read_bytes;
    } else if ((read_bytes < 0) && (errno == EINTR)) {
      continue;
    } else {
      break;
    }
  }
  TEST_ASSERT(close(descriptor) == 0);
  TEST_ASSERT(got > 0U);
  TEST_ASSERT(got < (sizeof(s_html) - 1U));
  s_html[got] = '\0';
  return got;
}

/**
 * @brief Check ManhwaUS chapter image and title extraction.
 * @details Executes the manhwaus chapter scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @param[in] site Selected site descriptor.
 * @param[out] metadata Metadata value for this operation.
 * @param[in] metadata_cap Metadata cap value for this operation.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_test_manhwaus_chapter(const mdl_site_t* site, char* metadata, size_t metadata_cap)
{
  const size_t len = internal_read_fixture("manhwaus_chapter.html");
  TEST_ASSERT(mdl_extract_images(s_html,
                                 len,
                                 "https://manhwaus.net/webtoon/player/chapter-108-5/",
                                 site->page_img_attr,
                                 site->page_img_url_contains,
                                 &s_urls) == k_ra8_ok);
  TEST_ASSERT_EQ((uint16_t)2, s_urls.count);
  TEST_ASSERT(strncmp(s_urls.urls[0], "https://ii", strlen("https://ii")) == 0);
  TEST_ASSERT(strncmp(s_urls.urls[1], "https://ii", strlen("https://ii")) == 0);
  TEST_ASSERT(strstr(s_urls.urls[0], "/online/") != nullptr);
  TEST_ASSERT(strstr(s_urls.urls[1], "/chapters/") != nullptr);
  TEST_ASSERT(
    mdl_extract_selector(s_html, len, site->chapter_title_selector, metadata, metadata_cap) ==
    k_ra8_ok);
  TEST_ASSERT(strcmp(metadata, "Chapter 108.5 - The Return") == 0);
}

/**
 * @brief Check ManhwaUS discovery filtering.
 * @details Executes the manhwaus discovery scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @param[in] site Selected site descriptor.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_manhwaus_discovery(const mdl_site_t* site)
{
  const size_t len = internal_read_fixture("manhwaus_discovery.html");
  TEST_ASSERT(
    mdl_extract_hits(s_html, len, "https://manhwaus.net/", site->search_result_contains, &s_hits) ==
    k_ra8_ok);
  TEST_ASSERT_EQ((uint16_t)4, s_hits.count);
  TEST_ASSERT_EQ((uint16_t)2, mdl_search_filter_series_hits(&s_hits, site->chapter_url_contains));
  TEST_ASSERT_EQ((uint16_t)2, s_hits.count);
  TEST_ASSERT(strcmp(s_hits.hits[0].url, "https://manhwaus.net/webtoon/player/") == 0);
  TEST_ASSERT(strcmp(s_hits.hits[1].url, "https://manhwaus.net/webtoon/alpha/") == 0);
}

/**
 * @brief Check ManhwaUS series links and metadata selectors.
 * @details Executes the manhwaus series scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @param[in] site Selected site descriptor.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_manhwaus_series(const mdl_site_t* site)
{
  const size_t len = internal_read_fixture("manhwaus_series.html");
  TEST_ASSERT(mdl_extract_anchors(s_html,
                                  len,
                                  "https://manhwaus.net/webtoon/player/",
                                  site->chapter_url_contains,
                                  &s_urls) == k_ra8_ok);
  TEST_ASSERT_EQ((uint16_t)2, s_urls.count);
  TEST_ASSERT(mdl_urlname_chapter_value(s_urls.urls[0]) == 109.0);
  TEST_ASSERT(mdl_urlname_chapter_value(s_urls.urls[1]) == 108.5);
  char metadata[k_mdl_url_max];
  TEST_ASSERT(
    mdl_extract_selector(s_html, len, site->series_title_selector, metadata, sizeof(metadata)) ==
    k_ra8_ok);
  TEST_ASSERT(strcmp(metadata, "The Player Who Returned 10,000 Years Later") == 0);
  TEST_ASSERT(mdl_extract_selector(s_html, len, "class:post-title", metadata, sizeof(metadata)) ==
              k_ra8_ok);
  TEST_ASSERT(strcmp(metadata, "The Player Who Returned 10,000 Years Later") == 0);
  TEST_ASSERT(
    mdl_extract_selector(s_html, len, site->series_summary_selector, metadata, sizeof(metadata)) ==
    k_ra8_ok);
  TEST_ASSERT(strcmp(metadata, "A player returns & faces a changed world.") == 0);
  TEST_ASSERT(
    mdl_extract_selector(s_html, len, site->series_author_selector, metadata, sizeof(metadata)) ==
    k_ra8_ok);
  TEST_ASSERT(strcmp(metadata, "Butterfly Valley") == 0);
  TEST_ASSERT(
    mdl_extract_selector(s_html, len, site->series_artist_selector, metadata, sizeof(metadata)) ==
    k_ra8_ok);
  TEST_ASSERT(strcmp(metadata, "Maeng Ju-gong") == 0);
  char cover[k_mdl_url_max];
  TEST_ASSERT(
    mdl_extract_selector(s_html, len, site->series_cover_selector, metadata, sizeof(metadata)) ==
    k_ra8_ok);
  TEST_ASSERT(mdl_extract_resolve_url("https://manhwaus.net/webtoon/player/",
                                      metadata,
                                      cover,
                                      sizeof(cover)));
  TEST_ASSERT(strcmp(cover, "https://manhwaus.net/uploads/player/cover.png") == 0);
  char tiny[k_tiny_metadata_cap];
  TEST_ASSERT(mdl_extract_selector(s_html, len, site->series_title_selector, tiny, sizeof(tiny)) ==
              k_ra8_err_invalid_size);
  TEST_ASSERT(mdl_extract_selector(s_html, len, "css:.post-title", metadata, sizeof(metadata)) ==
              k_ra8_err_invalid_arg);
  internal_test_manhwaus_chapter(site, metadata, sizeof(metadata));
}

/**
 * @test The ManhwaUS descriptor matches captured discovery and reader HTML.
 * @brief Exercise the manhwaus descriptor regression scenario.
 * @details Executes the manhwaus descriptor scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_manhwaus_descriptor(void)
{
  TEST_BEGIN("shipped descriptor: manhwaus");
  char config_path[1024];
  TEST_ASSERT(
    __builtin_snprintf(config_path, sizeof(config_path), "%s/manhwaus.conf", MDL_SITE_CONFIG_DIR) >
    0);
  mdl_site_t site;
  TEST_ASSERT(mdl_config_load(mdl_test_storage_get(), config_path, &site) == k_ra8_ok);
  TEST_ASSERT(strcmp(site.host, "manhwaus.net") == 0);
  TEST_ASSERT(strcmp(site.search_url, "https://manhwaus.net/search/?s={q}") == 0);
  TEST_ASSERT(site.browse_url[0] != '\0');
  internal_test_manhwaus_discovery(&site);
  internal_test_manhwaus_series(&site);
  TEST_END("shipped descriptor: manhwaus");
}

/** @test The Pepper&Carrot descriptor matches its official archive and episode
 * @brief Exercise the peppercarrot discovery regression scenario.
 * @details Executes the peppercarrot discovery scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @param[in] site Selected site descriptor.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 * HTML. */
RA8_INTERNAL static void internal_test_peppercarrot_discovery(const mdl_site_t* site)
{
  const size_t len = internal_read_fixture("peppercarrot_discovery.html");
  TEST_ASSERT(mdl_extract_hits(s_html,
                               len,
                               "https://www.peppercarrot.com/en/webcomics/index.html",
                               site->search_result_contains,
                               &s_hits) == k_ra8_ok);
  TEST_ASSERT_EQ((uint16_t)1, s_hits.count);
  TEST_ASSERT_EQ((uint16_t)0, mdl_search_filter_series_hits(&s_hits, site->chapter_url_contains));
  TEST_ASSERT(
    strcmp(s_hits.hits[0].url, "https://www.peppercarrot.com/en/webcomics/peppercarrot.html") == 0);
}

/**
 * @brief Check Pepper&Carrot series links and metadata selectors.
 * @details Executes the peppercarrot series scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @param[in] site Selected site descriptor.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_peppercarrot_series(const mdl_site_t* site)
{
  const size_t len = internal_read_fixture("peppercarrot_series.html");
  TEST_ASSERT(mdl_extract_anchors(s_html,
                                  len,
                                  "https://www.peppercarrot.com/en/webcomics/peppercarrot.html",
                                  site->chapter_url_contains,
                                  &s_urls) == k_ra8_ok);
  TEST_ASSERT_EQ((uint16_t)2, s_urls.count);
  TEST_ASSERT(mdl_urlname_chapter_number(s_urls.urls[0]) == 39L);
  TEST_ASSERT(mdl_urlname_chapter_number(s_urls.urls[1]) == 38L);
  TEST_ASSERT(strncmp(s_urls.urls[0], site->chapter_url_prefix, strlen(site->chapter_url_prefix)) ==
              0);
  char metadata[k_mdl_url_max];
  TEST_ASSERT(
    mdl_extract_selector(s_html, len, site->series_title_selector, metadata, sizeof(metadata)) ==
    k_ra8_ok);
  TEST_ASSERT(strcmp(metadata, "Pepper & Carrot") == 0);
  TEST_ASSERT(
    mdl_extract_selector(s_html, len, site->series_author_selector, metadata, sizeof(metadata)) ==
    k_ra8_ok);
  TEST_ASSERT(strcmp(metadata, "David Revoy") == 0);
}

/**
 * @brief Check Pepper&Carrot chapter image and title extraction.
 * @details Executes the peppercarrot chapter scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @param[in] site Selected site descriptor.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_peppercarrot_chapter(const mdl_site_t* site)
{
  const size_t len = internal_read_fixture("peppercarrot_chapter.html");
  TEST_ASSERT(mdl_extract_images(s_html,
                                 len,
                                 "https://www.peppercarrot.com/en/webcomic/ep39_The-Tavern.html",
                                 site->page_img_attr,
                                 site->page_img_url_contains,
                                 &s_urls) == k_ra8_ok);
  TEST_ASSERT_EQ((uint16_t)3, s_urls.count);
  TEST_ASSERT(strstr(s_urls.urls[0], "/0_sources/ep39_The-Tavern/low-res/") != nullptr);
  TEST_ASSERT(strstr(s_urls.urls[2], "E39P02.jpg") != nullptr);
  char metadata[k_mdl_url_max];
  TEST_ASSERT(
    mdl_extract_selector(s_html, len, site->chapter_title_selector, metadata, sizeof(metadata)) ==
    k_ra8_ok);
  TEST_ASSERT(strcmp(metadata, "Episode 39: The Tavern") == 0);
}

/** @test The Pepper&Carrot descriptor matches its official archive and episode
 * @brief Exercise the peppercarrot descriptor regression scenario.
 * @details Executes the peppercarrot descriptor scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 * HTML. */
RA8_INTERNAL static void internal_test_peppercarrot_descriptor(void)
{
  TEST_BEGIN("shipped descriptor: peppercarrot");
  char config_path[1024];
  TEST_ASSERT(__builtin_snprintf(config_path,
                                 sizeof(config_path),
                                 "%s/peppercarrot.conf",
                                 MDL_SITE_CONFIG_DIR) > 0);
  mdl_site_t site;
  TEST_ASSERT(mdl_config_load(mdl_test_storage_get(), config_path, &site) == k_ra8_ok);
  TEST_ASSERT(strcmp(site.host, "www.peppercarrot.com") == 0);
  TEST_ASSERT(site.search_url[0] == '\0');
  TEST_ASSERT(site.browse_url[0] != '\0');
  TEST_ASSERT(strcmp(site.chapter_url_prefix, "https://www.peppercarrot.com/en/webcomic/ep") == 0);
  TEST_ASSERT_EQ((uint32_t)k_peppercarrot_rate_pm, site.rate_per_min);
  const mdl_gov_cfg_t gov_cfg = mdl_config_gov_cfg(&site);
  TEST_ASSERT_EQ((uint32_t)k_peppercarrot_rate_pm, gov_cfg.rate_per_min);
  internal_test_peppercarrot_discovery(&site);
  internal_test_peppercarrot_series(&site);
  internal_test_peppercarrot_chapter(&site);
  TEST_END("shipped descriptor: peppercarrot");
}

int main(void)
{
  TEST_ASSERT_EQ(k_ra8_ok, mdl_test_storage_init());
  TEST_BEGIN("all shipped descriptors have qualification vectors");
  TEST_ASSERT_EQ((uint16_t)2, (uint16_t)MDL_SHIPPED_SITE_COUNT);
  TEST_END("all shipped descriptors have qualification vectors");
  internal_test_manhwaus_descriptor();
  internal_test_peppercarrot_descriptor();
  TEST_ASSERT_EQ(k_ra8_ok, mdl_test_storage_deinit());
  (void)internal_test_output_fd_text(STDERR_FILENO, "[OK  ] test_mdl_sites.c\n");
  return 0;
}
