#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Build the deterministic INT8 CONV_2D fixture Vela compiles for issue #227.

The distill path (`tools/vela/src/vela_gen.py distill`) needs a REAL Vela output,
and a real Vela output needs a real quantized .tflite. Rather than commit an
opaque binary model or take a TensorFlow dependency to author one, this script
writes the fixture byte for byte with the `flatbuffers` runtime and the TFLite
schema bindings the Vela package already vendors, so the whole chain is
reproducible from text in the tree:

    python3 tools/vela/models/conv_int8_fixture.py build/vela/conv_int8.tflite
    python3 tools/vela/src/vela_gen.py compile build/vela/conv_int8.tflite -o build/vela
    python3 tools/vela/src/vela_gen.py distill build/vela/conv_int8_vela.tflite \
            --symbol ra8_npu_model_conv_int8_vela --name conv_int8_vela \
            -o tools/vela/generated/ra8_npu_model_conv_int8_vela.h

The model is one 1x1 INT8 convolution, 4 input channels to 4 output channels
over an 8x8 spatial extent, per-axis quantized weights and an INT32 bias: the
smallest graph that still exercises a weight arena, a scratch arena, and an
input/output activation pair, which is what makes the distilled region layout
worth pinning. Every value is a fixed pattern, so two runs produce identical
bytes and the distilled golden is stable.

