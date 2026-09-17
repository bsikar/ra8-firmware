/**
 * @file test_ra8_tflm_interpreter.cc
 * @brief Model-driven `MicroInterpreter` host tests for the RA8P1 inference
 *        runtime: real graphs, a static arena, and the off-target NPU refusal
 *        (issue #228)
 *
 * @details
 * The residual #228 names is the runtime half of the Ethos-U55 story, and the
 * first line of it is blunt: the `MicroInterpreter` model-driven path has never
 * executed anywhere in this tree. `test_ra8_tflm_op_subset.cc` pins which
 * operators resolve, which is the registration surface; it never builds a
 * model, never plans an arena, and never invokes a graph. So "TFLite-micro is
 * vendored and integrated" rested on a link, not on a run.
 *
 * This file executes it. Each test builds a real `.tflite` FlatBuffer in memory
 * with the vendored schema bindings, hands it to `tflite::MicroInterpreter` over
 * a static arena, and checks the tensors that come back. Nothing is read from a
 * committed golden blob, so the model under test is visible in the test itself.
 *
 * The quantization is chosen so the INT8 arithmetic is EXACT: input scale 0.5,
 * per-channel weight scale 0.5, output scale 0.25, every zero point 0, so the
 * requantization multiplier is exactly 1.0 and the expected output bytes are
 * plain integer arithmetic over the weights and bias. A test that had to model
 * the kernel's fixed-point rounding to state its expectation would be pinning
 * the reference kernel's internals rather than the runtime's behaviour.
 *
 * What this canNOT do, and does not claim: no Ethos-U operator runs here. Off
 * target `RA8_HAS_NPU` is undefined, `tflite::Register_ETHOSU()` is `nullptr` by
 * the first-party kernel's own contract, and the last test pins the consequence
 * rather than papering over it -- a Vela-lowered graph is REFUSED at
 * `AllocateTensors()` on a host build instead of silently running as something
 * else. Executing that node needs the real command stream (#227) and RA8P1
 * silicon (#229).
 *
 * @note The refusal tests make the vendored runtime print its own diagnostics
 *       to stderr on the way to returning `kTfLiteError`. That output is
 *       expected; the harness verdict is what matters.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "unity_minimal.h"

#include <cstdint>
#include <cstring>

namespace tflite {
/** @brief Declared by the first-party kernel; nullptr off-target by contract. */
TFLMRegistration* Register_ETHOSU();
/** @brief The custom-operator name Vela emits and the resolver keys on. */
const char* ethosu_custom_name();
} // namespace tflite

