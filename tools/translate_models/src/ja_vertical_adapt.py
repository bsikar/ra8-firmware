"""Deterministic Japanese vertical-column (ccw_90) and horizontal CTC adaptation."""

from __future__ import annotations

import math
from dataclasses import dataclass
from pathlib import Path

from tchr_format import DeterministicPrng

_TWO_INPUTS = 2
_SLICE_AXES_MIN = 3
_SLICE_STEPS_MIN = 4
_RGB_CH = 3
_HIRA_LO = 0x3041
_HIRA_HI = 0x3093
_KATA_LO = 0x30A1
_KATA_HI = 0x30F3
_KANJI_LO = 0x4E00
_KANJI_HI = 0x9FFF
_VERT_PAD_PROB = 0.30
_VERT_STRETCH_PROB = 0.75
_BATCH_SIZE = 16
_LR_MAX = 3.5e-4
_LR_MIN = 1.5e-5
_BN_SETTLE_BATCHES = 20
_POW_CLIP_PENALTY_LIMIT = 4.0


def _adapt_deps() -> tuple[
    object,
    tuple[object, object, object],
    tuple[object, object, object],
    object,
    object,
    object,
]:
    """Lazily import PyTorch, ONNX, ORT, and Pillow only when adapting weights."""
    import numpy as np  # noqa: PLC0415
    import onnx  # noqa: PLC0415
    import onnxruntime as ort  # noqa: PLC0415
    import torch  # noqa: PLC0415
    import torch.nn.functional as torch_f  # noqa: PLC0415
    from onnx import numpy_helper  # noqa: PLC0415
    from PIL import Image, ImageDraw, ImageFont  # noqa: PLC0415
    from torch import nn  # noqa: PLC0415

    return np, (onnx, numpy_helper, ort), (Image, ImageDraw, ImageFont), torch, nn, torch_f


@dataclass
class _AdaptGraph:
    """Captured ONNX graph structure, constants, and tensor shapes."""

    model: object
    in_name: str
    consts: dict[str, object]
    const_node_idx: dict[str, int]
    pvals: dict[str, object]
    shapes: dict[str, tuple[int, ...]]
    non_const: list[object]
    static_names: set[str]


def _collect_onnx_consts(model: object) -> tuple[dict[str, object], dict[str, int]]:
    """Collect initializers and Constant-node tensors plus node indices."""
    _np, (_onx, numpy_helper, _ort), _pil, _torch, _nn, _f = _adapt_deps()
    consts: dict[str, object] = {
        init.name: numpy_helper.to_array(init)  # type: ignore[attr-defined]
        for init in model.graph.initializer  # type: ignore[attr-defined]
    }
    const_node_idx: dict[str, int] = {}
    for ni, node in enumerate(model.graph.node):  # type: ignore[attr-defined]
        if node.op_type == "Constant":
            for attr in node.attribute:
                if attr.name == "value":
                    consts[node.output[0]] = numpy_helper.to_array(attr.t)  # type: ignore[attr-defined]
                    const_node_idx[node.output[0]] = ni
    return consts, const_node_idx


def _probe_onnx_values(
    model: object,
    height: int,
    width: int,
) -> tuple[str, dict[str, object]]:
    """Run one ORT shape/constant probe across all graph node outputs."""
    np, (onnx, _nh, ort), _pil, _torch, _nn, _f = _adapt_deps()
    probe = onnx.ModelProto()  # type: ignore[attr-defined]
    probe.CopyFrom(model)
    seen = {o.name for o in probe.graph.output}
    for node in probe.graph.node:
        if node.op_type == "Constant":
            continue
        for out_name in node.output:
            if out_name and out_name not in seen:
                probe.graph.output.append(onnx.helper.ValueInfoProto(name=out_name))  # type: ignore[attr-defined]
                seen.add(out_name)

    so = ort.SessionOptions()  # type: ignore[attr-defined]
    so.intra_op_num_threads = 2
    so.inter_op_num_threads = 1
    ps = ort.InferenceSession(  # type: ignore[attr-defined]
        probe.SerializeToString(),
        sess_options=so,
        providers=["CPUExecutionProvider"],
    )
    in_name = model.graph.input[0].name  # type: ignore[attr-defined]
    dummy = np.zeros((1, _RGB_CH, height, width), dtype=np.float32)  # type: ignore[attr-defined]
    outputs = ps.run(None, {in_name: dummy})
    return in_name, {o.name: v for o, v in zip(probe.graph.output, outputs, strict=True)}


