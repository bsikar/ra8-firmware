/**
 * @file test_media_dl_sites.c
 * @brief Qualification fixtures for every shipped media_dl site descriptor.
 * @details Exercises discovery, chapter/image extraction, and descriptor-driven
 * metadata selectors against captured current-site HTML for every descriptor.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mdl_config.h"
#include "mdl_extract.h"
#include "mdl_search.h"
#include "mdl_urlname.h"
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
  k_fixture_cap       = 32768U, /**< Captured HTML scratch capacity.       */
  k_tiny_metadata_cap = 8U,     /**< Deliberately undersized value buffer. */
} site_test_limit_t;
static char           s_html[k_fixture_cap];
static mdl_hit_list_t s_hits;
static mdl_url_list_t s_urls;

static size_t read_fixture(const char* leaf)
{
  char path[1024];
  TEST_ASSERT(snprintf(path, sizeof(path), "%s/%s", MDL_SITE_FIXTURE_DIR, leaf) > 0);
  FILE* fp = fopen(path, "rb");
  TEST_ASSERT_NOT_NULL(fp);
  const size_t got = fread(s_html, 1U, sizeof(s_html) - 1U, fp);
  TEST_ASSERT(fclose(fp) == 0);
  TEST_ASSERT(got > 0U);
  TEST_ASSERT(got < (sizeof(s_html) - 1U));
  s_html[got] = '\0';
  return got;
}

/** @test The ManhwaUS descriptor matches captured discovery and reader HTML. */
static void test_manhwaus_descriptor(void)
{
  TEST_BEGIN("shipped descriptor: manhwaus");
  char config_path[1024];
  TEST_ASSERT(snprintf(config_path, sizeof(config_path), "%s/manhwaus.conf", MDL_SITE_CONFIG_DIR) >
              0);
  mdl_site_t site;
  TEST_ASSERT(mdl_config_load(config_path, &site) == k_ra8_ok);
  TEST_ASSERT(strcmp(site.host, "manhwaus.net") == 0);
  TEST_ASSERT(strstr(site.search_url, "{q}") != nullptr);
  TEST_ASSERT(site.browse_url[0] != '\0');

  size_t len = read_fixture("manhwaus_discovery.html");
  TEST_ASSERT(
    mdl_extract_hits(s_html, len, "https://manhwaus.net/", site.search_result_contains, &s_hits) ==
    k_ra8_ok);
  TEST_ASSERT_EQ((uint16_t)4, s_hits.count);
  TEST_ASSERT_EQ((uint16_t)2, mdl_search_filter_series_hits(&s_hits, site.chapter_url_contains));
  TEST_ASSERT_EQ((uint16_t)2, s_hits.count);
  TEST_ASSERT(strcmp(s_hits.hits[0].url, "https://manhwaus.net/webtoon/player/") == 0);
  TEST_ASSERT(strcmp(s_hits.hits[1].url, "https://manhwaus.net/webtoon/alpha/") == 0);

  len = read_fixture("manhwaus_series.html");
  TEST_ASSERT(mdl_extract_anchors(s_html,
                                  len,
                                  "https://manhwaus.net/webtoon/player/",
                                  site.chapter_url_contains,
                                  &s_urls) == k_ra8_ok);
  TEST_ASSERT_EQ((uint16_t)2, s_urls.count);
  TEST_ASSERT(mdl_urlname_chapter_value(s_urls.urls[0]) == 109.0);
  TEST_ASSERT(mdl_urlname_chapter_value(s_urls.urls[1]) == 108.5);
  char metadata[k_mdl_url_max];
  TEST_ASSERT(
    mdl_extract_selector(s_html, len, site.series_title_selector, metadata, sizeof(metadata)) ==
    k_ra8_ok);
  TEST_ASSERT(strcmp(metadata, "The Player Who Returned 10,000 Years Later") == 0);
  TEST_ASSERT(mdl_extract_selector(s_html, len, "class:post-title", metadata, sizeof(metadata)) ==
              k_ra8_ok);
  TEST_ASSERT(strcmp(metadata, "The Player Who Returned 10,000 Years Later") == 0);
  TEST_ASSERT(
    mdl_extract_selector(s_html, len, site.series_summary_selector, metadata, sizeof(metadata)) ==
    k_ra8_ok);
  TEST_ASSERT(strcmp(metadata, "A player returns & faces a changed world.") == 0);
  TEST_ASSERT(
    mdl_extract_selector(s_html, len, site.series_author_selector, metadata, sizeof(metadata)) ==
    k_ra8_ok);
  TEST_ASSERT(strcmp(metadata, "Butterfly Valley") == 0);
  TEST_ASSERT(
    mdl_extract_selector(s_html, len, site.series_artist_selector, metadata, sizeof(metadata)) ==
    k_ra8_ok);
  TEST_ASSERT(strcmp(metadata, "Maeng Ju-gong") == 0);
  char cover[k_mdl_url_max];
  TEST_ASSERT(
    mdl_extract_selector(s_html, len, site.series_cover_selector, metadata, sizeof(metadata)) ==
    k_ra8_ok);
  TEST_ASSERT(mdl_extract_resolve_url("https://manhwaus.net/webtoon/player/",
                                      metadata,
                                      cover,
                                      sizeof(cover)));
  TEST_ASSERT(strcmp(cover, "https://manhwaus.net/uploads/player/cover.png") == 0);
  char tiny[k_tiny_metadata_cap];
  TEST_ASSERT(mdl_extract_selector(s_html, len, site.series_title_selector, tiny, sizeof(tiny)) ==
              k_ra8_err_invalid_size);
  TEST_ASSERT(mdl_extract_selector(s_html, len, "css:.post-title", metadata, sizeof(metadata)) ==
              k_ra8_err_invalid_arg);

  len = read_fixture("manhwaus_chapter.html");
  TEST_ASSERT(mdl_extract_images(s_html,
                                 len,
                                 "https://manhwaus.net/webtoon/player/chapter-108-5/",
                                 site.page_img_attr,
                                 site.page_img_url_contains,
                                 &s_urls) == k_ra8_ok);
  TEST_ASSERT_EQ((uint16_t)2, s_urls.count);
  TEST_ASSERT(strstr(s_urls.urls[0], "/uploads/") != nullptr);
  TEST_ASSERT(strstr(s_urls.urls[1], "/uploads/") != nullptr);
  TEST_ASSERT(
    mdl_extract_selector(s_html, len, site.chapter_title_selector, metadata, sizeof(metadata)) ==
    k_ra8_ok);
  TEST_ASSERT(strcmp(metadata, "Chapter 108.5 - The Return") == 0);
  TEST_END("shipped descriptor: manhwaus");
}