namespace {

/**
 * @brief Fixture geometry, arena sizes and the exact quantization contract.
 *
 * @details The scales are not arbitrary. `k_in_scale * k_w_scale` is the bias
 *          scale AND the output scale, so the kernel's requantization
 *          multiplier is exactly 1.0 and the accumulator reaches the output
 *          byte unrounded. That is what lets ::k_expect_conv be written as
 *          integer arithmetic instead of a fixed-point simulation.
 */
enum : int {
  k_in_h      = 2,  /**< Input rows.                            */
  k_in_w      = 2,  /**< Input columns.                         */
  k_in_ch     = 2,  /**< Input channels.                        */
  k_out_ch    = 2,  /**< Output channels, one per weight row.   */
  k_conv_vals = 8,  /**< Elements in the conv input and output. */
  k_resolvers = 8,  /**< Resolver registration slots.           */
};

/** @brief Input quantization scale; see the note on exactness above. */
constexpr float k_in_scale = 0.5F;
/** @brief Per-channel weight scale, identical on both output channels. */
constexpr float k_w_scale = 0.5F;
/** @brief Output and bias scale: `k_in_scale * k_w_scale`. */
constexpr float k_out_scale = 0.25F;

/** @brief Arena large enough for either fixture graph. */
constexpr size_t k_arena_bytes = 16384U;
/** @brief An arena deliberately too small to plan the conv graph. */
constexpr size_t k_tiny_arena_bytes = 512U;

/** @brief 1x1 weights, `[out_ch][1][1][in_ch]`, quantized dimension 0. */
constexpr int8_t k_weights[k_out_ch * k_in_ch] = {1, 2, 3, -1};
/** @brief INT32 bias in units of ::k_out_scale, one per output channel. */
constexpr int32_t k_bias[k_out_ch] = {4, -5};
/** @brief Input bytes, NHWC, four spatial positions of two channels. */
constexpr int8_t k_input[k_conv_vals] = {1, 2, 3, 4, 5, 6, -7, 8};

/**
 * @brief The exact output the conv fixture must produce.
 *
 * @details Per position `(a, b)` and channel `c`:
 *          `out = w[c][0] * a + w[c][1] * b + bias[c]`, because the multiplier
 *          is 1.0 and every zero point is 0. Worked through: (1,2) -> 9, -4;
 *          (3,4) -> 15, 0; (5,6) -> 21, 4; (-7,8) -> 13, -34.
 */
constexpr int8_t k_expect_conv[k_conv_vals] = {9, -4, 15, 0, 21, 4, 13, -34};

/** @brief Arena for the interpreter under test; 16-byte aligned as required. */
alignas(16) uint8_t g_arena[k_arena_bytes];
/** @brief The undersized arena of ::k_tiny_arena_bytes. */
alignas(16) uint8_t g_tiny_arena[k_tiny_arena_bytes];

/** @brief Flatbuffer offsets shared by the fixture builders. */
using Offsets = flatbuffers::FlatBufferBuilder;

/**
 * @brief Build one per-tensor quantization record.
 *
 * @param[in,out] fbb   Builder the record is emitted into.
 * @param[in]     scale Quantization scale.
 *
 * @return `flatbuffers::Offset<tflite::QuantizationParameters>` the record.
 *
 * @pre `fbb` is not finished.
 * @post One quantization table is written into `fbb`.
 *
 * @note Zero point is always 0 in this fixture; see ::k_in_scale.
 * @since 0.1.0
 */
flatbuffers::Offset<tflite::QuantizationParameters> quant_one(Offsets& fbb, float scale)
{
  const float   scales[1]      = {scale};
  const int64_t zero_points[1] = {0};
  return tflite::CreateQuantizationParameters(
      fbb, 0, 0, fbb.CreateVector(scales, 1U), fbb.CreateVector(zero_points, 1U)
  );
}

/**
 * @brief Build a per-channel quantization record over ::k_out_ch channels.
 *
 * @param[in,out] fbb   Builder the record is emitted into.
 * @param[in]     scale Scale, identical on every channel.
 *
 * @return `flatbuffers::Offset<tflite::QuantizationParameters>` the record.
 *
 * @pre `fbb` is not finished.
 * @post One quantization table is written into `fbb`.
 *
 * @note `quantized_dimension` is 0, matching the weight layout Vela emits.
 * @since 0.1.0
 */
flatbuffers::Offset<tflite::QuantizationParameters> quant_per_channel(Offsets& fbb, float scale)
{
  const float   scales[k_out_ch]      = {scale, scale};
  const int64_t zero_points[k_out_ch] = {0, 0};
  return tflite::CreateQuantizationParameters(
      fbb,
      0,
      0,
      fbb.CreateVector(scales, static_cast<size_t>(k_out_ch)),
      fbb.CreateVector(zero_points, static_cast<size_t>(k_out_ch)),
      tflite::QuantizationDetails_NONE,
      0,
      0
  );
}

/**
 * @brief Build the INT8 1x1 CONV_2D fixture model, optionally plus a RESHAPE.
 *
 * @param[in,out] fbb          Builder the model is emitted into and finished on.
 * @param[in]     with_reshape When true, a second node reshapes `[1,2,2,2]` to
 *                             `[1,8]`, so the graph exercises the interpreter's
 *                             multi-node walk rather than a single kernel.
 *
 * @return `const tflite::Model*` the finished model, valid while `fbb` lives.
 * @retval nullptr Never; construction cannot fail.
 *
 * @pre `fbb` is fresh.
 * @post `fbb` is finished and holds a complete TFLite model.
 *
 * @note Padding VALID, stride 1, no fused activation: a 1x1 kernel over a 2x2
 *       input keeps the spatial geometry, so the expectation stays readable.
 * @since 0.1.0
 */
const tflite::Model* build_conv_model(Offsets& fbb, bool with_reshape)
{
  flatbuffers::Offset<tflite::Buffer> buffers[4];
  buffers[0] = tflite::CreateBuffer(fbb);
  buffers[1] = tflite::CreateBuffer(
      fbb, fbb.CreateVector(reinterpret_cast<const uint8_t*>(k_weights), sizeof(k_weights))
  );
  buffers[2] = tflite::CreateBuffer(
      fbb, fbb.CreateVector(reinterpret_cast<const uint8_t*>(k_bias), sizeof(k_bias))
  );
  const int32_t shape_param[2] = {1, k_conv_vals};
  buffers[3]                   = tflite::CreateBuffer(
      fbb, fbb.CreateVector(reinterpret_cast<const uint8_t*>(shape_param), sizeof(shape_param))
  );

  const int32_t in_shape[4]  = {1, k_in_h, k_in_w, k_in_ch};
  const int32_t w_shape[4]   = {k_out_ch, 1, 1, k_in_ch};
  const int32_t b_shape[1]   = {k_out_ch};
  const int32_t out_shape[4] = {1, k_in_h, k_in_w, k_out_ch};
  const int32_t flat_shape[2] = {1, k_conv_vals};
  const int32_t param_shape[1] = {2};

  flatbuffers::Offset<tflite::Tensor> tensors[6];
  tensors[0] = tflite::CreateTensor(
      fbb,
      fbb.CreateVector(in_shape, 4U),
      tflite::TensorType_INT8,
      0,
      fbb.CreateString("input"),
      quant_one(fbb, k_in_scale)
  );
  tensors[1] = tflite::CreateTensor(
      fbb,
      fbb.CreateVector(w_shape, 4U),
      tflite::TensorType_INT8,
      1,
      fbb.CreateString("weights"),
      quant_per_channel(fbb, k_w_scale)
  );
  tensors[2] = tflite::CreateTensor(
      fbb,
      fbb.CreateVector(b_shape, 1U),
      tflite::TensorType_INT32,
      2,
      fbb.CreateString("bias"),
      quant_per_channel(fbb, k_out_scale)
  );
  tensors[3] = tflite::CreateTensor(
      fbb,
      fbb.CreateVector(out_shape, 4U),
      tflite::TensorType_INT8,
      0,
      fbb.CreateString("conv_out"),
      quant_one(fbb, k_out_scale)
  );
  tensors[4] = tflite::CreateTensor(
      fbb,
      fbb.CreateVector(param_shape, 1U),
      tflite::TensorType_INT32,
      3,
      fbb.CreateString("new_shape"),
      0
  );
  tensors[5] = tflite::CreateTensor(
      fbb,
      fbb.CreateVector(flat_shape, 2U),
      tflite::TensorType_INT8,
      0,
      fbb.CreateString("flat_out"),
      quant_one(fbb, k_out_scale)
  );

  flatbuffers::Offset<tflite::OperatorCode> codes[2];
  codes[0] = tflite::CreateOperatorCode(fbb, 0, 0, 1, tflite::BuiltinOperator_CONV_2D);
  codes[1] = tflite::CreateOperatorCode(fbb, 0, 0, 1, tflite::BuiltinOperator_RESHAPE);

  const int32_t conv_in[3]  = {0, 1, 2};
  const int32_t conv_out[1] = {3};
  flatbuffers::Offset<tflite::Operator> ops[2];
  ops[0] = tflite::CreateOperator(
      fbb,
      0,
      fbb.CreateVector(conv_in, 3U),
      fbb.CreateVector(conv_out, 1U),
      tflite::BuiltinOptions_Conv2DOptions,
      tflite::CreateConv2DOptions(
          fbb, tflite::Padding_VALID, 1, 1, tflite::ActivationFunctionType_NONE, 1, 1
      )
          .Union()
  );

  const int32_t reshape_in[2]  = {3, 4};
  const int32_t reshape_out[1] = {5};
  ops[1]                       = tflite::CreateOperator(
      fbb,
      1,
      fbb.CreateVector(reshape_in, 2U),
      fbb.CreateVector(reshape_out, 1U),
      tflite::BuiltinOptions_ReshapeOptions,
      tflite::CreateReshapeOptions(fbb, fbb.CreateVector(flat_shape, 2U)).Union()
  );

  const int32_t graph_in[1]   = {0};
  const int32_t conv_only[1]  = {3};
  const int32_t reshaped[1]   = {5};
  const size_t  tensor_count  = with_reshape ? 6U : 4U;
  const size_t  op_count      = with_reshape ? 2U : 1U;
  const int32_t* graph_out    = with_reshape ? reshaped : conv_only;

  const flatbuffers::Offset<tflite::SubGraph> subgraph = tflite::CreateSubGraph(
      fbb,
      fbb.CreateVector(tensors, tensor_count),
      fbb.CreateVector(graph_in, 1U),
      fbb.CreateVector(graph_out, 1U),
      fbb.CreateVector(ops, op_count),
      fbb.CreateString("conv_int8")
  );

  const flatbuffers::Offset<tflite::Model> model = tflite::CreateModel(
      fbb,
      TFLITE_SCHEMA_VERSION,
      fbb.CreateVector(codes, op_count),
      fbb.CreateVector(&subgraph, 1U),
      fbb.CreateString("ra8 #228 interpreter fixture"),
      fbb.CreateVector(buffers, 4U)
  );
  fbb.Finish(model, tflite::ModelIdentifier());
  return tflite::GetModel(fbb.GetBufferPointer());
}

/**
 * @brief Build a one-node graph whose only operator is the Ethos-U custom op.
 *
 * @param[in,out] fbb Builder the model is emitted into and finished on.
 *
 * @return `const tflite::Model*` the finished model, valid while `fbb` lives.
 * @retval nullptr Never; construction cannot fail.
 *
 * @pre `fbb` is fresh.
 * @post `fbb` is finished and holds a complete TFLite model.
 *
 * @note The custom code is taken from `tflite::ethosu_custom_name()`, not a
 *       literal, so the fixture and the kernel cannot drift apart.
 * @since 0.1.0
 */
const tflite::Model* build_ethosu_model(Offsets& fbb)
{
  flatbuffers::Offset<tflite::Buffer> buffers[1];
  buffers[0] = tflite::CreateBuffer(fbb);

  const int32_t shape[2] = {1, k_conv_vals};
  flatbuffers::Offset<tflite::Tensor> tensors[2];
  tensors[0] = tflite::CreateTensor(
      fbb,
      fbb.CreateVector(shape, 2U),
      tflite::TensorType_INT8,
      0,
      fbb.CreateString("input"),
      quant_one(fbb, k_in_scale)
  );
  tensors[1] = tflite::CreateTensor(
      fbb,
      fbb.CreateVector(shape, 2U),
      tflite::TensorType_INT8,
      0,
      fbb.CreateString("output"),
      quant_one(fbb, k_out_scale)
  );

  flatbuffers::Offset<tflite::OperatorCode> codes[1];
  codes[0] = tflite::CreateOperatorCode(
      fbb,
      0,
      fbb.CreateString(tflite::ethosu_custom_name()),
      1,
      tflite::BuiltinOperator_CUSTOM
  );

  const int32_t op_in[1]  = {0};
  const int32_t op_out[1] = {1};
  flatbuffers::Offset<tflite::Operator> ops[1];
  ops[0] = tflite::CreateOperator(
      fbb, 0, fbb.CreateVector(op_in, 1U), fbb.CreateVector(op_out, 1U)
  );

  const flatbuffers::Offset<tflite::SubGraph> subgraph = tflite::CreateSubGraph(
      fbb,
      fbb.CreateVector(tensors, 2U),
      fbb.CreateVector(op_in, 1U),
      fbb.CreateVector(op_out, 1U),
      fbb.CreateVector(ops, 1U),
      fbb.CreateString("ethosu_only")
  );

  const flatbuffers::Offset<tflite::Model> model = tflite::CreateModel(
      fbb,
      TFLITE_SCHEMA_VERSION,
      fbb.CreateVector(codes, 1U),
      fbb.CreateVector(&subgraph, 1U),
      fbb.CreateString("ra8 #228 ethos-u refusal fixture"),
      fbb.CreateVector(buffers, 1U)
  );
  fbb.Finish(model, tflite::ModelIdentifier());
  return tflite::GetModel(fbb.GetBufferPointer());
}

/** @brief The resolver type every test here uses. */
using Resolver = tflite::MicroMutableOpResolver<k_resolvers>;

/**
 * @brief Register the two builtins the fixture graphs need.
 *
 * @param[in,out] resolver Resolver to populate.
 *
 * @return `void`
 *
 * @pre `resolver` is empty.
 * @post CONV_2D and RESHAPE are registered.
 *
 * @note Both come from the lean vendored subset pinned by
 *       test_ra8_tflm_op_subset.cc.
 * @since 0.1.0
 */
void register_cpu_ops(Resolver& resolver)
{
  TEST_ASSERT_EQ(kTfLiteOk, resolver.AddConv2D());
  TEST_ASSERT_EQ(kTfLiteOk, resolver.AddReshape());
}

/** @brief Arena bytes the conv fixture actually needed, for the arena test. */
size_t g_conv_arena_used = 0U;

/**
 * @brief A single-node INT8 CONV_2D model runs and produces exact bytes.
 */
void test_conv_int8_model_runs(void)
{
  TEST_BEGIN("tflm interpreter runs an int8 conv model end to end");
  Offsets              fbb(4096U);
  const tflite::Model* model = build_conv_model(fbb, false);
  TEST_ASSERT_NOT_NULL(model);
  TEST_ASSERT_EQ(TFLITE_SCHEMA_VERSION, static_cast<int>(model->version()));

  Resolver resolver;
  register_cpu_ops(resolver);

  tflite::MicroInterpreter interpreter(model, resolver, g_arena, k_arena_bytes);
  TEST_ASSERT_EQ(kTfLiteOk, interpreter.AllocateTensors());
  TEST_ASSERT_EQ(1U, interpreter.inputs_size());
  TEST_ASSERT_EQ(1U, interpreter.outputs_size());

  TfLiteTensor* in = interpreter.input(0);
  TEST_ASSERT_NOT_NULL(in);
  TEST_ASSERT_EQ(kTfLiteInt8, in->type);
  TEST_ASSERT_EQ(4, in->dims->size);
  TEST_ASSERT_EQ(k_conv_vals, static_cast<int>(in->bytes));
  TEST_ASSERT(in->params.scale == k_in_scale);
  TEST_ASSERT_EQ(0, in->params.zero_point);
  std::memcpy(in->data.int8, k_input, sizeof(k_input));

  TEST_ASSERT_EQ(kTfLiteOk, interpreter.Invoke());

  TfLiteTensor* out = interpreter.output(0);
  TEST_ASSERT_NOT_NULL(out);
  TEST_ASSERT_EQ(kTfLiteInt8, out->type);
  TEST_ASSERT_EQ(k_conv_vals, static_cast<int>(out->bytes));
  TEST_ASSERT(out->params.scale == k_out_scale);
  for (int i = 0; i < k_conv_vals; ++i) {
    TEST_ASSERT_EQ(static_cast<int>(k_expect_conv[i]), static_cast<int>(out->data.int8[i]));
  }

  g_conv_arena_used = interpreter.arena_used_bytes();
  TEST_ASSERT(g_conv_arena_used > 0U);
  TEST_ASSERT(g_conv_arena_used <= k_arena_bytes);
  TEST_END("tflm interpreter runs an int8 conv model end to end");
}

/**
 * @brief Invoking the same model twice on new input recomputes the output.
 */
void test_second_invoke_recomputes(void)
{
  TEST_BEGIN("tflm interpreter recomputes on a second invoke");
  Offsets              fbb(4096U);
  const tflite::Model* model = build_conv_model(fbb, false);
  Resolver             resolver;
  register_cpu_ops(resolver);

  tflite::MicroInterpreter interpreter(model, resolver, g_arena, k_arena_bytes);
  TEST_ASSERT_EQ(kTfLiteOk, interpreter.AllocateTensors());

  TfLiteTensor* in = interpreter.input(0);
  std::memset(in->data.int8, 0, static_cast<size_t>(k_conv_vals));
  TEST_ASSERT_EQ(kTfLiteOk, interpreter.Invoke());
  TfLiteTensor* out = interpreter.output(0);
  /* Zero input leaves the bias alone: 4 and -5 in output units. */
  TEST_ASSERT_EQ(4, static_cast<int>(out->data.int8[0]));
  TEST_ASSERT_EQ(-5, static_cast<int>(out->data.int8[1]));

  std::memcpy(in->data.int8, k_input, sizeof(k_input));
  TEST_ASSERT_EQ(kTfLiteOk, interpreter.Invoke());
  out = interpreter.output(0);
  TEST_ASSERT_EQ(static_cast<int>(k_expect_conv[0]), static_cast<int>(out->data.int8[0]));
  TEST_ASSERT_EQ(static_cast<int>(k_expect_conv[7]), static_cast<int>(out->data.int8[7]));
  TEST_END("tflm interpreter recomputes on a second invoke");
}

/**
 * @brief A two-node graph walks both kernels and reshapes the result.
 */
void test_two_node_graph_runs(void)
{
  TEST_BEGIN("tflm interpreter walks a two-node conv plus reshape graph");
  Offsets              fbb(4096U);
  const tflite::Model* model = build_conv_model(fbb, true);
  Resolver             resolver;
  register_cpu_ops(resolver);

  tflite::MicroInterpreter interpreter(model, resolver, g_arena, k_arena_bytes);
  TEST_ASSERT_EQ(kTfLiteOk, interpreter.AllocateTensors());

  TfLiteTensor* in = interpreter.input(0);
  std::memcpy(in->data.int8, k_input, sizeof(k_input));
  TEST_ASSERT_EQ(kTfLiteOk, interpreter.Invoke());

  TfLiteTensor* out = interpreter.output(0);
  TEST_ASSERT_NOT_NULL(out);
  /* The reshape is the observable difference: rank 2, not rank 4. */
  TEST_ASSERT_EQ(2, out->dims->size);
  TEST_ASSERT_EQ(1, out->dims->data[0]);
  TEST_ASSERT_EQ(k_conv_vals, out->dims->data[1]);
  for (int i = 0; i < k_conv_vals; ++i) {
    TEST_ASSERT_EQ(static_cast<int>(k_expect_conv[i]), static_cast<int>(out->data.int8[i]));
  }
  TEST_END("tflm interpreter walks a two-node conv plus reshape graph");
}

/**
 * @brief An arena too small to plan the graph is refused, not overrun.
 */
void test_small_arena_is_refused(void)
{
  TEST_BEGIN("tflm interpreter refuses an arena smaller than the graph needs");
  /* The conv test established the real requirement. */
  TEST_ASSERT(g_conv_arena_used > k_tiny_arena_bytes);

  Offsets              fbb(4096U);
  const tflite::Model* model = build_conv_model(fbb, false);
  Resolver             resolver;
  register_cpu_ops(resolver);

  tflite::MicroInterpreter interpreter(model, resolver, g_tiny_arena, k_tiny_arena_bytes);
  TEST_ASSERT_EQ(kTfLiteError, interpreter.AllocateTensors());
  /* A refused allocation must not leave an invokable interpreter behind. */
  TEST_ASSERT_EQ(kTfLiteError, interpreter.Invoke());
  TEST_END("tflm interpreter refuses an arena smaller than the graph needs");
}

/**
 * @brief A graph missing one operator is refused at allocation.
 */
void test_unregistered_op_is_refused(void)
{
  TEST_BEGIN("tflm interpreter refuses a graph whose operator is unregistered");
  Offsets              fbb(4096U);
  const tflite::Model* model = build_conv_model(fbb, true);
  Resolver             resolver;
  /* CONV_2D only: the reshape node has no registration. */
  TEST_ASSERT_EQ(kTfLiteOk, resolver.AddConv2D());

  tflite::MicroInterpreter interpreter(model, resolver, g_arena, k_arena_bytes);
  TEST_ASSERT_EQ(kTfLiteError, interpreter.AllocateTensors());
  TEST_END("tflm interpreter refuses a graph whose operator is unregistered");
}

/**
 * @brief A Vela-shaped Ethos-U graph is refused on a host build, by name.
 *
 * @details This is the honest edge of #228 off silicon. `Register_ETHOSU()` is
 *          `nullptr` here because `RA8_HAS_NPU` is undefined, so the custom op
 *          cannot be registered at all and the graph cannot run. The refusal
 *          has to happen at `AllocateTensors()`: a host build must not decide
 *          to execute an NPU node as something else.
 *
 *          Note for anyone registering the kernel on a real device:
 *          `MicroMutableOpResolver::AddCustom()` copies through its pointer
 *          without a null check, so the caller has to test
 *          `Register_ETHOSU()` first. That is why this test never passes the
 *          off-target nullptr to the resolver.
 */
void test_ethosu_graph_is_refused_off_target(void)
{
  TEST_BEGIN("tflm interpreter refuses a Vela ethos-u graph off target");
  TEST_ASSERT_NULL(tflite::Register_ETHOSU());
  TEST_ASSERT_NOT_NULL(tflite::ethosu_custom_name());

  Offsets              fbb(4096U);
  const tflite::Model* model = build_ethosu_model(fbb);
  TEST_ASSERT_NOT_NULL(model);

  Resolver resolver;
  register_cpu_ops(resolver);

  tflite::MicroInterpreter interpreter(model, resolver, g_arena, k_arena_bytes);
  TEST_ASSERT_EQ(kTfLiteError, interpreter.AllocateTensors());
  TEST_END("tflm interpreter refuses a Vela ethos-u graph off target");
}

} // namespace

/**
 * @brief Run the model-driven interpreter tests.
 *
 * @return `int` process status.
 * @retval 0 Every test passed (the harness aborts on the first failure).
 *
 * @pre None; no NPU, no silicon, no committed model blob.
 * @post No global state outside this translation unit is modified.
 *
 * @note test_conv_int8_model_runs runs first: it records the arena the graph
 *       actually needs, which the undersized-arena test then compares against.
 * @since 0.1.0
 */
int main(void)
{
  test_conv_int8_model_runs();
  test_second_invoke_recomputes();
  test_two_node_graph_runs();
  test_small_arena_is_refused();
  test_unregistered_op_is_refused();
  test_ethosu_graph_is_refused_off_target();
  return 0;
}
