/**
 * @file test_mdl_net_provider.c
 * @brief Focused coverage for the injected transport-factory dispatcher.
 * @details ::mdl_net_provider_open is the seam that decides WHICH backend an
 *          application mode opens for a run, and it is the reason those modes
 *          no longer name libcurl. It is qualified here, in the core, against a
 *          scripted factory and with no backend at all: the dispatcher must
 *          forward the policy unchanged, and every rejected call must leave the
 *          caller a handle ::mdl_net_destroy accepts rather than a half-built
 *          one.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "mdl_net.h"
#include "ra8_attributes.h"
#include "unity_minimal.h"

/** @brief Exact capacities for the deterministic provider vectors. */
typedef enum : uint32_t {
  k_provider_max_bytes = 4096U, /**< Response cap the fixture policy carries. */
} provider_limit_t;

/**
 * @struct provider_script_t
 * @brief What the dispatcher forwarded to the scripted factory.
 * @details Records the entry count and the policy pointer so a vector can
 *          prove the dispatcher forwarded rather than reinterpreted its
 *          arguments, and prove a rejected call never reached the factory.
 * @invariant `opens` counts only entries into ::internal_script_open.
 * @see internal_script_open()
 * @since 0.1.0
 */
typedef struct {
  const mdl_net_policy_t* seen;   /**< Policy object the dispatcher passed. */
  ra8_err_t               result; /**< Result the scripted factory returns. */
  uint32_t                opens;  /**< Times the factory was entered.       */
} provider_script_t;

/** @brief Scripted destroy: the handle is stack-owned, so nothing to release.
 * @details Implements this test-only seam with caller-owned fixtures and no allocation.
 * @param[in,out] ctx Opaque caller-owned fixture context.
 * @return Nothing.
 * @pre Pointer arguments satisfy their documented readable and writable extents.
 * @pre The caller retains ownership of every supplied fixture object.
 * @post Documented outputs reflect the processed fixture on success.
 * @post Failure preserves caller-owned resources as documented.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_script_destroy(void* ctx)
{
  (void)ctx;
}

/** @brief The scripted backend's method table; only `destroy` is reachable.
 * @details The provider dispatcher publishes a handle and never issues a
 *          request, so the two request methods are never entered by this file.
 * @since 0.1.0
 */
static const mdl_net_vtable_t s_script_vtable = {
  .get_buf  = nullptr,
  .get_body = nullptr,
  .destroy  = internal_script_destroy,
};

/** @brief Scripted factory: record the call and publish the fixture handle.
 * @details Implements this test-only seam with caller-owned fixtures and no allocation.
 * @param[in,out] ctx Opaque caller-owned ::provider_script_t state.
 * @param[in] policy Policy the dispatcher forwarded unchanged.
 * @param[out] out_net Interface populated when the script succeeds.
 * @return The result the script was told to return.
 * @retval k_ra8_ok @p out_net names the scripted backend.
 * @retval other The scripted construction failure.
 * @pre Pointer arguments satisfy their documented readable and writable extents.
 * @pre The caller retains ownership of every supplied fixture object.
 * @post Documented outputs reflect the processed fixture on success.
 * @post Failure preserves caller-owned resources as documented.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_script_open(void* ctx, const mdl_net_policy_t* policy, mdl_net_iface_t* out_net)
{
  provider_script_t* script = (provider_script_t*)ctx;
  script->opens += 1U;
  script->seen = policy;
  if (script->result == k_ra8_ok) {
    *out_net = (mdl_net_iface_t){.vtable = &s_script_vtable, .ctx = script};
  }
  return script->result;
}

/**
 * @test net provider guard
 *
 * @par MC/DC:
 * Decision: `(provider == nullptr) || (provider->open == nullptr) ||
 * (policy == nullptr)` in ::mdl_net_provider_open (3 conditions, OR;
 * N+1 = 4 vectors). The `out_net == nullptr` guard that precedes it is a
 * separate one-condition decision with its own vector.
 * - V1: provider set, open set, policy set -> false (control: factory entered)
 * - V2: provider=nullptr, rest ok          -> true  (varies provider)
 * - V3: provider->open=nullptr, rest ok    -> true  (varies open)
 * - V4: policy=nullptr, rest ok            -> true  (varies policy)
 * V1 pairs with each of V2..V4 to show that condition independently drives the
 * outcome; that is the minimal MC/DC set for a 3-condition OR.
 * @brief Exercise the injected transport-factory dispatcher.
 * @details Proves the dispatcher forwards the caller's policy object unchanged, refuses every unusable factory before entering it, and leaves a zeroed handle on every rejected path.
 * @return Nothing.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded fixtures are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_provider_guard(void)
{
  TEST_BEGIN("net provider guard");
  provider_script_t        script  = {.seen = nullptr, .result = k_ra8_ok, .opens = 0U};
  const mdl_net_provider_t good    = {.ctx = &script, .open = internal_script_open};
  const mdl_net_provider_t no_open = {.ctx = &script, .open = nullptr};
  const mdl_net_policy_t   policy  = {.max_response_bytes = (uint64_t)k_provider_max_bytes};
  mdl_net_iface_t          net     = {};

  /* V1 control: every condition false -> the injected factory is entered, and
   * the policy reaches it as the very object the caller supplied. */
  TEST_ASSERT_EQ(k_ra8_ok, mdl_net_provider_open(&good, &policy, &net));
  TEST_ASSERT_EQ((uint32_t)1U, script.opens);
  TEST_ASSERT(script.seen == &policy);
  TEST_ASSERT(net.vtable == &s_script_vtable);
  mdl_net_destroy(&net);

  /* V2: no provider at all. */
  net = (mdl_net_iface_t){.vtable = &s_script_vtable, .ctx = &script};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, mdl_net_provider_open(nullptr, &policy, &net));
  TEST_ASSERT(net.vtable == nullptr); /* a rejected open leaves a clean handle */
  /* V3: a provider carrying no factory method. */
  net = (mdl_net_iface_t){.vtable = &s_script_vtable, .ctx = &script};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, mdl_net_provider_open(&no_open, &policy, &net));
  TEST_ASSERT(net.vtable == nullptr);
  /* V4: no policy to harden the backend with. */
  net = (mdl_net_iface_t){.vtable = &s_script_vtable, .ctx = &script};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, mdl_net_provider_open(&good, nullptr, &net));
  TEST_ASSERT(net.vtable == nullptr);

  /* The preceding out-handle guard: nowhere to publish, so nothing is done. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, mdl_net_provider_open(&good, &policy, nullptr));
  TEST_ASSERT_EQ((uint32_t)1U, script.opens);

  /* A factory that fails is propagated verbatim, and still leaves the caller a
   * handle it can hand to mdl_net_destroy. */
  script.result = k_ra8_fail;
  net           = (mdl_net_iface_t){.vtable = &s_script_vtable, .ctx = &script};
  TEST_ASSERT_EQ(k_ra8_fail, mdl_net_provider_open(&good, &policy, &net));
  TEST_ASSERT(net.vtable == nullptr);
  TEST_ASSERT_EQ((uint32_t)2U, script.opens);
  mdl_net_destroy(&net);
  TEST_END("net provider guard");
}

int main(void)
{
  internal_test_provider_guard();
  return 0;
}