/** @test The Pepper&Carrot descriptor matches its official archive and episode
 * HTML. */
static void test_peppercarrot_descriptor(void)
{
  TEST_BEGIN("shipped descriptor: peppercarrot");
  char config_path[1024];
  TEST_ASSERT(
    snprintf(config_path, sizeof(config_path), "%s/peppercarrot.conf", MDL_SITE_CONFIG_DIR) > 0);
  mdl_site_t site;
  TEST_ASSERT(mdl_config_load(config_path, &site) == k_ra8_ok);
  TEST_ASSERT(strcmp(site.host, "www.peppercarrot.com") == 0);
  TEST_ASSERT(site.search_url[0] == '\0');
  TEST_ASSERT(site.browse_url[0] != '\0');
  TEST_ASSERT(strcmp(site.chapter_url_prefix, "https://www.peppercarrot.com/en/webcomic/ep") == 0);

  size_t len = read_fixture("peppercarrot_discovery.html");
  TEST_ASSERT(mdl_extract_hits(s_html,
                               len,
                               "https://www.peppercarrot.com/en/webcomics/index.html",
                               site.search_result_contains,
                               &s_hits) == k_ra8_ok);
  TEST_ASSERT_EQ((uint16_t)1, s_hits.count);
  TEST_ASSERT_EQ((uint16_t)0, mdl_search_filter_series_hits(&s_hits, site.chapter_url_contains));
  TEST_ASSERT(
    strcmp(s_hits.hits[0].url, "https://www.peppercarrot.com/en/webcomics/peppercarrot.html") == 0);

  len = read_fixture("peppercarrot_series.html");
  TEST_ASSERT(mdl_extract_anchors(s_html,
                                  len,
                                  "https://www.peppercarrot.com/en/webcomics/peppercarrot.html",
                                  site.chapter_url_contains,
                                  &s_urls) == k_ra8_ok);
  TEST_ASSERT_EQ((uint16_t)2, s_urls.count);
  TEST_ASSERT(mdl_urlname_chapter_number(s_urls.urls[0]) == 39L);
  TEST_ASSERT(mdl_urlname_chapter_number(s_urls.urls[1]) == 38L);
  TEST_ASSERT(strncmp(s_urls.urls[0], site.chapter_url_prefix, strlen(site.chapter_url_prefix)) ==
              0);
  char metadata[k_mdl_url_max];
  TEST_ASSERT(
    mdl_extract_selector(s_html, len, site.series_title_selector, metadata, sizeof(metadata)) ==
    k_ra8_ok);
  TEST_ASSERT(strcmp(metadata, "Pepper & Carrot") == 0);
  TEST_ASSERT(
    mdl_extract_selector(s_html, len, site.series_author_selector, metadata, sizeof(metadata)) ==
    k_ra8_ok);
  TEST_ASSERT(strcmp(metadata, "David Revoy") == 0);

  len = read_fixture("peppercarrot_chapter.html");
  TEST_ASSERT(mdl_extract_images(s_html,
                                 len,
                                 "https://www.peppercarrot.com/en/webcomic/ep39_The-Tavern.html",
                                 site.page_img_attr,
                                 site.page_img_url_contains,
                                 &s_urls) == k_ra8_ok);
  TEST_ASSERT_EQ((uint16_t)3, s_urls.count);
  TEST_ASSERT(strstr(s_urls.urls[0], "/0_sources/ep39_The-Tavern/low-res/") != nullptr);
  TEST_ASSERT(strstr(s_urls.urls[2], "E39P02.jpg") != nullptr);
  TEST_ASSERT(
    mdl_extract_selector(s_html, len, site.chapter_title_selector, metadata, sizeof(metadata)) ==
    k_ra8_ok);
  TEST_ASSERT(strcmp(metadata, "Episode 39: The Tavern") == 0);
  TEST_END("shipped descriptor: peppercarrot");
}

int32_t main(void)
{
  TEST_BEGIN("all shipped descriptors have qualification vectors");
  TEST_ASSERT_EQ((uint16_t)2, (uint16_t)MDL_SHIPPED_SITE_COUNT);
  TEST_END("all shipped descriptors have qualification vectors");
  test_manhwaus_descriptor();
  test_peppercarrot_descriptor();
  (void)fprintf(stderr, "[OK  ] test_media_dl_sites.c\n");
  return 0;
}