def _load_adapt_graph(onnx_path: Path, height: int, width: int) -> _AdaptGraph:
    """Load pruned ONNX recognizer and probe intermediate shapes at fixed geometry."""
    _np, (onnx, _nh, _ort), _pil, _torch, _nn, _f = _adapt_deps()
    model = onnx.load(str(onnx_path))  # type: ignore[attr-defined]
    in_name, pvals = _probe_onnx_values(model, height, width)
    shapes: dict[str, tuple[int, ...]] = {
        k: v.shape  # type: ignore[attr-defined]
        for k, v in pvals.items()
    }
    shapes[in_name] = (1, _RGB_CH, height, width)

    consts, const_node_idx = _collect_onnx_consts(model)
    for k, v in consts.items():
        shapes[k] = v.shape  # type: ignore[attr-defined]

    non_const = [n for n in model.graph.node if n.op_type != "Constant"]
    static_names = set(consts.keys())
    for node in non_const:
        if all(i in static_names or i == "" for i in node.input) or node.op_type == "Shape":
            static_names.update(node.output)
    for out_name in static_names:
        if out_name not in consts and out_name in pvals:
            consts[out_name] = pvals[out_name].copy()  # type: ignore[attr-defined]
    return _AdaptGraph(
        model,
        in_name,
        consts,
        const_node_idx,
        pvals,
        shapes,
        non_const,
        static_names,
    )


def _init_conv_or_bn(
    graph: _AdaptGraph,
    idx: int,
    node: object,
    at: dict[str, object],
    *,
    targets: tuple[object, list[tuple[str, object]]],
) -> str:
    """Build PyTorch Conv2d or BatchNorm2d matching an ONNX node."""
    _np, _onx, _pil, torch, nn, _f = _adapt_deps()
    mods, export_map = targets
    ins = node.input  # type: ignore[attr-defined]
    if node.op_type == "Conv":  # type: ignore[attr-defined]
        w = graph.consts[ins[1]].copy()  # type: ignore[attr-defined]
        b = graph.consts[ins[2]].copy() if len(ins) > _TWO_INPUTS else None  # type: ignore[attr-defined]
        groups = int(at.get("group", 1))  # type: ignore[arg-type]
        conv = nn.Conv2d(  # type: ignore[attr-defined]
            int(w.shape[1]) * groups,
            int(w.shape[0]),
            (int(w.shape[2]), int(w.shape[3])),
            stride=tuple(at.get("strides", [1, 1])),  # type: ignore[arg-type]
            padding=(0, 0),
            dilation=tuple(at.get("dilations", [1, 1])),  # type: ignore[arg-type]
            groups=groups,
            bias=(b is not None),
        )
        with torch.no_grad():  # type: ignore[attr-defined]
            conv.weight.copy_(torch.from_numpy(w))  # type: ignore[attr-defined]
            if b is not None and conv.bias is not None:
                conv.bias.copy_(torch.from_numpy(b))  # type: ignore[attr-defined]
        key = f"conv_{idx}"
        mods[key] = conv  # type: ignore[index]
        export_map.append((ins[1], lambda c=conv: c.weight.detach().cpu().numpy()))
        if b is not None:
            export_map.append((ins[2], lambda c=conv: c.bias.detach().cpu().numpy()))
        return key

    sc, bs, mn, vr = (graph.consts[k].copy() for k in ins[1:5])  # type: ignore[attr-defined]
    eps = float(at.get("epsilon", 1e-5))  # type: ignore[arg-type]
    bn = nn.BatchNorm2d(len(sc), eps=eps, momentum=0.05)  # type: ignore[attr-defined]
    with torch.no_grad():  # type: ignore[attr-defined]
        bn.weight.copy_(torch.from_numpy(sc))  # type: ignore[attr-defined]
        bn.bias.copy_(torch.from_numpy(bs))  # type: ignore[attr-defined]
        bn.running_mean.copy_(torch.from_numpy(mn))  # type: ignore[attr-defined]
        bn.running_var.copy_(torch.from_numpy(vr))  # type: ignore[attr-defined]
    key = f"bn_{idx}"
    mods[key] = bn  # type: ignore[index]
    export_map.append((ins[1], lambda b_=bn: b_.weight.detach().cpu().numpy()))
    export_map.append((ins[2], lambda b_=bn: b_.bias.detach().cpu().numpy()))
    export_map.append((ins[3], lambda b_=bn: b_.running_mean.detach().cpu().numpy()))
    export_map.append((ins[4], lambda b_=bn: b_.running_var.detach().cpu().numpy()))
    return key


