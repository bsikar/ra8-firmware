/**
 * @file test_ra8_tflm_op_subset.cc
 * @brief Unit tests pinning the vendored TFLite-micro operator subset and the
 *        first-party Ethos-U custom-op registration seam (issue #228)
 *
 * @details
 * Issue #228's residual is the runtime half of the RA8P1 Ethos-U55 story: the
 * `MicroInterpreter` model-driven path has never executed, and the CPU-fallback
 * leg -- what happens to a graph node the NPU cannot take -- is unexercised.
 * Both of those ultimately need a real Vela command stream (#227) and RA8P1
 * silicon (#229). What does NOT need either is the question underneath them:
 * which operators this tree can resolve at all, and whether the resolver keys
 * the Ethos-U custom op on the same name the first-party kernel publishes.
 *
 * That boundary IS the CPU-fallback contract. A Vela-lowered model reaches the
 * NPU through one custom operator named by `tflite::ethosu_custom_name()`;
 * every node Vela leaves on the CPU must resolve against the lean kernel subset
 * `cmake/tflite_micro.cmake` compiles. Until this file existed nothing in the
 * tree asserted either half, and the build's own comment was already wrong
 * about the subset: it omitted `MAX_POOL_2D`, which `pooling.cc` registers.
 *
 * These are host tests. They compile the real first-party
 * `libs/ra8_hal/src/ra8_ethosu_kernel.cc` off-target -- the first time that
 * translation unit is built for anything but the RA8P1 -- so the documented
 * no-NPU contract (`Register_ETHOSU()` yields `nullptr`, the custom name is
 * still published) is checked rather than assumed. No NPU register is touched,
 * no inference runs, and nothing here claims anything about silicon.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/micro/kernels/micro_ops.h"
#include "tensorflow/lite/micro/micro_common.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "unity_minimal.h"

namespace tflite {
/** @brief Declared by the first-party kernel; nullptr off-target by contract. */
TFLMRegistration* Register_ETHOSU();
/** @brief The custom-operator name Vela emits and the resolver keys on. */
const char* ethosu_custom_name();
} // namespace tflite

