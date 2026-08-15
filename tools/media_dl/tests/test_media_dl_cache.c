/**
 * @file test_media_dl_cache.c
 * @brief Persistent per-host document-cache contract tests.
 * @details Exercises exact URL identity, conditional revalidation, immutable
 *          body publication, corrupt-index recovery, host partitioning, and
 *          refetch policy through scripted network and portable storage seams.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mdl_cache.h"
#include "mdl_cache_internal.h"
#include "mdl_sanitize.h"
#include "mdl_test_storage.h"
#include "ra8_attributes.h"
#include "unity_minimal.h"

/** @brief Fixed capacities for the cache contract fixture. */
typedef enum : uint16_t {
  k_test_body_capacity = 4096U, /**< Returned document scratch bytes. */
  k_test_path_capacity = 128U,  /**< Temporary-root template bytes.  */
  k_test_step_max      = 4U,    /**< Scripted responses per scenario. */
} mdl_cache_test_limit_t;

/**
 * @struct cache_step_t
 * @brief One scripted network response.
 * @since 0.1.0
 */
typedef struct {
  long        status;        /**< HTTP response status.             */
  const char* body;          /**< Body returned for a 2xx response. */
  const char* etag;          /**< Response ETag, or NULL.           */
  const char* last_modified; /**< Last-Modified, or NULL.           */
  ra8_err_t   error;         /**< Callback status.                  */
} cache_step_t;

/**
 * @struct cache_script_t
 * @brief Bounded network script plus captured conditional requests.
 * @since 0.1.0
 */
typedef struct {
  cache_step_t steps[k_test_step_max];                           /**< Responses. */
  char         if_none_match[k_test_step_max][k_mdl_etag_max];   /**< ETags sent. */
  char         if_modified[k_test_step_max][k_mdl_last_mod_max]; /**< Dates sent. */
  size_t       step_count;                                       /**< Script size. */
  size_t       calls;                                            /**< Calls made. */
} cache_script_t;

/** @brief Large bounded index workspace kept off the test stack. */
static mdl_cache_index_t s_cache_index;
/** @brief Returned document workspace kept off the test stack. */
static char s_body[k_test_body_capacity];

