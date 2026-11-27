"""Offline ONNX-to-Keras graph translator and INT8 TFLite exporter."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from model_registry import ModelPipelineSpec

_NDIM_4D = 4
_NDIM_2D = 2
_RGB_CH = 3
_CONV_BIAS_INPUTS = 3
_CLIP_INPUT_MAX = 2
_SLICE_AXES_INPUTS = 3
_CLIP_MIN_DEFAULT = -1e9
_CLIP_MAX_DEFAULT = 1e9


@dataclass
class _BuildState:
    """Mutable SSA translation state mapping ONNX names to Keras tensors."""

    in_name: str
    tensors: dict[str, object] = field(default_factory=dict)
    is_nhwc: dict[str, bool] = field(default_factory=dict)
    consts: dict[str, object] = field(default_factory=dict)
    pvals: dict[str, object] = field(default_factory=dict)
    shapes: dict[str, tuple[int, ...]] = field(default_factory=dict)
    static: set[str] = field(default_factory=set)
    conv1_rgb_flipped: bool = False


def _onnx_deps() -> tuple[object, tuple[object, object, object], object, object, object]:
    """Lazily import heavy ONNX/TF dependencies only during conversion."""
    import numpy as np  # noqa: PLC0415
    import onnx  # noqa: PLC0415
    import onnxruntime as ort  # noqa: PLC0415
    import tensorflow as tf  # noqa: PLC0415
    import tf_keras as keras  # noqa: PLC0415
    from onnx import numpy_helper  # noqa: PLC0415

    return np, (onnx, numpy_helper, ort), tf, keras, np.float32


def _probe_onnx_graph(
    onnx_path: Path,
    height: int,
    width: int,
) -> tuple[object, str, dict[str, object], dict[str, object], dict[str, tuple[int, ...]]]:
    """Probe static constants and fixed-shape tensor dimensions with ORT."""
    np, (onnx, numpy_helper, ort), _tf, _keras, _f32 = _onnx_deps()
    model = onnx.load(str(onnx_path))
    consts: dict[str, object] = {
        init.name: numpy_helper.to_array(init) for init in model.graph.initializer
    }
    for node in model.graph.node:
        if node.op_type == "Constant":
            for attr in node.attribute:
                if attr.name == "value":
                    consts[node.output[0]] = numpy_helper.to_array(attr.t)

    probe = onnx.ModelProto()
    probe.CopyFrom(model)
    seen = {o.name for o in probe.graph.output}
    for node in probe.graph.node:
        if node.op_type == "Constant":
            continue
        for out_name in node.output:
            if out_name and out_name not in seen:
                probe.graph.output.append(onnx.helper.ValueInfoProto(name=out_name))
                seen.add(out_name)

    so = ort.SessionOptions()
    so.intra_op_num_threads = 2
    so.inter_op_num_threads = 1
    sess = ort.InferenceSession(
        probe.SerializeToString(),
        sess_options=so,
        providers=["CPUExecutionProvider"],
    )
    in_name = model.graph.input[0].name
    dummy = np.zeros((1, _RGB_CH, height, width), dtype=np.float32)
    outputs = sess.run(None, {in_name: dummy})
    pvals = {o.name: val for o, val in zip(probe.graph.output, outputs, strict=True)}
    shapes: dict[str, tuple[int, ...]] = {name: val.shape for name, val in pvals.items()}
    shapes[in_name] = (1, _RGB_CH, height, width)
    for name, val in consts.items():
        shapes[name] = val.shape  # type: ignore[attr-defined]
    return model, in_name, consts, pvals, shapes


def _as_nhwc(state: _BuildState, name: str, tf: object) -> object:
    """Return 4D tensor in NHWC layout, transposing if needed."""
    tensor = state.tensors[name]
    if len(tensor.shape) == _NDIM_4D and not state.is_nhwc.get(name, False):  # type: ignore[attr-defined]
        return tf.transpose(tensor, (0, 2, 3, 1))  # type: ignore[attr-defined]
    return tensor


def _as_nchw(state: _BuildState, name: str, tf: object) -> object:
    """Return 4D tensor in NCHW layout, transposing if needed."""
    tensor = state.tensors[name]
    if len(tensor.shape) == _NDIM_4D and state.is_nhwc.get(name, False):  # type: ignore[attr-defined]
        return tf.transpose(tensor, (0, 3, 1, 2))  # type: ignore[attr-defined]
    return tensor


def _bcast_const(arr: object, *, nhwc4: bool, np: object, tf: object) -> object:
    """Broadcast static ONNX constant into compatible TF tensor layout."""
    if nhwc4 and arr.ndim == _NDIM_4D:  # type: ignore[attr-defined]
        return tf.constant(np.transpose(arr, (0, 2, 3, 1)), dtype=tf.float32)  # type: ignore[attr-defined]
    return tf.constant(arr, dtype=tf.float32)  # type: ignore[attr-defined]


def _node_attrs(node: object, onnx: object) -> dict[str, object]:
    """Decode ONNX node attributes into a plain Python dictionary."""
    return {
        attr.name: onnx.helper.get_attribute_value(attr)  # type: ignore[attr-defined]
        for attr in node.attribute  # type: ignore[attr-defined]
    }


def _emit_conv(
    state: _BuildState,
    idx: int,
    node: object,
    at: dict[str, object],
    *,
    deps: tuple[object, object, object, object],
) -> None:
    """Emit standard, depthwise, or grouped Conv2D with RGB fold on first layer."""
    np, _onx, tf, keras = deps
    x = _as_nhwc(state, node.input[0], tf)  # type: ignore[attr-defined]
    w_arr = state.consts[node.input[1]].copy()  # type: ignore[attr-defined]
    has_b = len(node.input) >= _CONV_BIAS_INPUTS  # type: ignore[attr-defined]
    b_arr = state.consts[node.input[2]].copy() if has_b else None  # type: ignore[attr-defined]
    if (
        (not state.conv1_rgb_flipped)
        and node.input[0] == state.in_name  # type: ignore[attr-defined]
        and w_arr.shape[1] == _RGB_CH
    ):
        w_arr = w_arr[:, ::-1, :, :].copy()
        state.conv1_rgb_flipped = True
    pads = list(at.get("pads", [0, 0, 0, 0]))  # type: ignore[call-overload]
    if any(p > 0 for p in pads):
        x = tf.pad(x, [[0, 0], [pads[0], pads[2]], [pads[1], pads[3]], [0, 0]])  # type: ignore[attr-defined]
    strides = tuple(at.get("strides", [1, 1]))  # type: ignore[arg-type]
    dilations = tuple(at.get("dilations", [1, 1]))  # type: ignore[arg-type]
    groups = int(at.get("group", 1))  # type: ignore[arg-type]
    in_ch = int(x.shape[-1])  # type: ignore[attr-defined]
    ksize = (int(w_arr.shape[2]), int(w_arr.shape[3]))
    if groups == 1:
        layer = keras.layers.Conv2D(  # type: ignore[attr-defined]
            int(w_arr.shape[0]),
            ksize,
            strides=strides,
            padding="valid",
            dilation_rate=dilations,
            use_bias=(b_arr is not None),
            name=f"conv_{idx}",
        )
        w_tf = np.transpose(w_arr, (2, 3, 1, 0))  # type: ignore[attr-defined]
    elif groups == in_ch and int(w_arr.shape[0]) == in_ch and int(w_arr.shape[1]) == 1:
        layer = keras.layers.DepthwiseConv2D(  # type: ignore[attr-defined]
            ksize,
            strides=strides,
            padding="valid",
            dilation_rate=dilations,
            use_bias=(b_arr is not None),
            name=f"dwconv_{idx}",
        )
        w_tf = np.transpose(w_arr, (2, 3, 0, 1))  # type: ignore[attr-defined]
    else:
        layer = keras.layers.Conv2D(  # type: ignore[attr-defined]
            int(w_arr.shape[0]),
            ksize,
            strides=strides,
            padding="valid",
            dilation_rate=dilations,
            groups=groups,
            use_bias=(b_arr is not None),
            name=f"gconv_{idx}",
        )
        w_tf = np.transpose(w_arr, (2, 3, 1, 0))  # type: ignore[attr-defined]
    y = layer(x)
    layer.set_weights([w_tf, b_arr] if b_arr is not None else [w_tf])
    state.tensors[node.output[0]] = y  # type: ignore[attr-defined]
    state.is_nhwc[node.output[0]] = True  # type: ignore[attr-defined]


def _emit_spatial_op(
    state: _BuildState,
    idx: int,
    node: object,
    at: dict[str, object],
    *,
    deps: tuple[object, object, object, object],
) -> None:
    """Emit ConvTranspose, BatchNorm, Pool, or Resize ops."""
    np, _onx, tf, keras = deps
    op, out_name = node.op_type, node.output[0]  # type: ignore[attr-defined]
    x = _as_nhwc(state, node.input[0], tf)  # type: ignore[attr-defined]
    if op == "ConvTranspose":
        w_arr = state.consts[node.input[1]].copy()  # type: ignore[attr-defined]
        has_b = len(node.input) >= _CONV_BIAS_INPUTS  # type: ignore[attr-defined]
        b_arr = state.consts[node.input[2]].copy() if has_b else None  # type: ignore[attr-defined]
        layer = keras.layers.Conv2DTranspose(  # type: ignore[attr-defined]
            int(w_arr.shape[1]),
            (int(w_arr.shape[2]), int(w_arr.shape[3])),
            strides=tuple(at.get("strides", [1, 1])),  # type: ignore[arg-type]
            padding="valid",
            use_bias=(b_arr is not None),
            name=f"deconv_{idx}",
        )
        y = layer(x)
        w_tf = np.transpose(w_arr, (2, 3, 1, 0))  # type: ignore[attr-defined]
        layer.set_weights([w_tf, b_arr] if b_arr is not None else [w_tf])
        state.tensors[out_name] = y
    elif op == "BatchNormalization":
        sc, bs, mn, vr = (state.consts[k] for k in node.input[1:5])  # type: ignore[attr-defined]
        eps = float(at.get("epsilon", 1e-5))  # type: ignore[arg-type]
        layer = keras.layers.BatchNormalization(epsilon=eps, name=f"bn_{idx}")  # type: ignore[attr-defined]
        y = layer(x)
        layer.set_weights([sc, bs, mn, vr])
        state.tensors[out_name] = y
    elif op == "Resize":
        tgt = state.shapes[out_name]
        raw_mode = at.get("mode", b"nearest")
        mode = raw_mode.decode() if isinstance(raw_mode, bytes) else str(raw_mode)
        method = "nearest" if mode == "nearest" else "bilinear"
        state.tensors[out_name] = tf.image.resize(x, [tgt[2], tgt[3]], method=method)  # type: ignore[attr-defined]
    elif op == "GlobalAveragePool":
        state.tensors[out_name] = tf.reduce_mean(x, axis=[1, 2], keepdims=True)  # type: ignore[attr-defined]
    else:
        pads = list(at.get("pads", [0, 0, 0, 0]))  # type: ignore[call-overload]
        if any(p > 0 for p in pads):
            x = tf.pad(x, [[0, 0], [pads[0], pads[2]], [pads[1], pads[3]], [0, 0]])  # type: ignore[attr-defined]
        ksize = tuple(at["kernel_shape"])  # type: ignore[arg-type]
        strides = tuple(at.get("strides", [1, 1]))  # type: ignore[arg-type]
        state.tensors[out_name] = tf.nn.avg_pool2d(  # type: ignore[attr-defined]
            x,
            ksize=ksize,
            strides=strides,
            padding="VALID",
        )
    state.is_nhwc[out_name] = True


def _emit_elem_or_shape(
    state: _BuildState,
    node: object,
    at: dict[str, object],
    *,
    np: object,
    tf: object,
) -> None:
    """Emit activation, elementwise binary, or tensor layout ops."""
    op, out_name = node.op_type, node.output[0]  # type: ignore[attr-defined]
    ins = node.input  # type: ignore[attr-defined]
    if op == "Relu":
        state.tensors[out_name] = tf.nn.relu(state.tensors[ins[0]])  # type: ignore[attr-defined]
        state.is_nhwc[out_name] = state.is_nhwc.get(ins[0], False)
    elif op == "Sigmoid":
        state.tensors[out_name] = tf.nn.sigmoid(state.tensors[ins[0]])  # type: ignore[attr-defined]
        state.is_nhwc[out_name] = state.is_nhwc.get(ins[0], False)
    elif op == "HardSigmoid":
        alpha = float(at.get("alpha", 0.2))  # type: ignore[arg-type]
        beta = float(at.get("beta", 0.5))  # type: ignore[arg-type]
        state.tensors[out_name] = tf.clip_by_value(  # type: ignore[attr-defined]
            state.tensors[ins[0]] * alpha + beta,
            0.0,
            1.0,
        )
        state.is_nhwc[out_name] = state.is_nhwc.get(ins[0], False)
    elif op == "HardSwish":
        x = state.tensors[ins[0]]
        state.tensors[out_name] = x * (tf.nn.relu6(x + 3.0) / 6.0)  # type: ignore[attr-defined]
        state.is_nhwc[out_name] = state.is_nhwc.get(ins[0], False)
    elif op == "Clip":
        has_min = len(ins) > 1 and ins[1] in state.consts
        has_max = len(ins) > _CLIP_INPUT_MAX and ins[2] in state.consts
        cmin = float(state.consts[ins[1]]) if has_min else float(at.get("min", _CLIP_MIN_DEFAULT))  # type: ignore[arg-type]
        cmax = float(state.consts[ins[2]]) if has_max else float(at.get("max", _CLIP_MAX_DEFAULT))  # type: ignore[arg-type]
        state.tensors[out_name] = tf.clip_by_value(state.tensors[ins[0]], cmin, cmax)  # type: ignore[attr-defined]
        state.is_nhwc[out_name] = state.is_nhwc.get(ins[0], False)
    elif op in ("Add", "Mul", "Div", "Sub", "Pow"):
        _emit_binary(state, node, np=np, tf=tf)
    else:
        _emit_tensor_op(state, node, at, np=np, tf=tf)


def _emit_binary(state: _BuildState, node: object, *, np: object, tf: object) -> None:
    """Emit elementwise Add, Mul, Div, Sub, or Pow with NHWC-aware broadcasting."""
    op, out_name = node.op_type, node.output[0]  # type: ignore[attr-defined]
    a_name, b_name = node.input[0], node.input[1]  # type: ignore[attr-defined]
    a_dyn, b_dyn = a_name in state.tensors, b_name in state.tensors
    if a_dyn and b_dyn:
        if state.is_nhwc.get(a_name, False) != state.is_nhwc.get(b_name, False):
            ta = _as_nhwc(state, a_name, tf)
            tb = _as_nhwc(state, b_name, tf)
            rnh = len(ta.shape) == _NDIM_4D  # type: ignore[attr-defined]
        else:
            ta, tb = state.tensors[a_name], state.tensors[b_name]
            rnh = state.is_nhwc.get(a_name, False)
    elif a_dyn:
        ta = state.tensors[a_name]
        rnh = state.is_nhwc.get(a_name, False)
        is_nhwc4 = rnh and len(ta.shape) == _NDIM_4D  # type: ignore[attr-defined]
        tb = _bcast_const(state.consts[b_name], nhwc4=is_nhwc4, np=np, tf=tf)
    else:
        tb = state.tensors[b_name]
        rnh = state.is_nhwc.get(b_name, False)
        is_nhwc4 = rnh and len(tb.shape) == _NDIM_4D  # type: ignore[attr-defined]
        ta = _bcast_const(state.consts[a_name], nhwc4=is_nhwc4, np=np, tf=tf)
    ops = {
        "Add": lambda u, v: u + v,
        "Mul": lambda u, v: u * v,
        "Div": lambda u, v: u / v,
        "Sub": lambda u, v: u - v,
        "Pow": tf.math.pow,  # type: ignore[attr-defined]
    }
    state.tensors[out_name] = ops[op](ta, tb)  # type: ignore[operator]
    state.is_nhwc[out_name] = rnh


def _emit_concat(
    state: _BuildState,
    node: object,
    at: dict[str, object],
    *,
    np: object,
    tf: object,
) -> None:
    """Emit Concat preserving NHWC when concatenating 4D feature maps on channels."""
    out_name = node.output[0]  # type: ignore[attr-defined]
    axis = int(at.get("axis", 0))  # type: ignore[arg-type]
    ins = list(node.input)  # type: ignore[attr-defined]
    all_4d = all(len(state.tensors[i].shape) == _NDIM_4D for i in ins if i in state.tensors)  # type: ignore[attr-defined]
    if all_4d and axis == 1:
        items = [
            _as_nhwc(state, i, tf)
            if i in state.tensors
            else _bcast_const(state.consts[i], nhwc4=True, np=np, tf=tf)
            for i in ins
        ]
        state.tensors[out_name] = tf.concat(items, axis=-1)  # type: ignore[attr-defined]
        state.is_nhwc[out_name] = True
        return
    items = [
        _as_nchw(state, i, tf)
        if i in state.tensors
        else tf.constant(state.consts[i], dtype=tf.float32)  # type: ignore[attr-defined]
        for i in ins
    ]
    state.tensors[out_name] = tf.concat(items, axis=axis)  # type: ignore[attr-defined]
    state.is_nhwc[out_name] = False


def _emit_tensor_op(
    state: _BuildState,
    node: object,
    at: dict[str, object],
    *,
    np: object,
    tf: object,
) -> None:
    """Emit Concat, Reshape, Transpose, Slice, Squeeze, Unsqueeze, Math, MatMul."""
    op, out_name = node.op_type, node.output[0]  # type: ignore[attr-defined]
    ins = node.input  # type: ignore[attr-defined]
    if op == "Concat":
        _emit_concat(state, node, at, np=np, tf=tf)
    elif op == "Reshape":
        if ins[0] in state.consts:
            state.consts[out_name] = state.consts[ins[0]].reshape(state.shapes[out_name])  # type: ignore[attr-defined]
            state.static.add(out_name)
        else:
            x = _as_nchw(state, ins[0], tf)
            state.tensors[out_name] = tf.reshape(x, state.shapes[out_name])  # type: ignore[attr-defined]
            state.is_nhwc[out_name] = False
    elif op == "Transpose":
        x = _as_nchw(state, ins[0], tf)
        state.tensors[out_name] = tf.transpose(x, perm=at["perm"])  # type: ignore[attr-defined]
        state.is_nhwc[out_name] = False
    elif op == "Slice":
        _emit_slice(state, node, tf=tf)
    elif op in ("Squeeze", "Unsqueeze"):
        _emit_squeeze_unsqueeze(state, node, at, tf=tf)
    elif op in ("ReduceMean", "Sqrt", "MatMul", "Softmax", "Identity"):
        _emit_math_tail(state, node, at, tf=tf)
    else:
        msg = f"unsupported ONNX op {op!r} at output {out_name!r}"
        raise NotImplementedError(msg)


def _emit_slice(state: _BuildState, node: object, *, tf: object) -> None:
    """Emit static-bounds ONNX Slice in TensorFlow."""
    out_name = node.output[0]  # type: ignore[attr-defined]
    ins = node.input  # type: ignore[attr-defined]
    x = _as_nchw(state, ins[0], tf)
    s_raw = state.pvals[ins[1]] if ins[1] in state.pvals else state.consts[ins[1]]
    e_raw = state.pvals[ins[2]] if ins[2] in state.pvals else state.consts[ins[2]]
    starts = s_raw.astype(int).tolist()  # type: ignore[attr-defined]
    ends = e_raw.astype(int).tolist()  # type: ignore[attr-defined]
    if len(ins) > _SLICE_AXES_INPUTS:
        a_raw = state.pvals[ins[3]] if ins[3] in state.pvals else state.consts[ins[3]]
        axes = a_raw.astype(int).tolist()  # type: ignore[attr-defined]
    else:
        axes = list(range(len(starts)))
    in_shape = state.shapes[ins[0]]
    rank = len(in_shape)
    begin = [0] * rank
    size = list(in_shape)
    for st, en, ax in zip(starts, ends, axes, strict=True):
        ax_norm = ax if ax >= 0 else rank + ax
        st_norm = st if st >= 0 else in_shape[ax_norm] + st
        en_norm = min(in_shape[ax_norm], en if en >= 0 else in_shape[ax_norm] + en)
        begin[ax_norm] = st_norm
        size[ax_norm] = en_norm - st_norm
    state.tensors[out_name] = tf.slice(x, begin, size)  # type: ignore[attr-defined]
    state.is_nhwc[out_name] = False


def _emit_squeeze_unsqueeze(
    state: _BuildState,
    node: object,
    at: dict[str, object],
    *,
    tf: object,
) -> None:
    """Emit ONNX Squeeze or Unsqueeze without dropping NCHW rank tracking."""
    op, out_name = node.op_type, node.output[0]  # type: ignore[attr-defined]
    ins = node.input  # type: ignore[attr-defined]
    axes = at.get("axes")
    if axes is None and len(ins) > 1:
        ax_arr = state.pvals[ins[1]] if ins[1] in state.pvals else state.consts[ins[1]]
        axes = ax_arr.astype(int).tolist()  # type: ignore[attr-defined]
    x = _as_nchw(state, ins[0], tf)
    ax_list = [int(a) for a in (axes or [])]  # type: ignore[attr-defined]
    if op == "Squeeze":
        for ax in sorted(ax_list, reverse=True):
            x = tf.squeeze(x, axis=ax)  # type: ignore[attr-defined]
    else:
        for ax in sorted(ax_list):
            x = tf.expand_dims(x, axis=ax)  # type: ignore[attr-defined]
    state.tensors[out_name] = x
    state.is_nhwc[out_name] = False


def _emit_math_tail(state: _BuildState, node: object, at: dict[str, object], *, tf: object) -> None:
    """Emit ReduceMean, Sqrt, MatMul, Softmax, and Identity nodes."""
    op, out_name = node.op_type, node.output[0]  # type: ignore[attr-defined]
    ins = node.input  # type: ignore[attr-defined]
    if op == "ReduceMean":
        x = (
            _as_nchw(state, ins[0], tf)
            if state.is_nhwc.get(ins[0], False)
            else state.tensors[ins[0]]
        )
        keepdims = bool(at.get("keepdims", 1))
        state.tensors[out_name] = tf.reduce_mean(x, axis=at["axes"], keepdims=keepdims)  # type: ignore[attr-defined]
        state.is_nhwc[out_name] = False
    elif op == "Sqrt":
        state.tensors[out_name] = tf.sqrt(state.tensors[ins[0]])  # type: ignore[attr-defined]
        state.is_nhwc[out_name] = state.is_nhwc.get(ins[0], False)
    elif op == "MatMul":
        ta = (
            state.tensors[ins[0]]
            if ins[0] in state.tensors
            else tf.constant(state.consts[ins[0]], tf.float32)  # type: ignore[attr-defined]
        )
        tb = (
            state.tensors[ins[1]]
            if ins[1] in state.tensors
            else tf.constant(state.consts[ins[1]], tf.float32)  # type: ignore[attr-defined]
        )
        state.tensors[out_name] = tf.matmul(ta, tb)  # type: ignore[attr-defined]
        state.is_nhwc[out_name] = False
    elif op == "Softmax":
        ax = int(at.get("axis", -1))  # type: ignore[arg-type]
        state.tensors[out_name] = tf.nn.softmax(state.tensors[ins[0]], axis=ax)  # type: ignore[attr-defined]
        state.is_nhwc[out_name] = False
    elif op == "Identity":
        state.tensors[out_name] = state.tensors[ins[0]]
        state.is_nhwc[out_name] = state.is_nhwc.get(ins[0], False)


def convert_onnx_to_keras(
    onnx_path: Path,
    *,
    height: int,
    width: int,
    drop_softmax: bool,
) -> object:
    """Translate fixed-input NCHW ONNX detector or recognizer to NHWC Keras.

    Args:
        onnx_path: Source ONNX model expecting BGR NCHW `(1, 3, H, W)` input.
        height: Fixed input tensor height.
        width: Fixed input tensor width.
        drop_softmax: Omit final `Softmax` node when emitting raw CTC logits.

    Returns:
        Constructed Keras model expecting RGB NHWC `(1, H, W, 3)` input.
    """
    np, (onnx, _nh, _ort), tf, keras, _f32 = _onnx_deps()
    model, in_name, consts, pvals, shapes = _probe_onnx_graph(onnx_path, height, width)
    inp = keras.Input(shape=(height, width, _RGB_CH), batch_size=1, name="x_rgb")  # type: ignore[attr-defined]
    state = _BuildState(
        in_name=in_name,
        tensors={in_name: inp},
        is_nhwc={in_name: True},
        consts=consts,
        pvals=pvals,
        shapes=shapes,
        static=set(consts.keys()),
    )

    nodes = [n for n in model.graph.node if n.op_type != "Constant"]  # type: ignore[attr-defined]
    for node in nodes:
        if all(i in state.static or i == "" for i in node.input) or node.op_type == "Shape":
            state.static.update(node.output)
    for out_name in state.static:
        if out_name not in state.consts and out_name in state.pvals:
            state.consts[out_name] = state.pvals[out_name].copy()  # type: ignore[attr-defined]

    deps = (np, onnx, tf, keras)
    final_out = model.graph.output[0].name  # type: ignore[attr-defined]
    spatial_ops = (
        "ConvTranspose",
        "BatchNormalization",
        "Resize",
        "AveragePool",
        "GlobalAveragePool",
    )
    for idx, node in enumerate(nodes):
        if all(o in state.static for o in node.output):
            continue
        if drop_softmax and node.op_type == "Softmax" and node.output[0] == final_out:
            state.tensors[final_out] = state.tensors[node.input[0]]
            continue
        at = _node_attrs(node, onnx)
        if node.op_type == "Conv":
            _emit_conv(state, idx, node, at, deps=deps)
        elif node.op_type in spatial_ops:
            _emit_spatial_op(state, idx, node, at, deps=deps)
        else:
            _emit_elem_or_shape(state, node, at, np=np, tf=tf)

    out_tensor = state.tensors[final_out]
    if len(out_tensor.shape) == _NDIM_4D and not state.is_nhwc.get(final_out, False):  # type: ignore[attr-defined]
        out_tensor = tf.transpose(out_tensor, (0, 2, 3, 1))  # type: ignore[attr-defined]
    return keras.Model(inp, out_tensor)  # type: ignore[attr-defined]


def export_int8_tflite(
    keras_model: object,
    cal_images_rgb: list[object],
    out_tflite_path: Path,
) -> tuple[bytes, dict[str, object]]:
    """Quantize a converted Keras model to full-integer INT8 TFLite.

    Args:
        keras_model: Converted RGB-NHWC Keras model.
        cal_images_rgb: Representative normalized float32 RGB images `(H, W, 3)`.
        out_tflite_path: Destination `.tflite` output path.

    Returns:
        Serialized `.tflite` bytes and quantized tensor scale/zero-point metadata.
    """
    np, _onx, tf, _keras, _f32 = _onnx_deps()
    from ai_edge_litert.interpreter import Interpreter  # noqa: PLC0415

    converter = tf.lite.TFLiteConverter.from_keras_model(keras_model)  # type: ignore[attr-defined]
    converter.optimizations = [tf.lite.Optimize.DEFAULT]  # type: ignore[attr-defined]
    converter.representative_dataset = lambda: (
        [img[None, ...].astype(np.float32)]  # type: ignore[index,attr-defined]
        for img in cal_images_rgb
    )
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]  # type: ignore[attr-defined]
    converter.inference_input_type = tf.int8  # type: ignore[attr-defined]
    converter.inference_output_type = tf.int8  # type: ignore[attr-defined]
    tfl_bytes = converter.convert()
    out_tflite_path.parent.mkdir(parents=True, exist_ok=True)
    out_tflite_path.write_bytes(tfl_bytes)

    interp = Interpreter(model_path=str(out_tflite_path))
    interp.allocate_tensors()
    id0 = interp.get_input_details()[0]
    od0 = interp.get_output_details()[0]
    in_sc, in_zp = id0["quantization"]
    out_sc, out_zp = od0["quantization"]
    return tfl_bytes, {
        "input_shape": [int(v) for v in id0["shape"]],
        "output_shape": [int(v) for v in od0["shape"]],
        "input_scale": float(in_sc),
        "input_zero_point": int(in_zp),
        "output_scale": float(out_sc),
        "output_zero_point": int(out_zp),
        "tflite_bytes": len(tfl_bytes),
    }


def prune_onnx_classifier_head(
    onnx_path: Path,
    keep_class_ids: list[int],
    out_onnx_path: Path,
) -> None:
    """Slice final CTC linear head weights and biases to `keep_class_ids`."""
    np, (onnx, numpy_helper, _ort), _tf, _keras, _f32 = _onnx_deps()
    model = onnx.load(str(onnx_path))  # type: ignore[attr-defined]
    out_dim = model.graph.output[0].type.tensor_type.shape.dim[-1].dim_value
    idx = np.array(keep_class_ids, dtype=np.int64)  # type: ignore[attr-defined]
    for node in model.graph.node:
        if node.op_type == "Constant":
            arr = numpy_helper.to_array(node.attribute[0].t)  # type: ignore[attr-defined]
            if arr.ndim == _NDIM_2D and arr.shape[1] == out_dim:
                sliced = arr[:, idx].copy()
                node.attribute[0].t.CopyFrom(numpy_helper.from_array(sliced, node.output[0]))  # type: ignore[attr-defined]
            elif arr.ndim == 1 and arr.shape[0] == out_dim:
                sliced = arr[idx].copy()
                node.attribute[0].t.CopyFrom(numpy_helper.from_array(sliced, node.output[0]))  # type: ignore[attr-defined]
    for out_info in model.graph.output:
        out_info.type.tensor_type.shape.dim[-1].dim_value = len(keep_class_ids)
    out_onnx_path.parent.mkdir(parents=True, exist_ok=True)
    onnx.save(model, str(out_onnx_path))  # type: ignore[attr-defined]


def verify_float_parity(
    onnx_path: Path,
    keras_model: object,
    spec: ModelPipelineSpec,
) -> float:
    """Verify max absolute output difference between ORT float32 and Keras."""
    np, (_onx, _nh, ort), _tf, _keras, _f32 = _onnx_deps()
    rng = np.random.Generator(np.random.PCG64(0))  # type: ignore[attr-defined]
    x_rgb = rng.uniform(
        -1.0,
        1.0,
        (1, spec.preprocess.height, spec.preprocess.width, _RGB_CH),
    ).astype(np.float32)  # type: ignore[attr-defined]
    x_bgr = x_rgb.transpose(0, 3, 1, 2)[:, ::-1, :, :].copy()
    so = ort.SessionOptions()  # type: ignore[attr-defined]
    so.intra_op_num_threads = 2
    so.inter_op_num_threads = 1
    sess = ort.InferenceSession(str(onnx_path), sess_options=so, providers=["CPUExecutionProvider"])  # type: ignore[attr-defined]
    y_ort = sess.run(None, {sess.get_inputs()[0].name: x_bgr})[0]
    if spec.kind == "detector":
        y_ort = y_ort.transpose(0, 2, 3, 1)
    y_tf = keras_model(x_rgb, training=False).numpy()  # type: ignore[operator]
    if spec.drop_softmax:
        exp = np.exp(y_tf - np.max(y_tf, axis=-1, keepdims=True))  # type: ignore[attr-defined]
        y_tf = exp / np.sum(exp, axis=-1, keepdims=True)  # type: ignore[attr-defined]
    return float(np.max(np.abs(y_ort - y_tf)))  # type: ignore[attr-defined]