namespace {

/**
 * @brief Resolver capacity and fixture counts.
 *
 * @details `k_resolver_slots` is the template parameter of the resolver under
 *          test: the nine vendored builtins plus the Ethos-U custom op, with
 *          slack so the double-registration test trips the resolver's own
 *          duplicate guard rather than a full table.
 */
enum : unsigned int {
  k_resolver_slots   = 16U, /**< Resolver registration capacity.        */
  k_vendored_builtin = 9U,  /**< Builtins the vendored kernels provide. */
};

/** @brief Set by ::fake_ethosu_invoke so dispatch is observable. */
int g_fake_invoke_calls = 0;

/**
 * @brief Stand-in Ethos-U operator body, used only to identify a registration.
 *
 * @details The real dispatcher drives the NPU through `ra8_ethosu_shim`, which
 *          needs the RA8P1 register window. This stand-in exists so the test
 *          can assert WHICH registration the resolver hands back for the custom
 *          name; it is never invoked here and asserts nothing about the NPU.
 *
 * @param[in] context Unused kernel context.
 * @param[in] node    Unused graph node.
 *
 * @return `TfLiteStatus` always `kTfLiteOk`.
 * @retval kTfLiteOk Recorded the call.
 *
 * @pre None.
 * @post ::g_fake_invoke_calls is incremented.
 *
 * @note Not thread-safe (single-threaded host test).
 * @since 0.1.0
 */
TfLiteStatus fake_ethosu_invoke(TfLiteContext* context, TfLiteNode* node)
{
  (void)context;
  (void)node;
  ++g_fake_invoke_calls;
  return kTfLiteOk;
}

/** @brief Registration carrying ::fake_ethosu_invoke as its identity. */
TFLMRegistration g_fake_ethosu = {
  /*init=*/nullptr,
  /*free=*/nullptr,
  /*prepare=*/nullptr,
  /*invoke=*/fake_ethosu_invoke,
  /*reset=*/nullptr,
  /*builtin_code=*/0,
  /*custom_name=*/nullptr,
};

/**
 * @brief Register every builtin the vendored kernel set provides.
 *
 * @details One call site for the subset under test, so a kernel added to or
 *          removed from the vendor tree changes this list and nothing else.
 *          `MAX_POOL_2D` is included deliberately: `pooling.cc` registers it,
 *          so it is part of the CPU-fallback surface whatever the build comment
 *          used to say.
 *
 * @param[in,out] r Resolver to populate.
 *
 * @return `unsigned int` count of registrations that reported success.
 *
 * @pre `r` is empty.
 * @post `r` holds one registration per successful add.
 *
 * @note Not thread-safe (single-threaded host test).
 * @since 0.1.0
 */
unsigned int add_vendored_builtins(tflite::MicroMutableOpResolver<k_resolver_slots>& r)
{
  unsigned int ok = 0U;
  ok += (r.AddConv2D() == kTfLiteOk) ? 1U : 0U;
  ok += (r.AddDepthwiseConv2D() == kTfLiteOk) ? 1U : 0U;
  ok += (r.AddFullyConnected() == kTfLiteOk) ? 1U : 0U;
  ok += (r.AddAdd() == kTfLiteOk) ? 1U : 0U;
  ok += (r.AddMul() == kTfLiteOk) ? 1U : 0U;
  ok += (r.AddReshape() == kTfLiteOk) ? 1U : 0U;
  ok += (r.AddSoftmax() == kTfLiteOk) ? 1U : 0U;
  ok += (r.AddAveragePool2D() == kTfLiteOk) ? 1U : 0U;
  ok += (r.AddMaxPool2D() == kTfLiteOk) ? 1U : 0U;
  return ok;
}

/**
 * @brief Every vendored builtin registers and resolves with a live invoke.
 *
 * @details The CPU-fallback surface is only real if each registration carries
 *          an `invoke` handler; the vendored Ethos-U stub this tree replaces is
 *          exactly the shape of a registration that resolves and then does
 *          nothing.
 */
void test_vendored_builtins_resolve(void)
{
  TEST_BEGIN("tflm vendored builtins resolve with a live invoke");
  tflite::MicroMutableOpResolver<k_resolver_slots> r;
  TEST_ASSERT_EQ(k_vendored_builtin, add_vendored_builtins(r));

  const tflite::BuiltinOperator subset[k_vendored_builtin] = {
    tflite::BuiltinOperator_CONV_2D,
    tflite::BuiltinOperator_DEPTHWISE_CONV_2D,
    tflite::BuiltinOperator_FULLY_CONNECTED,
    tflite::BuiltinOperator_ADD,
    tflite::BuiltinOperator_MUL,
    tflite::BuiltinOperator_RESHAPE,
    tflite::BuiltinOperator_SOFTMAX,
    tflite::BuiltinOperator_AVERAGE_POOL_2D,
    tflite::BuiltinOperator_MAX_POOL_2D,
  };
  for (unsigned int i = 0U; i < k_vendored_builtin; ++i) {
    const TFLMRegistration* reg = r.FindOp(subset[i]);
    TEST_ASSERT_NOT_NULL(reg);
    TEST_ASSERT_NOT_NULL((const void*)reg->invoke);
    TEST_ASSERT_EQ((int)subset[i], (int)reg->builtin_code);
  }
  TEST_END("tflm vendored builtins resolve with a live invoke");
}

/**
 * @brief MAX_POOL_2D is in the subset, though the build comment omitted it.
 *
 * @details `pooling.cc` defines `Register_MAX_POOL_2D()` alongside
 *          `Register_AVERAGE_POOL_2D()`, and `cmake/tflite_micro.cmake` globs
 *          the whole file, so the operator has been linkable all along while
 *          the CMake header comment and `docs/SOUP/tflite-micro.md` listed only
 *          the average-pool half. This test is what stops that drift coming
 *          back: it fails to link if the kernel leaves the tree, and fails to
 *          resolve if the registration is dropped.
 */
void test_max_pool_is_in_the_subset(void)
{
  TEST_BEGIN("tflm MAX_POOL_2D is registrable from the vendored pooling kernel");
  const TFLMRegistration max_pool = tflite::Register_MAX_POOL_2D();
  TEST_ASSERT_NOT_NULL((const void*)max_pool.invoke);

  const TFLMRegistration avg_pool = tflite::Register_AVERAGE_POOL_2D();
  TEST_ASSERT_NOT_NULL((const void*)avg_pool.invoke);
  /* Two distinct operators out of one vendored translation unit. */
  TEST_ASSERT(max_pool.invoke != avg_pool.invoke);
  TEST_END("tflm MAX_POOL_2D is registrable from the vendored pooling kernel");
}

/**
 * @brief A builtin outside the subset does not resolve.
 *
 * @details The CPU-fallback boundary stated as a test. `LOGISTIC` is not
 *          vendored, so a Vela-lowered graph that leaves one on the CPU fails
 *          resolution rather than dispatching somewhere wrong. These ops are
 *          referenced only by schema code: calling `AddLogistic()` would
 *          reference `Register_LOGISTIC()` and fail to link, which is itself
 *          the proof the kernel is absent.
 */
void test_unvendored_builtin_does_not_resolve(void)
{
  TEST_BEGIN("tflm a builtin outside the vendored subset does not resolve");
  tflite::MicroMutableOpResolver<k_resolver_slots> r;
  TEST_ASSERT_EQ(k_vendored_builtin, add_vendored_builtins(r));
  TEST_ASSERT_NULL(r.FindOp(tflite::BuiltinOperator_LOGISTIC));
  TEST_ASSERT_NULL(r.FindOp(tflite::BuiltinOperator_CONCATENATION));
  TEST_ASSERT_NULL(r.FindOp(tflite::BuiltinOperator_QUANTIZE));
  TEST_END("tflm a builtin outside the vendored subset does not resolve");
}

/**
 * @brief The Ethos-U custom op registers under the first-party published name.
 *
 * @details The resolver keys a custom operator on a string, and the only
 *          authority for that string is `tflite::ethosu_custom_name()` in the
 *          first-party kernel. Registering under the published name and finding
 *          it back is what proves the two halves agree; a literal "ethos-u" in
 *          the test would prove only that the test agrees with itself.
 */
void test_ethosu_custom_name_registers(void)
{
  TEST_BEGIN("tflm the ethos-u custom op registers under the published name");
  const char* name = tflite::ethosu_custom_name();
  TEST_ASSERT_NOT_NULL(name);

  tflite::MicroMutableOpResolver<k_resolver_slots> r;
  TEST_ASSERT_EQ(kTfLiteOk, r.AddCustom(name, &g_fake_ethosu));

  const TFLMRegistration* reg = r.FindOp(name);
  TEST_ASSERT_NOT_NULL(reg);
  TEST_ASSERT((const void*)reg->invoke == (const void*)fake_ethosu_invoke);
  TEST_ASSERT_EQ((int)tflite::BuiltinOperator_CUSTOM, (int)reg->builtin_code);
  TEST_ASSERT_NOT_NULL(reg->custom_name);
  /* AddCustom stores the caller's pointer, so the resolver's key IS the name
   * the first-party kernel published, not a copy that could drift. */
  TEST_ASSERT((const void*)reg->custom_name == (const void*)name);
  TEST_END("tflm the ethos-u custom op registers under the published name");
}

/**
 * @brief An unregistered custom name resolves to nothing.
 *
 * @details Registering one custom operator must not make every custom node
 *          resolve: a model naming a different Ethos-U revision has to fail
 *          rather than land on this dispatcher.
 */
void test_unknown_custom_name_does_not_resolve(void)
{
  TEST_BEGIN("tflm an unregistered custom name does not resolve");
  tflite::MicroMutableOpResolver<k_resolver_slots> r;
  TEST_ASSERT_EQ(kTfLiteOk, r.AddCustom(tflite::ethosu_custom_name(), &g_fake_ethosu));
  TEST_ASSERT_NULL(r.FindOp("ethos-u-v2"));
  TEST_ASSERT_NULL(r.FindOp("TFLite_Detection_PostProcess"));
  TEST_END("tflm an unregistered custom name does not resolve");
}

/**
 * @brief Registering the Ethos-U op twice is refused and changes nothing.
 *
 * @details Both the vendored stub kernel and the first-party kernel define
 *          `Register_ETHOSU()`, and `cmake/tflite_micro.cmake` keeps them apart
 *          by excluding the vendored file. If that exclusion ever lapses, the
 *          second registration must lose rather than silently replace the
 *          first, so the guard is pinned here.
 */
void test_double_registration_is_refused(void)
{
  TEST_BEGIN("tflm registering the ethos-u op twice is refused");
  tflite::MicroMutableOpResolver<k_resolver_slots> r;
  const char* name = tflite::ethosu_custom_name();
  TEST_ASSERT_EQ(kTfLiteOk, r.AddCustom(name, &g_fake_ethosu));

  TFLMRegistration second = g_fake_ethosu;
  second.invoke           = nullptr;
  TEST_ASSERT_EQ(kTfLiteError, r.AddCustom(name, &second));

  const TFLMRegistration* reg = r.FindOp(name);
  TEST_ASSERT_NOT_NULL(reg);
  /* The first registration survived: the refusal is not a replacement. */
  TEST_ASSERT((const void*)reg->invoke == (const void*)fake_ethosu_invoke);
  TEST_END("tflm registering the ethos-u op twice is refused");
}

/**
 * @brief Off-target the first-party kernel declines to offer a registration.
 *
 * @details `ra8_ethosu_kernel.cc` documents that without `RA8_HAS_NPU` its
 *          `Register_ETHOSU()` returns `nullptr` (the CPU-only TFLite path)
 *          while `ethosu_custom_name()` still publishes the name. That contract
 *          is checked here on a host build, and it is why a resolver keys on the
 *          name: the registration does not exist on every device, the name does.
 */
void test_register_ethosu_is_null_off_target(void)
{
  TEST_BEGIN("tflm Register_ETHOSU yields nothing on a device with no NPU");
  TEST_ASSERT_NULL(tflite::Register_ETHOSU());
  TEST_ASSERT_NOT_NULL(tflite::ethosu_custom_name());
  /* No test in this file invoked the stand-in, so nothing dispatched. */
  TEST_ASSERT_EQ(0, g_fake_invoke_calls);
  TEST_END("tflm Register_ETHOSU yields nothing on a device with no NPU");
}

} // namespace

/**
 * @brief Run the operator-subset and custom-op registration tests.
 *
 * @return `int` process status.
 * @retval 0 Every test passed (the harness aborts on the first failure).
 *
 * @pre None; no NPU, no silicon, no model.
 * @post No global state outside this translation unit is modified.
 *
 * @note test_register_ethosu_is_null_off_target runs last: it asserts the
 *       stand-in dispatcher was never invoked, which only holds once every
 *       other test has run.
 * @since 0.1.0
 */
int main(void)
{
  test_vendored_builtins_resolve();
  test_max_pool_is_in_the_subset();
  test_unvendored_builtin_does_not_resolve();
  test_ethosu_custom_name_registers();
  test_unknown_custom_name_does_not_resolve();
  test_double_registration_is_refused();
  test_register_ethosu_is_null_off_target();
  return 0;
}