def _init_add_or_buffer(
    graph: _AdaptGraph,
    idx: int,
    node: object,
    targets: tuple[object, object, list[tuple[str, object]]],
) -> str | None:
    """Register trainable Add bias parameter or frozen binary buffer."""
    np, _onx, _pil, torch, nn, _f = _adapt_deps()
    mod_ref, params, export_map = targets
    op = node.op_type  # type: ignore[attr-defined]
    a_name, b_name = node.input[0], node.input[1]  # type: ignore[attr-defined]
    if op == "Add":
        if b_name in graph.consts and a_name not in graph.consts:
            arr = graph.consts[b_name].astype(np.float32).copy()  # type: ignore[attr-defined]
            param = nn.Parameter(torch.from_numpy(arr))  # type: ignore[attr-defined]
            key = f"addp_{idx}"
            params[key] = param  # type: ignore[index]
            export_map.append((b_name, lambda p_=param: p_.detach().cpu().numpy()))
            return key
        if a_name in graph.consts and b_name not in graph.consts:
            arr = graph.consts[a_name].astype(np.float32).copy()  # type: ignore[attr-defined]
            param = nn.Parameter(torch.from_numpy(arr))  # type: ignore[attr-defined]
            key = f"addp_a_{idx}"
            params[key] = param  # type: ignore[index]
            export_map.append((a_name, lambda p_=param: p_.detach().cpu().numpy()))
            return key
        return None
    if b_name in graph.consts and a_name not in graph.consts:
        key = f"c_{idx}_b"
        arr = graph.consts[b_name].astype(np.float32).copy()  # type: ignore[attr-defined]
        mod_ref.register_buffer(key, torch.from_numpy(arr))  # type: ignore[attr-defined]
        return key
    if a_name in graph.consts and b_name not in graph.consts:
        key = f"c_{idx}_a"
        arr = graph.consts[a_name].astype(np.float32).copy()  # type: ignore[attr-defined]
        mod_ref.register_buffer(key, torch.from_numpy(arr))  # type: ignore[attr-defined]
        return key
    return None


def _init_param_or_attr(
    graph: _AdaptGraph,
    idx: int,
    node: object,
    at: dict[str, object],
    targets: tuple[object, object, list[tuple[str, object]]],
) -> tuple[str | None, dict[str, object]]:
    """Register trainable MatMul/Add parameters or Slice metadata."""
    _np, _onx, _pil, torch, nn, _f = _adapt_deps()
    _mod_ref, params, export_map = targets
    op = node.op_type  # type: ignore[attr-defined]
    ins = node.input  # type: ignore[attr-defined]
    if op == "MatMul" and ins[1] in graph.consts:
        param = nn.Parameter(torch.from_numpy(graph.consts[ins[1]].copy()))  # type: ignore[attr-defined]
        key = f"mmw_{idx}"
        params[key] = param  # type: ignore[index]
        export_map.append((ins[1], lambda p_=param: p_.detach().cpu().numpy()))
        return key, at
    if op in ("Add", "Mul", "Div", "Sub", "Pow"):
        return _init_add_or_buffer(graph, idx, node, targets), at
    if op == "Reshape":
        return None, {"shape": graph.shapes[node.output[0]]}  # type: ignore[attr-defined]
    if op == "Slice":
        st = graph.pvals[ins[1]] if ins[1] in graph.pvals else graph.consts[ins[1]]
        en = graph.pvals[ins[2]] if ins[2] in graph.pvals else graph.consts[ins[2]]
        ax = (
            (graph.pvals[ins[3]] if ins[3] in graph.pvals else graph.consts[ins[3]])
            .astype(int)  # type: ignore[attr-defined]
            .tolist()
            if len(ins) > _SLICE_AXES_MIN
            else None
        )
        sp = (
            (graph.pvals[ins[4]] if ins[4] in graph.pvals else graph.consts[ins[4]])
            .astype(int)  # type: ignore[attr-defined]
            .tolist()
            if len(ins) > _SLICE_STEPS_MIN
            else None
        )
        return None, {
            "starts": st.astype(int).tolist(),  # type: ignore[attr-defined]
            "ends": en.astype(int).tolist(),  # type: ignore[attr-defined]
            "axes": ax,
            "steps": sp,
            "in_shape": graph.shapes[ins[0]],
        }
    return None, at