/**
 * @brief Copy one complete fixture string.
 * @details Treats a NULL fixture source as an empty string and rejects rather
 *          than truncating any captured validator.
 * @param[out] destination Writable destination.
 * @param[in] capacity Destination extent.
 * @param[in] source Optional source; NULL means empty.
 * @return Whether the complete string fit.
 * @retval true The complete source and NUL were copied.
 * @retval false The destination was cleared because capacity was insufficient.
 * @pre @p destination is non-NULL and @p capacity is nonzero.
 * @pre A non-NULL @p source is NUL-terminated.
 * @post Success leaves a complete NUL-terminated copy.
 * @post Failure clears the destination.
 * @note Test-only and allocation-free.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_test_copy(char* destination, size_t capacity, const char* source)
{
  const char*  text  = (source != nullptr) ? source : "";
  const size_t bytes = strlen(text);
  if (bytes >= capacity) {
    destination[0] = '\0';
    return false;
  }
  memcpy(destination, text, bytes + 1U);
  return true;
}

/**
 * @brief Serve one scripted response and capture its conditional headers.
 * @details Consumes responses in order, records both request validators, and
 *          copies a nonempty body only for successful 2xx statuses.
 * @param[in,out] context Script fixture.
 * @param[in] url Requested URL.
 * @param[in] request Request metadata.
 * @param[out] buffer Body destination.
 * @param[in] capacity Body capacity.
 * @param[out] out_length Returned body extent.
 * @param[out] response Response metadata.
 * @return Scripted status or a fixture contract error.
 * @retval k_ra8_ok The scripted response was produced completely.
 * @retval k_ra8_err_invalid_arg The script or callback contract is invalid.
 * @retval k_ra8_err_invalid_size A capture or body destination is too small.
 * @retval other The current script step selected an injected error.
 * @pre Every pointer is non-NULL and @p capacity is nonzero.
 * @pre The script contains an unused response.
 * @post Success consumes exactly one script step.
 * @post Captured validators reflect the request exactly.
 * @note The URL is intentionally irrelevant to response selection.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_script_fetch(void*                context,
                                                    const char*          url,
                                                    const mdl_net_req_t* request,
                                                    char*                buffer,
                                                    size_t               capacity,
                                                    size_t*              out_length,
                                                    mdl_net_resp_t*      response)
{
  (void)url;
  cache_script_t* script = (cache_script_t*)context;
  if ((script == nullptr) || (request == nullptr) || (buffer == nullptr) || (capacity == 0U) ||
      (out_length == nullptr) || (response == nullptr) || (script->calls >= script->step_count)) {
    return k_ra8_err_invalid_arg;
  }
  const size_t        call = script->calls;
  const cache_step_t* step = &script->steps[call];
  script->calls += 1U;
  if (!internal_test_copy(script->if_none_match[call],
                          sizeof(script->if_none_match[call]),
                          request->if_none_match) ||
      !internal_test_copy(script->if_modified[call],
                          sizeof(script->if_modified[call]),
                          request->if_modified_since)) {
    return k_ra8_err_invalid_size;
  }
  *response   = (mdl_net_resp_t){.status = step->status};
  *out_length = 0U;
  if (!internal_test_copy(response->etag, sizeof(response->etag), step->etag) ||
      !internal_test_copy(response->last_modified,
                          sizeof(response->last_modified),
                          step->last_modified)) {
    return k_ra8_err_invalid_size;
  }
  if (step->error != k_ra8_ok) {
    return step->error;
  }
  if ((step->status >= 200L) && (step->status <= 299L)) {
    const char*  body  = (step->body != nullptr) ? step->body : "";
    const size_t bytes = strlen(body);
    if ((bytes == 0U) || (bytes > capacity)) {
      return k_ra8_err_invalid_size;
    }
    memcpy(buffer, body, bytes);
    *out_length = bytes;
  }
  return k_ra8_ok;
}

/**
 * @brief Create one isolated cache root and binding.
 * @details Creates a unique real host directory, clears the bounded global
 *          workspaces, and binds them to the process-local portable storage.
 * @param[out] root Temporary root path.
 * @param[in] root_capacity Root buffer extent.
 * @param[out] cache Cache binding.
 * @pre @p root spans @p root_capacity bytes.
 * @pre Test storage was initialized.
 * @post A unique real directory and empty cache binding exist.
 * @post Global index/body workspaces are reset.
 * @note The caller removes the directory after its scenario.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_begin_case(char* root, size_t root_capacity, mdl_cache_t* cache)
{
  static const char k_template_path[] = "/tmp/mdl_cache_test_XXXXXX";
  TEST_ASSERT(sizeof(k_template_path) <= root_capacity);
  memcpy(root, k_template_path, sizeof(k_template_path));
  TEST_ASSERT_NOT_NULL(mkdtemp(root));
  memset(&s_cache_index, 0, sizeof(s_cache_index));
  memset(s_body, 0, sizeof(s_body));
  *cache = (mdl_cache_t){.storage = mdl_test_storage_get(), .index = &s_cache_index, .root = root};
}

/**
 * @brief Execute one cache lookup with a fixed request template.
 * @details Resets the shared body workspace and delegates through the scripted
 *          callback with a fixed bounded timeout.
 * @param[in,out] cache Cache binding.
 * @param[in] url Request URL.
 * @param[in,out] script Scripted network dependency.
 * @param[out] response Response metadata.
 * @param[out] result Cache outcome.
 * @param[out] out_length Returned body extent.
 * @return Canonical cache status.
 * @retval k_ra8_ok A complete body is available in the shared workspace.
 * @retval other Cache, storage, protocol, or scripted fetching failed.
 * @pre Every pointer is non-NULL.
 * @pre The global body workspace is exclusively owned.
 * @post Success returns the complete document in ::s_body.
 * @post Failure publishes no partial document length.
 * @note Each test process runs scenarios serially.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_get(mdl_cache_t*        cache,
                                           const char*         url,
                                           cache_script_t*     script,
                                           mdl_net_resp_t*     response,
                                           mdl_cache_result_t* result,
                                           size_t*             out_length)
{
  const mdl_net_req_t request = {.timeout_ms = 1000U};
  memset(s_body, 0, sizeof(s_body));
  return mdl_cache_get_buf(cache,
                           url,
                           &request,
                           internal_script_fetch,
                           script,
                           s_body,
                           sizeof(s_body),
                           out_length,
                           response,
                           result);
}

/**
 * @brief Require exact returned bytes.
 * @details Compares both the expected length and every returned byte so a
 *          prefix or stale buffer tail cannot satisfy the assertion.
 * @param[in] expected Expected NUL-terminated fixture bytes.
 * @param[in] actual Actual byte extent.
 * @pre ::s_body holds at least @p actual initialized bytes.
 * @pre @p expected is non-NULL.
 * @post Normal return proves exact length and byte equality.
 * @post No fixture state is modified.
 * @note Assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_expect_body(const char* expected, size_t actual)
{
  const size_t expected_length = strlen(expected);
  TEST_ASSERT_EQ((int64_t)expected_length, (int64_t)actual);
  TEST_ASSERT(memcmp(s_body, expected, expected_length) == 0);
}

/**
 * @brief Overwrite one fixture file with exact bytes.
 * @details Opens the existing regular fixture without following symlinks and
 *          loops over interrupted or positive-short writes until complete.
 * @param[in] path File path.
 * @param[in] bytes Source bytes.
 * @param[in] length Source extent.
 * @pre Inputs are valid and the file is a regular fixture.
 * @pre @p bytes spans @p length bytes.
 * @post Normal return means the file contains exactly the supplied bytes.
 * @post The descriptor is closed.
 * @note Test-only corruption seam.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_overwrite(const char* path, const uint8_t* bytes, size_t length)
{
  const int descriptor = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC | O_NOFOLLOW);
  TEST_ASSERT(descriptor >= 0);
  size_t offset = 0U;
  while (offset < length) {
    const ssize_t wrote = write(descriptor, bytes + offset, length - offset);
    if (wrote > 0) {
      offset += (size_t)wrote;
    } else if ((wrote < 0) && (errno == EINTR)) {
      continue;
    } else {
      break;
    }
  }
  TEST_ASSERT_EQ((int64_t)length, (int64_t)offset);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)close(descriptor));
}

/**
 * @brief Build and inspect one cached body path.
 * @details Joins the trusted host directory and body leaf, then snapshots the
 *          regular file metadata for later no-write comparison.
 * @param[in] paths Host cache paths.
 * @param[in] leaf Body leaf.
 * @param[out] path Complete body path.
 * @param[in] capacity Path buffer extent.
 * @param[out] metadata Host stat result.
 * @pre Every pointer is non-NULL.
 * @pre The body exists as a regular file.
 * @post Path and metadata identify the current body object.
 * @post No file content is modified.
 * @note Host stat is used only to prove a 304 did not replace the body.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_stat_body(const mdl_cache_paths_t* paths,
                                            const char*              leaf,
                                            char*                    path,
                                            size_t                   capacity,
                                            struct stat*             metadata)
{
  TEST_ASSERT(mdl_path_join(paths->directory, leaf, path, capacity));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)stat(path, metadata));
  TEST_ASSERT(S_ISREG(metadata->st_mode));
}

/**
 * @brief Prove two metadata snapshots describe an untouched body object.
 * @details Compares device, inode, extent, mtime, and ctime while deliberately
 *          excluding read-sensitive access time.
 * @param[in] before Snapshot before the cache lookup.
 * @param[in] after Snapshot after the cache lookup.
 * @pre Both pointers are non-NULL.
 * @pre Both snapshots came from the same path.
 * @post Normal return proves identity, size, mtime, and ctime are unchanged.
 * @post No filesystem state is modified.
 * @note Access time is intentionally ignored because verified reads may update it.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_expect_untouched(const struct stat* before,
                                                   const struct stat* after)
{
  TEST_ASSERT_EQ((int64_t)before->st_dev, (int64_t)after->st_dev);
  TEST_ASSERT_EQ((int64_t)before->st_ino, (int64_t)after->st_ino);
  TEST_ASSERT_EQ((int64_t)before->st_size, (int64_t)after->st_size);
#if defined(__APPLE__)
  TEST_ASSERT_EQ((int64_t)before->st_mtimespec.tv_sec, (int64_t)after->st_mtimespec.tv_sec);
  TEST_ASSERT_EQ((int64_t)before->st_mtimespec.tv_nsec, (int64_t)after->st_mtimespec.tv_nsec);
  TEST_ASSERT_EQ((int64_t)before->st_ctimespec.tv_sec, (int64_t)after->st_ctimespec.tv_sec);
  TEST_ASSERT_EQ((int64_t)before->st_ctimespec.tv_nsec, (int64_t)after->st_ctimespec.tv_nsec);
#else
  TEST_ASSERT_EQ((int64_t)before->st_mtim.tv_sec, (int64_t)after->st_mtim.tv_sec);
  TEST_ASSERT_EQ((int64_t)before->st_mtim.tv_nsec, (int64_t)after->st_mtim.tv_nsec);
  TEST_ASSERT_EQ((int64_t)before->st_ctim.tv_sec, (int64_t)after->st_ctim.tv_sec);
  TEST_ASSERT_EQ((int64_t)before->st_ctim.tv_nsec, (int64_t)after->st_ctim.tv_nsec);
#endif
}

/**
 * @brief Remove known cache files and their host directory.
 * @details Removes every explicitly retained body leaf, the index, and then
 *          the now-empty host directory without scanning the namespace.
 * @param[in] paths Host cache paths.
 * @param[in] leaves Body leaves to remove.
 * @param[in] leaf_count Number of leaf pointers.
 * @pre @p paths is non-NULL and leaves are safe basenames.
 * @pre The test exclusively owns this cache directory.
 * @post Known bodies, index, and host directory are absent.
 * @post Missing duplicate leaves are tolerated.
 * @note The caller removes the outer cache root separately.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_remove_host(const mdl_cache_paths_t* paths, const char* const* leaves, size_t leaf_count)
{
  for (size_t i = 0U; i < leaf_count; ++i) {
    char body_path[k_fw_fs_path_cap];
    TEST_ASSERT(mdl_path_join(paths->directory, leaves[i], body_path, sizeof(body_path)));
    if ((unlink(body_path) != 0) && (errno != ENOENT)) {
      TEST_ASSERT(false);
    }
  }
  if ((unlink(paths->index_path) != 0) && (errno != ENOENT)) {
    TEST_ASSERT(false);
  }
  TEST_ASSERT_EQ((int64_t)0, (int64_t)rmdir(paths->directory));
}

/**
 * @brief Load current host paths and require a valid index.
 * @details Reuses the production loader after successful publication and
 *          asserts that no corruption-recovery branch was needed.
 * @param[in,out] cache Cache binding.
 * @param[in] url Host-selecting URL.
 * @param[out] paths Derived host paths.
 * @pre Every pointer is non-NULL.
 * @pre Cache storage is initialized.
 * @post Paths and index workspace describe @p url's host.
 * @post No corrupt-index recovery was needed.
 * @note Used only after a successful publication.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_load_valid(mdl_cache_t* cache, const char* url, mdl_cache_paths_t* paths)
{
  bool rebuilt = false;
  TEST_ASSERT_EQ((int64_t)k_ra8_ok, (int64_t)priv_mdl_cache_load(cache, url, paths, &rebuilt));
  TEST_ASSERT(!rebuilt);
}

/**
 * @test A 304 reuses the verified entity and never replaces its body object.
 * @brief Verify conditional body-free reuse of one cached entity.
 * @details Publishes a validator-bearing entity, receives 304, and proves the
 *          returned bytes and underlying body metadata remain unchanged.
 * @pre Portable test storage is initialized.
 * @pre The scripted server has one 200 followed by one 304.
 * @post Validators are sent and body identity/timestamps remain unchanged.
 * @post The index records the 304 observation.
 * @note This pins issue #303's body-free 304 contract.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_conditional_304(void)
{
  TEST_BEGIN("host cache conditional 304");
  char        root[k_test_path_capacity];
  mdl_cache_t cache;
  internal_begin_case(root, sizeof(root), &cache);
  cache_script_t script = {
    .steps      = {{200L, "alpha", "\"v1\"", "Wed, 21 Oct 2015 07:28:00 GMT", k_ra8_ok},
                   {304L, nullptr, nullptr, nullptr, k_ra8_ok}},
    .step_count = 2U};
  mdl_net_resp_t     response;
  mdl_cache_result_t result;
  size_t             length = 0U;
  const char*        url    = "https://example.test/series/one";
  TEST_ASSERT_EQ(k_ra8_ok, internal_get(&cache, url, &script, &response, &result, &length));
  internal_expect_body("alpha", length);
  mdl_cache_paths_t paths;
  internal_load_valid(&cache, url, &paths);
  TEST_ASSERT_EQ((int64_t)1, (int64_t)s_cache_index.record_count);
  char leaf[k_mdl_relpath_max];
  TEST_ASSERT(internal_test_copy(leaf, sizeof(leaf), s_cache_index.records[0].relative_path));
  char        body_path[k_fw_fs_path_cap];
  struct stat before;
  internal_stat_body(&paths, leaf, body_path, sizeof(body_path), &before);

  TEST_ASSERT_EQ(k_ra8_ok, internal_get(&cache, url, &script, &response, &result, &length));
  internal_expect_body("alpha", length);
  TEST_ASSERT(result.body_reused);
  TEST_ASSERT(result.revalidated);
  TEST_ASSERT_EQ((int64_t)304, (int64_t)result.observed_status);
  TEST_ASSERT(strcmp(script.if_none_match[1], "\"v1\"") == 0);
  TEST_ASSERT(strcmp(script.if_modified[1], "Wed, 21 Oct 2015 07:28:00 GMT") == 0);
  struct stat after;
  TEST_ASSERT_EQ((int64_t)0, (int64_t)stat(body_path, &after));
  internal_expect_untouched(&before, &after);
  TEST_ASSERT_EQ((int64_t)304, (int64_t)s_cache_index.records[0].response_status);

  const char* leaves[] = {leaf};
  internal_remove_host(&paths, leaves, 1U);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)rmdir(root));
  TEST_END("host cache conditional 304");
}

/**
 * @test A changed validator publishes changed bytes and a new immutable body.
 * @brief Verify changed-validator publication under a new body identity.
 * @details Serves two entity generations and checks the conditional request,
 *          updated ETag, returned bytes, and distinct immutable leaves.
 * @pre Portable test storage is initialized.
 * @pre The scripted server has two successful generations.
 * @post The second request sends the first ETag and records the second ETag.
 * @post Both body generations can be cleaned by their distinct leaves.
 * @note The old immutable body is harmless until cache cleanup.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_changed_etag(void)
{
  TEST_BEGIN("host cache changed ETag");
  char        root[k_test_path_capacity];
  mdl_cache_t cache;
  internal_begin_case(root, sizeof(root), &cache);
  cache_script_t     script = {.steps      = {{200L, "alpha", "\"v1\"", nullptr, k_ra8_ok},
                                              {200L, "beta", "\"v2\"", nullptr, k_ra8_ok}},
                               .step_count = 2U};
  mdl_net_resp_t     response;
  mdl_cache_result_t result;
  size_t             length = 0U;
  const char*        url    = "https://example.test/chapter/one";
  TEST_ASSERT_EQ(k_ra8_ok, internal_get(&cache, url, &script, &response, &result, &length));
  mdl_cache_paths_t paths;
  internal_load_valid(&cache, url, &paths);
  char old_leaf[k_mdl_relpath_max];
  TEST_ASSERT(
    internal_test_copy(old_leaf, sizeof(old_leaf), s_cache_index.records[0].relative_path));

  TEST_ASSERT_EQ(k_ra8_ok, internal_get(&cache, url, &script, &response, &result, &length));
  internal_expect_body("beta", length);
  TEST_ASSERT(strcmp(script.if_none_match[1], "\"v1\"") == 0);
  TEST_ASSERT(strcmp(s_cache_index.records[0].etag, "\"v2\"") == 0);
  TEST_ASSERT(strcmp(old_leaf, s_cache_index.records[0].relative_path) != 0);
  char new_leaf[k_mdl_relpath_max];
  TEST_ASSERT(
    internal_test_copy(new_leaf, sizeof(new_leaf), s_cache_index.records[0].relative_path));

  const char* leaves[] = {old_leaf, new_leaf};
  internal_remove_host(&paths, leaves, 2U);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)rmdir(root));
  TEST_END("host cache changed ETag");
}

/**
 * @test A truncated index is discarded and rebuilt without stale validators.
 * @brief Verify authenticated-index recovery from a truncated generation.
 * @details Truncates an otherwise valid index, then proves the next lookup
 *          discards it, fetches unconditionally, and publishes fresh metadata.
 * @pre One valid generation exists before deliberate index corruption.
 * @pre The scripted server has a second unconditional 200 response.
 * @post The lookup reports index rebuilding and records only the fresh entity.
 * @post No stale validator is sent after authentication fails.
 * @note Corrupt cache metadata is disposable, unlike library state.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_corrupt_index_rebuild(void)
{
  TEST_BEGIN("host cache corrupt index rebuild");
  char        root[k_test_path_capacity];
  mdl_cache_t cache;
  internal_begin_case(root, sizeof(root), &cache);
  cache_script_t     script = {.steps      = {{200L, "alpha", "\"old\"", nullptr, k_ra8_ok},
                                              {200L, "rebuilt", "\"new\"", nullptr, k_ra8_ok}},
                               .step_count = 2U};
  mdl_net_resp_t     response;
  mdl_cache_result_t result;
  size_t             length = 0U;
  const char*        url    = "https://corrupt.test/index";
  TEST_ASSERT_EQ(k_ra8_ok, internal_get(&cache, url, &script, &response, &result, &length));
  mdl_cache_paths_t paths;
  internal_load_valid(&cache, url, &paths);
  char old_leaf[k_mdl_relpath_max];
  TEST_ASSERT(
    internal_test_copy(old_leaf, sizeof(old_leaf), s_cache_index.records[0].relative_path));
  static const uint8_t k_broken[] = {'b', 'a', 'd'};
  internal_overwrite(paths.index_path, k_broken, sizeof(k_broken));

  TEST_ASSERT_EQ(k_ra8_ok, internal_get(&cache, url, &script, &response, &result, &length));
  internal_expect_body("rebuilt", length);
  TEST_ASSERT(result.index_rebuilt);
  TEST_ASSERT(strcmp(script.if_none_match[1], "") == 0);
  TEST_ASSERT_EQ((int64_t)1, (int64_t)s_cache_index.record_count);
  TEST_ASSERT(strcmp(s_cache_index.records[0].etag, "\"new\"") == 0);
  char new_leaf[k_mdl_relpath_max];
  TEST_ASSERT(
    internal_test_copy(new_leaf, sizeof(new_leaf), s_cache_index.records[0].relative_path));

  const char* leaves[] = {old_leaf, new_leaf};
  internal_remove_host(&paths, leaves, 2U);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)rmdir(root));
  TEST_END("host cache corrupt index rebuild");
}

/**
 * @test Validator-free bodies skip network unless refetch is requested.
 * @brief Verify ordinary reuse and forced refetch for validator-free bodies.
 * @details Counts callbacks across initial publication, direct verified reuse,
 *          and a forced network refresh with changed bytes.
 * @pre The first 200 response carries no validators.
 * @pre The second scripted response has different bytes.
 * @post Ordinary reuse performs no callback; refetch performs exactly one.
 * @post Refetch publishes and returns the changed bytes.
 * @note Refetch forces revalidation, not validator omission when validators exist.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_refetch_policy(void)
{
  TEST_BEGIN("host cache refetch policy");
  char        root[k_test_path_capacity];
  mdl_cache_t cache;
  internal_begin_case(root, sizeof(root), &cache);
  cache_script_t     script = {.steps      = {{200L, "alpha", nullptr, nullptr, k_ra8_ok},
                                              {200L, "forced", nullptr, nullptr, k_ra8_ok}},
                               .step_count = 2U};
  mdl_net_resp_t     response;
  mdl_cache_result_t result;
  size_t             length = 0U;
  const char*        url    = "https://plain.test/document";
  TEST_ASSERT_EQ(k_ra8_ok, internal_get(&cache, url, &script, &response, &result, &length));
  mdl_cache_paths_t paths;
  internal_load_valid(&cache, url, &paths);
  char old_leaf[k_mdl_relpath_max];
  TEST_ASSERT(
    internal_test_copy(old_leaf, sizeof(old_leaf), s_cache_index.records[0].relative_path));

  TEST_ASSERT_EQ(k_ra8_ok, internal_get(&cache, url, &script, &response, &result, &length));
  internal_expect_body("alpha", length);
  TEST_ASSERT_EQ((int64_t)1, (int64_t)script.calls);
  TEST_ASSERT(result.body_reused);
  TEST_ASSERT(!result.revalidated);

  cache.refetch = true;
  TEST_ASSERT_EQ(k_ra8_ok, internal_get(&cache, url, &script, &response, &result, &length));
  internal_expect_body("forced", length);
  TEST_ASSERT_EQ((int64_t)2, (int64_t)script.calls);
  TEST_ASSERT(result.revalidated);
  char new_leaf[k_mdl_relpath_max];
  TEST_ASSERT(
    internal_test_copy(new_leaf, sizeof(new_leaf), s_cache_index.records[0].relative_path));

  const char* leaves[] = {old_leaf, new_leaf};
  internal_remove_host(&paths, leaves, 2U);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)rmdir(root));
  TEST_END("host cache refetch policy");
}

/**
 * @test Host indexes are isolated even when one binding serves both hosts.
 * @brief Verify one persistent index namespace per exact request host.
 * @details Publishes distinct documents for two hosts, confirms their derived
 *          directories differ, then reuses the first without another fetch.
 * @pre Two distinct host URLs have one scripted 200 response each.
 * @pre Neither response carries validators.
 * @post Each host has a distinct directory and retained entity.
 * @post Returning to host A reuses A without a third network call.
 * @note URL hashes remain secondary to exact URL comparison inside each host.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_host_partition(void)
{
  TEST_BEGIN("host cache partition");
  char        root[k_test_path_capacity];
  mdl_cache_t cache;
  internal_begin_case(root, sizeof(root), &cache);
  cache_script_t     script = {.steps      = {{200L, "alpha", nullptr, nullptr, k_ra8_ok},
                                              {200L, "beta", nullptr, nullptr, k_ra8_ok}},
                               .step_count = 2U};
  mdl_net_resp_t     response;
  mdl_cache_result_t result;
  size_t             length = 0U;
  const char*        url_a  = "https://alpha.test/index";
  const char*        url_b  = "https://beta.test/index";
  TEST_ASSERT_EQ(k_ra8_ok, internal_get(&cache, url_a, &script, &response, &result, &length));
  mdl_cache_paths_t paths_a;
  internal_load_valid(&cache, url_a, &paths_a);
  char leaf_a[k_mdl_relpath_max];
  TEST_ASSERT(internal_test_copy(leaf_a, sizeof(leaf_a), s_cache_index.records[0].relative_path));

  TEST_ASSERT_EQ(k_ra8_ok, internal_get(&cache, url_b, &script, &response, &result, &length));
  mdl_cache_paths_t paths_b;
  internal_load_valid(&cache, url_b, &paths_b);
  char leaf_b[k_mdl_relpath_max];
  TEST_ASSERT(internal_test_copy(leaf_b, sizeof(leaf_b), s_cache_index.records[0].relative_path));
  TEST_ASSERT(strcmp(paths_a.directory, paths_b.directory) != 0);

  TEST_ASSERT_EQ(k_ra8_ok, internal_get(&cache, url_a, &script, &response, &result, &length));
  internal_expect_body("alpha", length);
  TEST_ASSERT_EQ((int64_t)2, (int64_t)script.calls);
  const char* leaves_a[] = {leaf_a};
  const char* leaves_b[] = {leaf_b};
  internal_remove_host(&paths_a, leaves_a, 1U);
  internal_remove_host(&paths_b, leaves_b, 1U);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)rmdir(root));
  TEST_END("host cache partition");
}

/**
 * @test A 304 without a retained body retries once without validators.
 * @brief Verify bounded unconditional recovery from an unexpected 304.
 * @details Starts empty, scripts a 304 then a 200, and proves exactly two
 *          validator-free calls produce one published entity.
 * @pre The cache is empty and the script returns 304 then 200.
 * @pre Both response steps fit the fixed callback script.
 * @post Exactly two unconditional calls occur and the 200 body is published.
 * @post No validator is synthesized for either request.
 * @note A second 304 is separately rejected by the same production branch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_unexpected_304_retry(void)
{
  TEST_BEGIN("host cache unexpected 304 retry");
  char        root[k_test_path_capacity];
  mdl_cache_t cache;
  internal_begin_case(root, sizeof(root), &cache);
  cache_script_t     script = {.steps      = {{304L, nullptr, nullptr, nullptr, k_ra8_ok},
                                              {200L, "recovered", "\"fresh\"", nullptr, k_ra8_ok}},
                               .step_count = 2U};
  mdl_net_resp_t     response;
  mdl_cache_result_t result;
  size_t             length = 0U;
  const char*        url    = "https://retry.test/index";
  TEST_ASSERT_EQ(k_ra8_ok, internal_get(&cache, url, &script, &response, &result, &length));
  internal_expect_body("recovered", length);
  TEST_ASSERT_EQ((int64_t)2, (int64_t)script.calls);
  TEST_ASSERT(strcmp(script.if_none_match[0], "") == 0);
  TEST_ASSERT(strcmp(script.if_none_match[1], "") == 0);
  mdl_cache_paths_t paths;
  internal_load_valid(&cache, url, &paths);
  char leaf[k_mdl_relpath_max];
  TEST_ASSERT(internal_test_copy(leaf, sizeof(leaf), s_cache_index.records[0].relative_path));

  const char* leaves[] = {leaf};
  internal_remove_host(&paths, leaves, 1U);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)rmdir(root));
  TEST_END("host cache unexpected 304 retry");
}

/**
 * @test A repeated 304 without a body fails without publishing an entity.
 * @brief Verify a second body-free 304 is a terminal protocol error.
 * @details Scripts two consecutive 304 responses against an empty host and
 *          proves the bounded retry leaves the persistent index empty.
 * @pre The cache is empty and both scripted responses are 304.
 * @pre Portable storage is initialized.
 * @post The protocol error is returned after exactly two calls.
 * @post The host index remains empty and no body leaf is published.
 * @note This prevents an unbounded 304 recovery loop.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_repeated_304_rejected(void)
{
  TEST_BEGIN("host cache repeated 304 rejected");
  char        root[k_test_path_capacity];
  mdl_cache_t cache;
  internal_begin_case(root, sizeof(root), &cache);
  cache_script_t     script = {.steps      = {{304L, nullptr, nullptr, nullptr, k_ra8_ok},
                                              {304L, nullptr, nullptr, nullptr, k_ra8_ok}},
                               .step_count = 2U};
  mdl_net_resp_t     response;
  mdl_cache_result_t result;
  size_t             length = 0U;
  const char*        url    = "https://retry.test/repeated";
  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 internal_get(&cache, url, &script, &response, &result, &length));
  TEST_ASSERT_EQ((int64_t)2, (int64_t)script.calls);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)length);
  mdl_cache_paths_t paths;
  internal_load_valid(&cache, url, &paths);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)s_cache_index.record_count);
  internal_remove_host(&paths, nullptr, 0U);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)rmdir(root));
  TEST_END("host cache repeated 304 rejected");
}

/**
 * @brief Run every persistent cache contract scenario.
 * @return Zero when all assertions pass.
 * @pre The host supports POSIX temporary directories.
 * @pre No other test shares the process-local storage binding.
 * @post Every created cache namespace is removed.
 * @post The portable storage adapter is deinitialized.
 * @note Assertion failure terminates the process.
 * @since 0.1.0
 */
int32_t main(void)
{
  TEST_ASSERT_EQ(k_ra8_ok, mdl_test_storage_init());
  internal_test_conditional_304();
  internal_test_changed_etag();
  internal_test_corrupt_index_rebuild();
  internal_test_refetch_policy();
  internal_test_host_partition();
  internal_test_unexpected_304_retry();
  internal_test_repeated_304_rejected();
  TEST_ASSERT_EQ(k_ra8_ok, mdl_test_storage_deinit());
  (void)write(STDERR_FILENO,
              "[OK  ] test_media_dl_cache.c\n",
              sizeof("[OK  ] test_media_dl_cache.c\n") - 1U);
  return 0;
}
