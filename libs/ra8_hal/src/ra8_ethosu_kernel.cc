/**
 * @file ra8_ethosu_kernel.cc
 * @brief First-party TFLite-micro Ethos-U55 custom operator over `ra8_ethosu_shim`
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * This is the REAL `tflite::Register_ETHOSU()` operator kernel for this project,
 * replacing the vendored portable stub
 * (`libs/third_party/tflite-micro/.../kernels/ethosu.cc`, which returned
 * `nullptr`). The vendored stub is excluded from the TFLite-micro object library
 * by `cmake/tflite_micro.cmake`, and this first-party translation unit is
 * compiled into that library in its place, so `MicroInterpreter` resolves the
 * `ethos-u` custom operator to this dispatcher.
 *
 * On invoke it translates the TFLite-micro node into the Arm Ethos-U operator ABI
 * and drives the NPU through the first-party `ra8_ethosu_shim`
 * (`ethosu_reserve_driver` / `ethosu_invoke_v3` / `ethosu_release_driver`), which
 * runs on top of the `ra8_npu` HAL. It vendors NO Arm `ethos-u-core-driver` code:
 * the shim provides those symbols.
 *
 * ## Tensor -> region convention (Ethos-U / Vela)
 *
 * A Vela-lowered `ethos-u` custom op takes the command stream as its FIRST input
 * tensor; the remaining input tensors plus all output tensors are the arenas the
 * command stream references by region index (BASEPn). This kernel therefore
 * points `QBASE`/`QSIZE` at input 0 and programs each subsequent input, then each
 * output, into successive region base pointers, bounded to the NPU's
 * `k_ra8_npu_region_count` capacity.
 *
 * ## Status (honest)
 *
 * The kernel COMPILES and LINKS for the RA8P1 and its registration is
 * host/sim-checkable via ::ra8_ethosu_kernel_available. A full model-driven run
 * additionally needs a Vela-compiled `.tflite` (the offline Vela compiler is a
 * separate follow-up) and real silicon: board_sim's honest NPU model rejects a
 * real Vela command stream it cannot interpret, so the end-to-end inference path
 * is exercised on-silicon, not in the emulator. The `custom_name` / `invoke`
 * wiring proven here is the piece this change lands.
 *
 * ## Warning boundary
 *
 * Built inside the vendored TFLite-micro object library (`-w`) because it is
 * dominated by the SOUP TFLite include tree; it stays pure 7-bit ASCII and fully
 * documented like every other first-party TU.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_device.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/micro/micro_common.h"

#ifdef RA8_HAS_NPU
#include "ra8_ethosu_kernel.h"
#include "ra8_ethosu_shim.h"
#include "ra8_npu_regs.h"
#include "tensorflow/lite/micro/kernels/kernel_util.h"
#include "tensorflow/lite/micro/micro_utils.h"
#endif /* RA8_HAS_NPU */

namespace tflite {
namespace {

/** @brief Custom-operator name the Vela compiler emits and the resolver keys on. */
constexpr char k_ethosu_op_name[] = "ethos-u";

#ifdef RA8_HAS_NPU

/**
 * @brief Collect the NPU region base pointers/sizes from a node's tensors.
 *
 * @details Programs each input tensor after input 0 (the command stream), then
 *          each output tensor, into successive Ethos-U region slots per the Vela
 *          convention, bounded to `k_ra8_npu_region_count`.
 *
 * @param[in]  context   TFLite-micro kernel context (tensor accessors).
 * @param[in]  node      Graph node whose inputs (past 0) and outputs are regions.
 * @param[out] base_addr Region base addresses, `k_ra8_npu_region_count` entries.
 * @param[out] base_size Region byte sizes, `k_ra8_npu_region_count` entries.
 * @param[in,out] region_n Running region count, advanced per programmed slot.
 *
 * @return `bool` success.
 * @retval true  Every referenced tensor resolved and was programmed.
 * @retval false A referenced tensor was null (malformed node).
 *
 * @pre `base_addr` / `base_size` hold `k_ra8_npu_region_count` entries.
 * @pre `region_n` points at a valid, initialised counter.
 * @post `*region_n` never exceeds `k_ra8_npu_region_count`.
 * @post On `false` the partially-filled arrays must not be used.
 *
 * @note Not thread-safe (the single NPU is a serialized engine).
 * @since 0.1.0
 */
bool ethosu_collect_regions(TfLiteContext* context,
                            TfLiteNode*    node,
                            uint64_t*      base_addr,
                            size_t*        base_size,
                            int*           region_n)
{
  const int region_cap = static_cast<int>(k_ra8_npu_region_count);

  const int num_inputs = node->inputs->size;
  for (int i = 1; (i < num_inputs) && (*region_n < region_cap); ++i) {
    const TfLiteEvalTensor* t = tflite::micro::GetEvalInput(context, node, i);
    if (t == nullptr) {
      return false;
    }
    base_addr[*region_n] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(t->data.data));
    base_size[*region_n] = tflite::EvalTensorBytes(t);
    ++(*region_n);
  }