def _run_pt_binary(
    module: object,
    env: dict[str, object],
    spec: tuple[str, str | None, list[str], str],
    act_pen: object,
) -> object:
    """Execute Add, Mul, Div, Sub, or Pow node and return updated Pow penalty."""
    _np, _onx, _pil, _torch, _nn, torch_f = _adapt_deps()
    op, key, inps, out_n = spec
    if key is not None:
        if key.startswith("addp_a"):
            ta, tb = module.params[key], env[inps[1]]  # type: ignore[attr-defined]
        elif key.startswith("addp"):
            ta, tb = env[inps[0]], module.params[key]  # type: ignore[attr-defined]
        elif key.endswith("_a"):
            ta, tb = getattr(module, key), env[inps[1]]
        else:
            ta, tb = env[inps[0]], getattr(module, key)
    else:
        ta, tb = env[inps[0]], env[inps[1]]

    if op == "Add":
        env[out_n] = ta + tb  # type: ignore[operator]
    elif op == "Mul":
        env[out_n] = ta * tb  # type: ignore[operator]
    elif op == "Div":
        env[out_n] = ta / tb  # type: ignore[operator]
    elif op == "Sub":
        env[out_n] = ta - tb  # type: ignore[operator]
    else:
        if module.training:  # type: ignore[attr-defined]
            pen = torch_f.relu(ta.abs() - _POW_CLIP_PENALTY_LIMIT) ** 2  # type: ignore[attr-defined]
            act_pen = act_pen + pen.mean()  # type: ignore[operator]
        env[out_n] = ta**tb  # type: ignore[operator]
    return act_pen


def _run_pt_shape_or_pool(
    env: dict[str, object],
    spec: tuple[str, list[str], str, dict[str, object]],
    graph: _AdaptGraph,
) -> None:
    """Execute spatial pooling, Concat, Squeeze/Unsqueeze, Transpose, Reshape, Slice."""
    np, _onx, _pil, torch, _nn, torch_f = _adapt_deps()
    op, inps, out_n, at = spec
    if op == "AveragePool":
        pads = list(at.get("pads", [0, 0, 0, 0]))  # type: ignore[call-overload]
        ksize = tuple(at["kernel_shape"])  # type: ignore[arg-type]
        strides = tuple(at.get("strides", [1, 1]))  # type: ignore[arg-type]
        env[out_n] = torch_f.avg_pool2d(env[inps[0]], ksize, strides, (pads[0], pads[1]))  # type: ignore[attr-defined]
    elif op == "GlobalAveragePool":
        env[out_n] = torch.mean(env[inps[0]], dim=(2, 3), keepdim=True)  # type: ignore[attr-defined]
    elif op == "Concat":
        dev = env[graph.in_name].device  # type: ignore[attr-defined]
        items = [
            env[i] if i in env else torch.from_numpy(graph.consts[i].astype(np.float32)).to(dev)  # type: ignore[attr-defined]
            for i in inps
        ]
        env[out_n] = torch.cat(items, dim=int(at["axis"]))  # type: ignore[attr-defined,arg-type]
    elif op in ("Squeeze", "Unsqueeze"):
        axes = at.get("axes")
        if axes is None and len(inps) > 1:
            ax_raw = graph.pvals[inps[1]] if inps[1] in graph.pvals else graph.consts[inps[1]]
            axes = ax_raw.astype(int).tolist()  # type: ignore[attr-defined]
        v = env[inps[0]]
        for ax in sorted(axes or [], reverse=(op == "Squeeze")):  # type: ignore[arg-type]
            v = v.squeeze(ax) if op == "Squeeze" else v.unsqueeze(ax)  # type: ignore[attr-defined]
        env[out_n] = v
    elif op == "Transpose":
        env[out_n] = env[inps[0]].permute(*at["perm"])  # type: ignore[attr-defined,misc]
    elif op == "Reshape":
        tgt = at["shape"]
        sh = (-1, *tgt[1:]) if tgt[0] == 1 else tuple(tgt)  # type: ignore[index,arg-type]
        env[out_n] = torch.reshape(env[inps[0]], sh)  # type: ignore[attr-defined]
    else:
        v = env[inps[0]]
        rank = len(at["in_shape"])  # type: ignore[arg-type]
        starts = list(at["starts"])  # type: ignore[call-overload]
        ends = list(at["ends"])  # type: ignore[call-overload]
        axes = list(at["axes"]) if at["axes"] is not None else list(range(len(starts)))  # type: ignore[call-overload]
        steps = list(at["steps"]) if at["steps"] is not None else [1] * len(starts)  # type: ignore[call-overload]
        in_shape = at["in_shape"]
        sl = [slice(None)] * rank
        for st, en, ax, step in zip(starts, ends, axes, steps, strict=True):
            ax_norm = ax if ax >= 0 else rank + ax
            sl[ax_norm] = slice(st, min(en, in_shape[ax_norm]), step)  # type: ignore[index]
        env[out_n] = v[tuple(sl)]  # type: ignore[index]


