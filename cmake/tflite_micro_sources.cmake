# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# cmake/tflite_micro_sources.cmake
#
# The LEAN TFLite-micro subset, as paths relative to the pinned upstream
# tarball (build.zig.zon .tflite_micro). The tarball is the whole upstream
# tree, so this list is what keeps the build to the MicroInterpreter core and
# the reference kernels documented in docs/SOUP/tflite-micro.md. It is exactly
# the set of .cc files the tree vendored until RA8FW-385. kernels/ethosu.cc is
# listed because it is part of that set; both consumers filter it out and
# compile libs/ra8_hal/src/ra8_ethosu_kernel.cc instead.

set(RA8_TFLM_LEAN_SOURCES
    tensorflow/compiler/mlir/lite/core/api/error_reporter.cc
    tensorflow/compiler/mlir/lite/schema/schema_utils.cc
    tensorflow/lite/core/api/flatbuffer_conversions.cc
    tensorflow/lite/core/api/tensor_utils.cc
    tensorflow/lite/core/c/common.cc
    tensorflow/lite/kernels/internal/common.cc
    tensorflow/lite/kernels/internal/portable_tensor_utils.cc
    tensorflow/lite/kernels/internal/quantization_util.cc
    tensorflow/lite/kernels/internal/reference/portable_tensor_utils.cc
    tensorflow/lite/kernels/internal/runtime_shape.cc
    tensorflow/lite/kernels/internal/tensor_ctypes.cc
    tensorflow/lite/kernels/internal/tensor_utils.cc
    tensorflow/lite/kernels/kernel_util.cc
    tensorflow/lite/micro/arena_allocator/non_persistent_arena_buffer_allocator.cc
    tensorflow/lite/micro/arena_allocator/persistent_arena_buffer_allocator.cc
    tensorflow/lite/micro/arena_allocator/recording_single_arena_buffer_allocator.cc
    tensorflow/lite/micro/arena_allocator/single_arena_buffer_allocator.cc
    tensorflow/lite/micro/debug_log.cc
    tensorflow/lite/micro/flatbuffer_utils.cc
    tensorflow/lite/micro/hexdump.cc
    tensorflow/lite/micro/kernels/add.cc
    tensorflow/lite/micro/kernels/add_common.cc
    tensorflow/lite/micro/kernels/conv.cc
    tensorflow/lite/micro/kernels/conv_common.cc
    tensorflow/lite/micro/kernels/depthwise_conv.cc
    tensorflow/lite/micro/kernels/depthwise_conv_common.cc
    tensorflow/lite/micro/kernels/ethosu.cc
    tensorflow/lite/micro/kernels/fully_connected.cc
    tensorflow/lite/micro/kernels/fully_connected_common.cc
    tensorflow/lite/micro/kernels/kernel_util.cc
    tensorflow/lite/micro/kernels/micro_tensor_utils.cc
    tensorflow/lite/micro/kernels/mul.cc
    tensorflow/lite/micro/kernels/mul_common.cc
    tensorflow/lite/micro/kernels/pooling.cc
    tensorflow/lite/micro/kernels/pooling_common.cc
    tensorflow/lite/micro/kernels/reshape.cc
    tensorflow/lite/micro/kernels/reshape_common.cc
    tensorflow/lite/micro/kernels/softmax.cc
    tensorflow/lite/micro/kernels/softmax_common.cc
    tensorflow/lite/micro/memory_helpers.cc
    tensorflow/lite/micro/memory_planner/greedy_memory_planner.cc
    tensorflow/lite/micro/memory_planner/linear_memory_planner.cc
    tensorflow/lite/micro/memory_planner/non_persistent_buffer_planner_shim.cc
    tensorflow/lite/micro/micro_allocation_info.cc
    tensorflow/lite/micro/micro_allocator.cc
    tensorflow/lite/micro/micro_context.cc
    tensorflow/lite/micro/micro_interpreter.cc
    tensorflow/lite/micro/micro_interpreter_context.cc
    tensorflow/lite/micro/micro_interpreter_graph.cc
    tensorflow/lite/micro/micro_log.cc
    tensorflow/lite/micro/micro_op_resolver.cc
    tensorflow/lite/micro/micro_profiler.cc
    tensorflow/lite/micro/micro_resource_variable.cc
    tensorflow/lite/micro/micro_time.cc
    tensorflow/lite/micro/micro_utils.cc
    tensorflow/lite/micro/recording_micro_allocator.cc
    tensorflow/lite/micro/system_setup.cc
    tensorflow/lite/micro/tflite_bridge/flatbuffer_conversions_bridge.cc
    tensorflow/lite/micro/tflite_bridge/micro_error_reporter.cc
)