Needs the Vela install (for the vendored schema bindings); it is an opt-in step,
not part of the Vela-free gate.
"""

from __future__ import annotations

from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from collections.abc import Sequence

import struct
import sys
from pathlib import Path
from types import SimpleNamespace

TYPE_INT8 = 9
TYPE_INT32 = 2
BUILTIN_CONV_2D = 3
PADDING_VALID = 1
OPTIONS_CONV_2D = 1
SCHEMA_VERSION = 3
FILE_IDENTIFIER = b"TFL3"

OUT_CHANNELS = 4
IN_CHANNELS = 4
KERNEL_H = 1
KERNEL_W = 1
SPATIAL = 8
WEIGHT_MOD = 127
WEIGHT_BIAS = 63
INPUT_SCALE = 0.05
WEIGHT_SCALE = 0.01
OUTPUT_SCALE = 0.1
INPUT_ZERO_POINT = -1
BIAS_STEP = 100
BYTE_MASK = 0xFF
I32 = 4
I64 = 8


def _weight_bytes() -> bytes:
    """Deterministic INT8 weight pattern: ((i*5 + 1) % 127) - 63."""
    count = OUT_CHANNELS * KERNEL_H * KERNEL_W * IN_CHANNELS
    return bytes((((i * 5 + 1) % WEIGHT_MOD) - WEIGHT_BIAS) & BYTE_MASK for i in range(count))


def _bias_bytes() -> bytes:
    """Deterministic INT32 bias pattern: 100 * (i + 1), little-endian."""
    return b"".join(struct.pack("<i", BIAS_STEP * (i + 1)) for i in range(OUT_CHANNELS))


def _vec(builder, values: list[int], width: int, prepend: str) -> int:  # noqa: ANN001
    """Write a scalar flatbuffer vector of `values` using `prepend`."""
    builder.StartVector(width, len(values), width)
    for value in reversed(values):
        getattr(builder, prepend)(value)
    return builder.EndVector()


def _quantization(
    builder,  # noqa: ANN001  (flatbuffers.Builder; the package ships no stubs)
    schema,  # noqa: ANN001  (the SimpleNamespace built by _schema)
    scales: list[float],
    zero_points: list[int],
) -> int:
    """Emit a QuantizationParameters table (per-axis when several scales)."""
    scale_vec = _vec(builder, scales, I32, "PrependFloat32")
    zp_vec = _vec(builder, zero_points, I64, "PrependInt64")
    schema.QuantizationParameters.Start(builder)
    schema.QuantizationParameters.AddScale(builder, scale_vec)
    schema.QuantizationParameters.AddZeroPoint(builder, zp_vec)
    schema.QuantizationParameters.AddQuantizedDimension(builder, 0)
    return schema.QuantizationParameters.End(builder)


def _tensor(  # noqa: PLR0913
    builder,  # noqa: ANN001  (flatbuffers.Builder; the package ships no stubs)
    schema,  # noqa: ANN001  (the SimpleNamespace built by _schema)
    shape: Sequence[int],
    ttype: int,
    buffer_index: int,
    name: str,
    quant: int,
) -> int:
    """Emit one Tensor table."""
    name_off = builder.CreateString(name)
    shape_off = _vec(builder, list(shape), I32, "PrependInt32")
    schema.Tensor.Start(builder)
    schema.Tensor.AddShape(builder, shape_off)
    schema.Tensor.AddType(builder, ttype)
    schema.Tensor.AddBuffer(builder, buffer_index)
    schema.Tensor.AddName(builder, name_off)
    schema.Tensor.AddQuantization(builder, quant)
    schema.Tensor.AddHasRank(builder, True)  # noqa: FBT003 -- schema flag, not a switch
    return schema.Tensor.End(builder)


def _offsets(builder, offsets: list[int]) -> int:  # noqa: ANN001
    """Write a flatbuffer vector of table offsets."""
    builder.StartVector(I32, len(offsets), I32)
    for offset in reversed(offsets):
        builder.PrependUOffsetTRelative(offset)
    return builder.EndVector()


def _schema():  # noqa: ANN202  (flatbuffers module + SimpleNamespace, unstubbed)
    """Import the TFLite schema bindings vendored by the Vela package.

    Raises:
        RuntimeError: ethos-u-vela (or flatbuffers) is not installed.
    """
    try:
        import flatbuffers  # noqa: PLC0415 -- optional dep
        from ethosu.vela.tflite import (  # noqa: PLC0415 -- optional dep
            Buffer,
            Conv2DOptions,
            Model,
            Operator,
            OperatorCode,
            QuantizationParameters,
            SubGraph,
            Tensor,
        )
    except ImportError as exc:  # pragma: no cover -- depends on the environment
        msg = "ethos-u-vela is not installed, so the fixture cannot be built here"
        raise RuntimeError(msg) from exc
    schema = SimpleNamespace(
        Buffer=Buffer,
        Conv2DOptions=Conv2DOptions,
        Model=Model,
        Operator=Operator,
        OperatorCode=OperatorCode,
        QuantizationParameters=QuantizationParameters,
        SubGraph=SubGraph,
        Tensor=Tensor,
    )
    return flatbuffers, schema


def _tensors(builder, schema) -> int:  # noqa: ANN001  (unstubbed flatbuffers objects)
    """Emit the four tensors and return the offset of their vector."""
    q_in = _quantization(builder, schema, [INPUT_SCALE], [INPUT_ZERO_POINT])
    q_w = _quantization(builder, schema, [WEIGHT_SCALE] * OUT_CHANNELS, [0] * OUT_CHANNELS)
    q_b = _quantization(
        builder,
        schema,
        [INPUT_SCALE * WEIGHT_SCALE] * OUT_CHANNELS,
        [0] * OUT_CHANNELS,
    )
    q_out = _quantization(builder, schema, [OUTPUT_SCALE], [0])
    t_in = _tensor(
        builder, schema, [1, SPATIAL, SPATIAL, IN_CHANNELS], TYPE_INT8, 1, "input", q_in
    )
    t_w = _tensor(
        builder,
        schema,
        [OUT_CHANNELS, KERNEL_H, KERNEL_W, IN_CHANNELS],
        TYPE_INT8,
        2,
        "conv_w",
        q_w,
    )
    t_b = _tensor(builder, schema, [OUT_CHANNELS], TYPE_INT32, 3, "conv_b", q_b)
    t_out = _tensor(
        builder, schema, [1, SPATIAL, SPATIAL, OUT_CHANNELS], TYPE_INT8, 4, "output", q_out
    )
    return _offsets(builder, [t_in, t_w, t_b, t_out])


def _operators(builder, schema) -> int:  # noqa: ANN001  (unstubbed flatbuffers objects)
    """Emit the single CONV_2D operator and return its vector offset."""
    op_inputs = _vec(builder, [0, 1, 2], I32, "PrependInt32")
    op_outputs = _vec(builder, [3], I32, "PrependInt32")
    schema.Conv2DOptions.Start(builder)
    schema.Conv2DOptions.AddPadding(builder, PADDING_VALID)
    schema.Conv2DOptions.AddStrideW(builder, 1)
    schema.Conv2DOptions.AddStrideH(builder, 1)
    schema.Conv2DOptions.AddFusedActivationFunction(builder, 0)
    schema.Conv2DOptions.AddDilationWFactor(builder, 1)
    schema.Conv2DOptions.AddDilationHFactor(builder, 1)
    conv_options = schema.Conv2DOptions.End(builder)
    schema.Operator.Start(builder)
    schema.Operator.AddOpcodeIndex(builder, 0)
    schema.Operator.AddInputs(builder, op_inputs)
    schema.Operator.AddOutputs(builder, op_outputs)
    schema.Operator.AddBuiltinOptionsType(builder, OPTIONS_CONV_2D)
    schema.Operator.AddBuiltinOptions(builder, conv_options)
    return _offsets(builder, [schema.Operator.End(builder)])


def _subgraph(builder, schema, tensors: int, operators: int) -> int:  # noqa: ANN001
    """Emit the one subgraph and return its vector offset."""
    sg_inputs = _vec(builder, [0], I32, "PrependInt32")
    sg_outputs = _vec(builder, [3], I32, "PrependInt32")
    sg_name = builder.CreateString("main")
    schema.SubGraph.Start(builder)
    schema.SubGraph.AddTensors(builder, tensors)
    schema.SubGraph.AddInputs(builder, sg_inputs)
    schema.SubGraph.AddOutputs(builder, sg_outputs)
    schema.SubGraph.AddOperators(builder, operators)
    schema.SubGraph.AddName(builder, sg_name)
    return _offsets(builder, [schema.SubGraph.End(builder)])


def _opcodes(builder, schema) -> int:  # noqa: ANN001  (unstubbed flatbuffers objects)
    """Emit the single CONV_2D operator code and return its vector offset."""
    schema.OperatorCode.Start(builder)
    schema.OperatorCode.AddDeprecatedBuiltinCode(builder, BUILTIN_CONV_2D)
    schema.OperatorCode.AddBuiltinCode(builder, BUILTIN_CONV_2D)
    schema.OperatorCode.AddVersion(builder, SCHEMA_VERSION)
    return _offsets(builder, [schema.OperatorCode.End(builder)])


def _buffers(builder, schema, blobs: Sequence[bytes]) -> int:  # noqa: ANN001
    """Emit one Buffer per entry of @p blobs and return the vector offset."""
    buffer_offsets = []
    for data in blobs:
        if data:
            data_off = _vec(builder, list(data), 1, "PrependByte")
            schema.Buffer.Start(builder)
            schema.Buffer.AddData(builder, data_off)
        else:
            schema.Buffer.Start(builder)
        buffer_offsets.append(schema.Buffer.End(builder))
    return _offsets(builder, buffer_offsets)


def build() -> bytes:
    """Return the fixture .tflite as bytes."""
    flatbuffers, schema = _schema()
    builder = flatbuffers.Builder(4096)

    tensors = _tensors(builder, schema)
    operators = _operators(builder, schema)
    subgraphs = _subgraph(builder, schema, tensors, operators)
    opcodes = _opcodes(builder, schema)
    buffers = _buffers(builder, schema, (b"", b"", _weight_bytes(), _bias_bytes(), b""))

    description = builder.CreateString("ra8 npu int8 conv fixture (#227)")
    schema.Model.Start(builder)
    schema.Model.AddVersion(builder, SCHEMA_VERSION)
    schema.Model.AddOperatorCodes(builder, opcodes)
    schema.Model.AddSubgraphs(builder, subgraphs)
    schema.Model.AddDescription(builder, description)
    schema.Model.AddBuffers(builder, buffers)
    builder.Finish(schema.Model.End(builder), file_identifier=FILE_IDENTIFIER)
    return bytes(builder.Output())


def main(argv: list[str]) -> int:
    """Write the fixture to argv[0] (default build/vela/conv_int8.tflite)."""
    out = Path(argv[0]) if argv else Path("build/vela/conv_int8.tflite")
    out.parent.mkdir(parents=True, exist_ok=True)
    data = build()
    out.write_bytes(data)
    print(f"conv_int8_fixture: wrote {out} ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