def _run_pt_conv_or_act(
    module: object,
    env: dict[str, object],
    node_spec: tuple[str, str | None, list[str], str, dict[str, object]],
) -> None:
    """Execute Conv, BatchNormalization, or elementwise activation op in PyTorch."""
    _np, _onx, _pil, torch, _nn, torch_f = _adapt_deps()
    op, key, inps, out_n, at = node_spec
    if op == "Identity":
        env[out_n] = env[inps[0]]
    elif op == "Conv":
        v = env[inps[0]]
        pads = list(at.get("pads", [0, 0, 0, 0]))  # type: ignore[call-overload]
        if any(pads):
            v = torch_f.pad(v, (pads[1], pads[3], pads[0], pads[2]))  # type: ignore[attr-defined]
        env[out_n] = module.mods[key](v)  # type: ignore[attr-defined]
    elif op == "BatchNormalization":
        env[out_n] = module.mods[key](env[inps[0]])  # type: ignore[attr-defined]
    elif op == "Relu":
        env[out_n] = torch_f.relu(env[inps[0]])  # type: ignore[attr-defined]
    elif op == "Sigmoid":
        env[out_n] = torch.sigmoid(env[inps[0]])  # type: ignore[attr-defined]
    elif op == "HardSigmoid":
        alpha = float(at.get("alpha", 0.2))  # type: ignore[arg-type]
        beta = float(at.get("beta", 0.5))  # type: ignore[arg-type]
        env[out_n] = torch.clamp(env[inps[0]] * alpha + beta, 0.0, 1.0)  # type: ignore[attr-defined,operator]
    else:
        env[out_n] = torch_f.hardswish(env[inps[0]])  # type: ignore[attr-defined]


def _run_pt_node(
    module: object,
    env: dict[str, object],
    node_spec: tuple[str, str | None, list[str], str, dict[str, object]],
    graph: _AdaptGraph,
    act_pen: object,
) -> tuple[object, tuple[object, object] | None]:
    """Execute one translated ONNX operator in PyTorch."""
    _np, _onx, _pil, torch, _nn, torch_f = _adapt_deps()
    op, key, inps, out_n, at = node_spec
    out_pair: tuple[object, object] | None = None
    act_ops = (
        "Identity",
        "Conv",
        "BatchNormalization",
        "Relu",
        "Sigmoid",
        "HardSigmoid",
        "HardSwish",
    )
    if op in act_ops:
        _run_pt_conv_or_act(module, env, node_spec)
    elif op in (
        "AveragePool",
        "GlobalAveragePool",
        "Concat",
        "Squeeze",
        "Unsqueeze",
        "Transpose",
        "Reshape",
        "Slice",
    ):
        _run_pt_shape_or_pool(env, (op, inps, out_n, at), graph)
    elif op == "ReduceMean":
        keepdims = bool(at.get("keepdims", 1))
        env[out_n] = torch.mean(env[inps[0]], dim=tuple(at["axes"]), keepdim=keepdims)  # type: ignore[attr-defined,arg-type]
    elif op == "Sqrt":
        env[out_n] = torch.sqrt(env[inps[0]])  # type: ignore[attr-defined]
    elif op == "MatMul":
        rhs = module.params[key] if key else env[inps[1]]  # type: ignore[attr-defined]
        env[out_n] = torch.matmul(env[inps[0]], rhs)  # type: ignore[attr-defined]
    elif op in ("Add", "Mul", "Div", "Sub", "Pow"):
        act_pen = _run_pt_binary(module, env, (op, key, inps, out_n), act_pen)
    elif op == "Softmax":
        logits = env[inps[0]]
        sm = torch_f.softmax(logits, dim=int(at.get("axis", -1)))  # type: ignore[attr-defined,arg-type]
        if out_n == graph.model.graph.output[0].name:  # type: ignore[attr-defined]
            out_pair = (logits, sm)
        else:
            env[out_n] = sm
    else:
        msg = f"unsupported ONNX adapt op {op!r}"
        raise NotImplementedError(msg)
    return act_pen, out_pair