  const int num_outputs = (node->outputs != nullptr) ? node->outputs->size : 0;
  for (int o = 0; (o < num_outputs) && (*region_n < region_cap); ++o) {
    TfLiteEvalTensor* t = tflite::micro::GetEvalOutput(context, node, o);
    if (t == nullptr) {
      return false;
    }
    base_addr[*region_n] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(t->data.data));
    base_size[*region_n] = tflite::EvalTensorBytes(t);
    ++(*region_n);
  }
  return true;
}

/**
 * @brief Invoke the Ethos-U55 command stream for one graph node via the shim.
 *
 * @details Builds the (command stream + region base) arguments from the node's
 *          tensors per the Vela convention and calls the `ra8_ethosu_shim`
 *          reserve/invoke/release sequence. Returns `kTfLiteError` on any bad
 *          node shape, driver reservation failure, or NPU fault.
 *
 * @param[in] context TFLite-micro kernel context (tensor accessors).
 * @param[in] node    Graph node: input 0 is the command stream; the remaining
 *                    inputs and all outputs are the NPU tensor regions.
 *
 * @return `TfLiteStatus`.
 * @retval kTfLiteOk    The command stream ran to completion with no NPU fault.
 * @retval kTfLiteError Malformed node, no NPU handle, or an NPU fault/timeout.
 *
 * @pre `node->inputs` holds at least the command-stream tensor.
 * @pre The NPU clock tree is up (the shim lazily runs `ra8_npu_init()`).
 * @post On `kTfLiteOk` the output tensors hold the NPU result.
 * @post No partial result is reported as success.
 *
 * @note Not thread-safe (the single NPU is a serialized engine).
 * @since 0.1.0
 */
TfLiteStatus ethosu_eval(TfLiteContext* context, TfLiteNode* node)
{
  if ((node->inputs == nullptr) || (node->inputs->size < 1)) {
    return kTfLiteError;
  }
  const TfLiteEvalTensor* cms = tflite::micro::GetEvalInput(context, node, 0);
  if ((cms == nullptr) || (cms->data.data == nullptr)) {
    return kTfLiteError;
  }
  const void* cms_data = cms->data.data;
  const int   cms_size = static_cast<int>(tflite::EvalTensorBytes(cms));

  uint64_t base_addr[k_ra8_npu_region_count] = {};
  size_t   base_size[k_ra8_npu_region_count] = {};
  int      region_n                          = 0;
  if (!ethosu_collect_regions(context, node, base_addr, base_size, &region_n)) {
    return kTfLiteError;
  }

  struct ethosu_driver* drv = ethosu_reserve_driver();
  if (drv == nullptr) {
    return kTfLiteError;
  }
  const int rc = ethosu_invoke_v3(drv, cms_data, cms_size, base_addr, base_size, region_n, nullptr);
  ethosu_release_driver(drv);
  return (rc == 0) ? kTfLiteOk : kTfLiteError;
}

#endif /* RA8_HAS_NPU */

} // namespace

TFLMRegistration* Register_ETHOSU()
{
#ifdef RA8_HAS_NPU
  static TFLMRegistration s_registration = {
    /*init=*/nullptr,
    /*free=*/nullptr,
    /*prepare=*/nullptr,
    /*invoke=*/ethosu_eval,
    /*reset=*/nullptr,
    /*builtin_code=*/0,
    /*custom_name=*/k_ethosu_op_name,
  };
  return &s_registration;
#else
  /* No NPU on this device: the CPU-only TFLite path never dispatches ethos-u. */
  return nullptr;
#endif
}

const char* ethosu_custom_name()
{
  return k_ethosu_op_name;
}

} // namespace tflite

#ifdef RA8_HAS_NPU
extern "C" int ra8_ethosu_kernel_available(void)
{
  const TFLMRegistration* r = tflite::Register_ETHOSU();
  if (r == nullptr) {
    return 0;
  }
  if (r->invoke == nullptr) {
    return 0;
  }
  const char* name = tflite::ethosu_custom_name();
  if ((r->custom_name == nullptr) || (name == nullptr)) {
    return 0;
  }
  return (r->custom_name == name) ? 1 : 0;
}
#endif /* RA8_HAS_NPU */