def _build_adapt_module(graph: _AdaptGraph) -> object:
    """Create trainable nn.Module wrapping an ONNX graph."""
    _np, (onnx, _nh, _ort), _pil, _torch, nn, _f = _adapt_deps()

    class _Module(nn.Module):  # type: ignore[name-defined,misc]
        def __init__(self) -> None:
            super().__init__()
            self.mods = nn.ModuleDict()  # type: ignore[attr-defined]
            self.params = nn.ParameterDict()  # type: ignore[attr-defined]
            self.plan: list[tuple[str, str | None, list[str], str, dict[str, object]]] = []
            self.export_map: list[tuple[str, object]] = []
            for idx, node in enumerate(graph.non_const):
                if all(o in graph.static_names for o in node.output):  # type: ignore[attr-defined]
                    continue
                at = {
                    a.name: onnx.helper.get_attribute_value(a)  # type: ignore[attr-defined]
                    for a in node.attribute  # type: ignore[attr-defined]
                }
                if node.op_type in ("Conv", "BatchNormalization"):  # type: ignore[attr-defined]
                    key = _init_conv_or_bn(
                        graph,
                        idx,
                        node,
                        at,
                        targets=(self.mods, self.export_map),
                    )
                    self.plan.append(
                        (node.op_type, key, list(node.input), node.output[0], at),  # type: ignore[attr-defined]
                    )
                else:
                    key, at2 = _init_param_or_attr(
                        graph,
                        idx,
                        node,
                        at,
                        (self, self.params, self.export_map),
                    )
                    self.plan.append(
                        (node.op_type, key, list(node.input), node.output[0], at2),  # type: ignore[attr-defined]
                    )

        def forward(self, x: object) -> tuple[object, object | None, object]:
            env: dict[str, object] = {graph.in_name: x}
            act_pen = x.new_zeros(())  # type: ignore[attr-defined]
            for step in self.plan:
                act_pen, res = _run_pt_node(self, env, step, graph, act_pen)
                if res is not None:
                    return res[0], res[1], act_pen
            out_name = graph.model.graph.output[0].name  # type: ignore[attr-defined]
            return env[out_name], None, act_pen

    return _Module()


def render_ja_horiz_rgb(text: str, font: object, *, width: int, bg: int, fg: int) -> object:
    """Render horizontal Japanese line centered in `(48, width, 3)` float32 [-1, 1]."""
    np, _onx, (pil_image, pil_draw, _pf), _torch, _nn, _f = _adapt_deps()
    d0 = pil_draw.Draw(pil_image.new("RGB", (width * 2, 48), (bg, bg, bg)))  # type: ignore[attr-defined]
    bb = d0.textbbox((0, 0), text, font=font)
    tw, th = max(1, bb[2] - bb[0]), max(1, bb[3] - bb[1])
    im = pil_image.new("RGB", (tw + 12, 48), (bg, bg, bg))  # type: ignore[attr-defined]
    ty = max(2, (48 - th) // 2 - bb[1])
    pil_draw.Draw(im).text((6 - bb[0], ty), text, fill=(fg, fg, fg), font=font)  # type: ignore[attr-defined]
    if tw + 12 > width:
        im = im.resize((width, 48), pil_image.Resampling.BILINEAR)  # type: ignore[attr-defined]
    canvas = pil_image.new("RGB", (width, 48), (bg, bg, bg))  # type: ignore[attr-defined]
    canvas.paste(im, (0, 0))
    return (np.asarray(canvas, dtype=np.float32) / 255.0 - 0.5) * 2.0  # type: ignore[attr-defined]


def render_ja_vert_ccw90_rgb(
    text: str,
    font: object,
    geom: tuple[int, int] = (30, 256),
    colors: tuple[int, int] = (255, 0),
    *,
    stretch: bool = False,
) -> object:
    """Render vertical column rotated 90 deg CCW matching `ra8_translate_ocr_rot_ccw_90`."""
    fsize, width = geom
    bg, fg = colors
    np, _onx, (pil_image, pil_draw, _pf), _torch, _nn, _f = _adapt_deps()
    col_w = int(fsize * 1.4)
    step_y = int(fsize * 1.1)
    col_h = max(48, len(text) * step_y + 12)
    col = pil_image.new("RGB", (col_w, col_h), (bg, bg, bg))  # type: ignore[attr-defined]
    draw = pil_draw.Draw(col)  # type: ignore[attr-defined]
    for idx_c, ch in enumerate(text):
        bb = draw.textbbox((0, 0), ch, font=font)
        cw = bb[2] - bb[0]
        draw.text(
            ((col_w - cw) // 2 - bb[0], 6 + idx_c * step_y - bb[1]),
            ch,
            fill=(fg, fg, fg),
            font=font,
        )
    rot = col.transpose(pil_image.Transpose.ROTATE_90)  # type: ignore[attr-defined]
    if stretch:
        canvas = rot.resize((width, 48), pil_image.Resampling.BILINEAR)  # type: ignore[attr-defined]
    else:
        rw, rh = rot.size
        new_w = max(16, round(rw * (48.0 / rh)))
        rot = rot.resize((min(width, new_w), 48), pil_image.Resampling.BILINEAR)  # type: ignore[attr-defined]
        canvas = pil_image.new("RGB", (width, 48), (bg, bg, bg))  # type: ignore[attr-defined]
        canvas.paste(rot, (0, 0))
    return (np.asarray(canvas, dtype=np.float32) / 255.0 - 0.5) * 2.0  # type: ignore[attr-defined]


def build_ja_char_pool(tokens: list[str]) -> list[str]:
    """Build weighted Kana + JIS Level-1 Kanji synthetic sampling pool (zero eval strings)."""
    hira = [c for c in tokens[1:] if len(c) == 1 and _HIRA_LO <= ord(c) <= _HIRA_HI]
    kata = [c for c in tokens[1:] if len(c) == 1 and _KATA_LO <= ord(c) <= _KATA_HI]
    kanji = [c for c in tokens[1:] if len(c) == 1 and _KANJI_LO <= ord(c) <= _KANJI_HI]
    punct = [c for c in tokens[1:] if c in "\u3002\u3001\uff01\uff1f\u30fb\u300c\u300d0123456789"]
    return hira * 8 + kata * 3 + kanji + punct * 2


def _sample_ja_batch(
    prng: DeterministicPrng,
    pool: list[str],
    c2id: dict[str, int],
    fonts: dict[int, object],
    spec: tuple[int, object],
) -> tuple[object, object, object]:
    """Render one mixed vertical/horizontal Japanese CTC training minibatch."""
    width, device = spec
    np, _onx, _pil, torch, _nn, _f = _adapt_deps()
    xs, tgts, tlens = [], [], []
    for _ in range(_BATCH_SIZE):
        length = prng.randint(3, 6)
        txt = "".join(str(prng.choice(pool)) for _ in range(length))
        fsz = int(prng.choice([22, 26, 30, 32]))
        bg = int(prng.choice([255, 245, 236]))
        fg = int(prng.choice([0, 12, 24]))
        mode = prng.randfloat()
        if mode < _VERT_PAD_PROB:
            rgb = render_ja_vert_ccw90_rgb(
                txt,
                fonts[fsz],
                (fsz, width),
                (bg, fg),
                stretch=False,
            )
        elif mode < _VERT_STRETCH_PROB:
            rgb = render_ja_vert_ccw90_rgb(
                txt,
                fonts[fsz],
                (fsz, width),
                (bg, fg),
                stretch=True,
            )
        else:
            rgb = render_ja_horiz_rgb(txt, fonts[fsz], width=width, bg=bg, fg=fg)
        xs.append(rgb.transpose(2, 0, 1)[::-1, :, :].copy())  # type: ignore[attr-defined]
        ids = [c2id[ch] for ch in txt]
        tgts.extend(ids)
        tlens.append(len(ids))
    bx = torch.from_numpy(np.stack(xs, axis=0)).to(device)  # type: ignore[attr-defined]
    bt = torch.tensor(tgts, dtype=torch.long, device=device)  # type: ignore[attr-defined]
    bl = torch.tensor(tlens, dtype=torch.long, device=device)  # type: ignore[attr-defined]
    return bx, bt, bl


def _save_adapted_onnx(graph: _AdaptGraph, module: object, out_onnx: Path) -> None:
    """Serialize adapted PyTorch conv and BatchNorm weights/stats back into ONNX."""
    np, (onnx, numpy_helper, _ort), _pil, _torch, _nn, _f = _adapt_deps()
    exported = onnx.ModelProto()  # type: ignore[attr-defined]
    exported.CopyFrom(graph.model)
    init_map = {init.name: init for init in exported.graph.initializer}
    for cname, getter in module.export_map:  # type: ignore[attr-defined]
        arr = getter().astype(np.float32)  # type: ignore[attr-defined]
        if cname in init_map:
            init_map[cname].CopyFrom(numpy_helper.from_array(arr, cname))  # type: ignore[attr-defined]
        elif cname in graph.const_node_idx:
            const_node = exported.graph.node[graph.const_node_idx[cname]]
            const_node.attribute[0].t.CopyFrom(numpy_helper.from_array(arr, cname))  # type: ignore[attr-defined]
    out_onnx.parent.mkdir(parents=True, exist_ok=True)
    onnx.save(exported, str(out_onnx))  # type: ignore[attr-defined]


@dataclass(frozen=True)
class AdaptConfig:
    """Hyperparameters for Japanese vertical/horizontal CTC adaptation."""

    width: int = 256
    steps: int = 700
    seed: int = 42


def adapt_japanese_vertical(
    pruned_onnx: Path,
    tokens: list[str],
    font_path: Path,
    out_onnx: Path,
    config: AdaptConfig | None = None,
) -> dict[str, float]:
    """Deterministically adapt pruned Japanese recognizer to vertical ccw_90 + horiz text.

    Args:
        pruned_onnx: Pruned ONNX Japanese model path (`ja_pruned.onnx`).
        tokens: Pruned class vocabulary ordered identically to classifier head.
        font_path: Noto Sans CJK JP font file path.
        out_onnx: Destination adapted ONNX file path.
        config: Width, optimization step count, and deterministic RNG seed.

    Returns:
        Summary dictionary recording final step count and ending CTC loss.
    """
    cfg = config or AdaptConfig()
    width, steps, seed = cfg.width, cfg.steps, cfg.seed
    _np, _onx, (_pi, _pd, pil_font), torch, nn, torch_f = _adapt_deps()
    torch.set_num_threads(2)  # type: ignore[attr-defined]
    torch.manual_seed(seed)  # type: ignore[attr-defined]
    has_cuda = bool(torch.cuda.is_available())  # type: ignore[attr-defined]
    device = torch.device("cuda" if has_cuda else "cpu")  # type: ignore[attr-defined]
    if has_cuda:
        torch.cuda.manual_seed_all(seed)  # type: ignore[attr-defined]
        torch.backends.cudnn.deterministic = True  # type: ignore[attr-defined]
        torch.backends.cudnn.benchmark = False  # type: ignore[attr-defined]

    graph = _load_adapt_graph(pruned_onnx, 48, width)
    module = _build_adapt_module(graph).to(device)  # type: ignore[attr-defined]
    module.train()  # type: ignore[attr-defined]

    fonts = {
        sz: pil_font.truetype(str(font_path), sz)  # type: ignore[attr-defined]
        for sz in (22, 26, 30, 32)
    }
    char_pool = build_ja_char_pool(tokens)
    c2id = {c: idx for idx, c in enumerate(tokens) if idx > 0}
    prng = DeterministicPrng(seed)
    opt = torch.optim.AdamW(module.parameters(), lr=_LR_MAX, weight_decay=1e-3)  # type: ignore[attr-defined]
    ctc_fn = nn.CTCLoss(blank=0, zero_infinity=True)  # type: ignore[attr-defined]
    last_loss = 0.0

    for step in range(1, steps + 1):
        bx, bt, bl = _sample_ja_batch(prng, char_pool, c2id, fonts, (width, device))
        logits, _sm, act_pen = module(bx)  # type: ignore[operator]
        lp = torch_f.log_softmax(logits, dim=-1).permute(1, 0, 2)  # type: ignore[attr-defined]
        in_lens = torch.full(  # type: ignore[attr-defined]
            (_BATCH_SIZE,),
            logits.shape[1],
            dtype=torch.long,  # type: ignore[attr-defined]
            device=device,
        )
        loss = ctc_fn(lp, bt, in_lens, bl) + act_pen
        opt.zero_grad()
        loss.backward()
        torch.nn.utils.clip_grad_norm_(module.parameters(), 2.0)  # type: ignore[attr-defined]
        lr = _LR_MIN + 0.5 * (_LR_MAX - _LR_MIN) * (1.0 + math.cos(math.pi * step / steps))
        for group in opt.param_groups:
            group["lr"] = lr
        opt.step()
        last_loss = float(loss.item())

    with torch.no_grad():  # type: ignore[attr-defined]
        for _ in range(_BN_SETTLE_BATCHES):
            bx, _bt, _bl = _sample_ja_batch(prng, char_pool, c2id, fonts, (width, device))
            module(bx)  # type: ignore[operator]

    module.eval()  # type: ignore[attr-defined]
    _save_adapted_onnx(graph, module, out_onnx)
    return {"steps": float(steps), "final_loss": last_loss}
