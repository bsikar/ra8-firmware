# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""CLI for reproducible Bergamot -> TMNT/TVOC/TSLT conversion and golden parity."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

from bergamot_io import parse_bergamot_shortlist, parse_marian_binary, parse_spm
from containers import DEFAULT_MAX_SEQ, pack_tmnt, pack_tslt, pack_tvoc
from manifest import DEFAULT_CDN_BASE, PAIRS, ArtifactPin, get_pair_spec
from ref_marian import TmntRunner, TvocReader

DEFAULT_KOEN_SENTENCES = [
    "\uc548\ub155\ud558\uc138\uc694.",
    "\uc624\ub298 \ub0a0\uc528\uac00 \uc88b\uc2b5\ub2c8\ub2e4.",
    "\uace0\ub9d9\uc2b5\ub2c8\ub2e4.",
    "\uc774 \ucc45\uc740 \uc7ac\ubbf8\uc788\uc2b5\ub2c8\ub2e4.",
    "\uc9c0\uae08 \uba87 \uc2dc\uc785\ub2c8\uae4c?",
]
DEFAULT_JAEN_SENTENCES = [
    "\u3053\u3093\u306b\u3061\u306f\u3002",
    "\u4eca\u65e5\u306f\u3044\u3044\u5929\u6c17\u3067\u3059\u3002",
    "\u3042\u308a\u304c\u3068\u3046\u3054\u3056\u3044\u307e\u3059\u3002",
]
DEFAULT_ZHEN_SENTENCES = [
    "\u4f60\u597d\u3002",
    "\u4eca\u5929\u5929\u6c14\u5f88\u597d\u3002",
    "\u8c22\u8c22\u4f60\u3002",
]
DEFAULT_SENTENCES = {
    "ko-en": DEFAULT_KOEN_SENTENCES,
    "ja-en": DEFAULT_JAEN_SENTENCES,
    "zh-en": DEFAULT_ZHEN_SENTENCES,
}


def _sha256_file(path: Path) -> str:
    """Compute lowercase SHA-256 hex digest of a file."""
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while chunk := handle.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _verify_pin(path: Path, pin: ArtifactPin) -> None:
    """Verify exact byte length and SHA-256 against a pinned upstream artifact."""
    size = path.stat().st_size
    if size != pin.size:
        msg = f"{path.name}: size {size} != pinned {pin.size}"
        raise ValueError(msg)
    got = _sha256_file(path)
    if got != pin.sha256:
        msg = f"{path.name}: sha256 {got} != pinned {pin.sha256}"
        raise ValueError(msg)


def fetch_pair_inputs(pair: str, cache_dir: Path, cdn_base: str = DEFAULT_CDN_BASE) -> Path:
    """Download and SHA-256-verify pinned Mozilla Bergamot inputs for `pair`."""
    spec = get_pair_spec(pair)
    out_dir = cache_dir / spec.pair_tag
    out_dir.mkdir(parents=True, exist_ok=True)
    for pin in (spec.model, spec.vocab, spec.shortlist):
        target = out_dir / pin.filename
        if target.exists():
            _verify_pin(target, pin)
            continue
        url = pin.url(cdn_base)
        req = urllib.request.Request(url, method="GET")  # noqa: S310 -- pinned https CDN URL
        with urllib.request.urlopen(req, timeout=120) as resp:  # noqa: S310
            data = resp.read()
        target.write_bytes(data)
        _verify_pin(target, pin)
    return out_dir


def convert_pair_from_dir(
    pair: str,
    input_dir: Path,
    out_dir: Path,
    *,
    max_seq: int = DEFAULT_MAX_SEQ,
    allow_unpinned: bool = False,
) -> dict[str, object]:
    """Convert raw Bergamot inputs in `input_dir` into `TMNT`, `TVOC`, and `TSLT`."""
    spec = get_pair_spec(pair)
    model_path = input_dir / spec.model.filename
    vocab_path = input_dir / spec.vocab.filename
    lex_path = input_dir / spec.shortlist.filename

    if not allow_unpinned:
        _verify_pin(model_path, spec.model)
        _verify_pin(vocab_path, spec.vocab)
        _verify_pin(lex_path, spec.shortlist)

    model = parse_marian_binary(model_path.read_bytes())
    vocab = parse_spm(vocab_path.read_bytes())
    shortlist = parse_bergamot_shortlist(lex_path.read_bytes(), expect_vocab=len(vocab.pieces))

    tvoc_bytes = pack_tvoc(vocab, norm_fullwidth=True)
    tslt_bytes = pack_tslt(shortlist, len(vocab.pieces))
    tmnt_bytes = pack_tmnt(
        model,
        vocab,
        nmt_model_id=spec.nmt_model_id,
        max_seq=max_seq,
        shortlist_blob=tslt_bytes,
    )

    out_dir.mkdir(parents=True, exist_ok=True)
    tag = spec.pair_tag
    (out_dir / f"{tag}.tvoc").write_bytes(tvoc_bytes)
    (out_dir / f"{tag}.tslt").write_bytes(tslt_bytes)
    (out_dir / f"{tag}.tmnt").write_bytes(tmnt_bytes)

    meta = {
        "pair": spec.pair,
        "nmt_model_id": spec.nmt_model_id,
        "architecture": spec.architecture,
        "source_version": spec.source_version,
        "marian_version": str(model.config.get("version", "")).replace("\x00", "").strip(),
        "license_spdx": spec.license_spdx,
        "license_url": spec.license_url,
        "max_seq": max_seq,
        "source_sha256": {
            spec.model.filename: _sha256_file(model_path),
            spec.vocab.filename: _sha256_file(vocab_path),
            spec.shortlist.filename: _sha256_file(lex_path),
        },
        "output_bytes": {
            f"{tag}.tmnt": len(tmnt_bytes),
            f"{tag}.tvoc": len(tvoc_bytes),
            f"{tag}.tslt": len(tslt_bytes),
        },
        "output_sha256": {
            f"{tag}.tmnt": hashlib.sha256(tmnt_bytes).hexdigest(),
            f"{tag}.tvoc": hashlib.sha256(tvoc_bytes).hexdigest(),
            f"{tag}.tslt": hashlib.sha256(tslt_bytes).hexdigest(),
        },
    }
    (out_dir / f"{tag}.manifest.json").write_text(
        json.dumps(meta, indent=2, ensure_ascii=True) + "\n", encoding="utf-8"
    )
    return meta


def _verify_with_zig_runner(
    zig_runner: Path,
    model_dir: Path,
    pair_tag: str,
    sentences: list[str],
    cases: list[dict[str, object]],
) -> dict[str, object]:
    """Run compiled `marian_zig_runner` and cross-verify exact token/logit parity."""
    tmnt_path = model_dir / f"{pair_tag}.tmnt"
    tvoc_path = model_dir / f"{pair_tag}.tvoc"
    max_tgt_len = max((len(c["greedy_ids"]) for c in cases), default=1) + 4  # type: ignore[arg-type]
    payload = {
        "sentences": sentences,
        "max_tgt_len": max_tgt_len,
        "top_k": 5,
        "verify_payload": True,
        "run_batch": True,
    }
    with tempfile.NamedTemporaryFile("w", suffix=".json", encoding="utf-8", delete=False) as tf:
        tf.write(json.dumps(payload, ensure_ascii=True))
        tmp_path = Path(tf.name)
    try:
        cmd = [
            str(zig_runner),
            "--tmnt",
            str(tmnt_path),
            "--tvoc",
            str(tvoc_path),
            "--input-json",
            str(tmp_path),
        ]
        raw = subprocess.check_output(cmd, text=True)  # noqa: S603
    finally:
        tmp_path.unlink(missing_ok=True)

    zig_rep = json.loads(raw)
    max_diff = 0.0
    for idx, (py_case, z_case) in enumerate(zip(cases, zig_rep["cases"], strict=True)):
        if py_case["src_ids"] != z_case["src_ids"]:
            msg = f"case {idx}: Zig src_ids differ from Python TVOC encoder"
            raise ValueError(msg)
        if py_case["greedy_ids"] != z_case["greedy_ids"]:
            msg = f"case {idx}: Zig greedy_ids differ from Python runner"
            raise ValueError(msg)
        py_steps: list[dict[str, object]] = py_case["steps"]  # type: ignore[assignment]
        for p_step, z_step in zip(py_steps, z_case["steps"], strict=True):
            diff = abs(float(p_step["selected_logit"]) - float(z_step["selected_logit"]))
            max_diff = max(max_diff, diff)
            p_step["zig_logits_sha256"] = z_step["logits_sha256"]
            p_step["zig_selected_logit_bits"] = z_step["selected_logit_bits"]
        if zig_rep.get("batch_shortlist") is not None:
            sl_ids = zig_rep["batch_shortlist"]["cases"][idx]["greedy_ids"]
            if sl_ids != z_case["greedy_ids"]:
                msg = f"case {idx}: Zig shortlist batched decode diverged from single decode"
                raise ValueError(msg)

    batch_summary: dict[str, object] = {"max_step_logit_diff": round(max_diff, 9)}
    if zig_rep.get("batch_full") is not None:
        batch_summary["batch_full_weight_bytes"] = zig_rep["batch_full"]["total_weight_bytes_read"]
    if zig_rep.get("batch_shortlist") is not None:
        batch_summary["batch_shortlist_weight_bytes"] = zig_rep["batch_shortlist"][
            "total_weight_bytes_read"
        ]
    return batch_summary


def generate_golden(
    pair: str,
    model_dir: Path,
    sentences: list[str],
    max_tgt_len: int = 24,
    zig_runner: Path | None = None,
) -> dict[str, object]:
    """Run reference quantized greedy decoding over `sentences` and emit golden dict."""
    spec = get_pair_spec(pair)
    tag = spec.pair_tag
    tmnt_path = model_dir / f"{tag}.tmnt"
    tvoc_path = model_dir / f"{tag}.tvoc"
    tmnt_raw = tmnt_path.read_bytes()
    tvoc_raw = tvoc_path.read_bytes()
    manifest = json.loads((model_dir / f"{tag}.manifest.json").read_text(encoding="utf-8"))

    runner = TmntRunner(tmnt_raw)
    tok = TvocReader.open(tvoc_raw)
    cases: list[dict[str, object]] = []
    for sent in sentences:
        src_ids = tok.encode(sent, add_eos=True)
        steps = runner.greedy_decode_with_logits(src_ids, max_tgt_len, top_k=5)
        out_ids = [int(s["selected_id"]) for s in steps]  # type: ignore[call-overload]
        cases.append(
            {
                "source_text": sent,
                "src_ids": src_ids,
                "greedy_ids": out_ids,
                "decoded_en": tok.decode(out_ids),
                "steps": [
                    {
                        "step": s["step"],
                        "selected_id": s["selected_id"],
                        "selected_logit": round(float(s["selected_logit"]), 6),  # type: ignore[arg-type]
                        "top_candidates": [
                            {"id": c["id"], "logit": round(float(c["logit"]), 6)}
                            for c in s["top_candidates"]  # type: ignore[attr-defined]
                        ],
                    }
                    for s in steps
                ],
            }
        )
    out: dict[str, object] = {
        "schema": 1,
        "pair": spec.pair,
        "source_version": spec.source_version,
        "marian_version": manifest.get("marian_version", ""),
        "license_spdx": spec.license_spdx,
        "license_url": spec.license_url,
        "source_sha256": manifest["source_sha256"],
        "output_sha256": manifest["output_sha256"],
        "zig_verified": zig_runner is not None,
        "cases": cases,
    }
    if zig_runner is not None:
        out["zig_verification"] = _verify_with_zig_runner(
            zig_runner, model_dir, tag, sentences, cases
        )
    return out


def _cmd_pairs() -> int:
    """Print registered language pairs and license info."""
    for spec in PAIRS.values():
        print(
            f"{spec.pair:6s} id={spec.nmt_model_id} v{spec.source_version} "
            f"{spec.architecture} license={spec.license_spdx}"
        )
    return 0


def _cmd_inspect(tmnt_path: Path, tvoc_path: Path | None) -> int:
    """Validate TMNT (and optional TVOC) containers and print summary metrics."""
    runner = TmntRunner(tmnt_path.read_bytes())
    print(
        f"TMNT d={runner.d_model} ffn={runner.d_ff} enc={runner.num_enc_layers} "
        f"dec={runner.num_dec_layers} vocab={runner.vocab_size} flags={runner.model_flags:#x} "
        f"first_k={runner.shortlist_first_k}"
    )
    if tvoc_path is not None:
        tok = TvocReader.open(tvoc_path.read_bytes())
        print(f"TVOC vocab={tok.vocab_size} model_type={tok.model_type} flags={tok.flags:#x}")
    return 0


def _build_parser() -> argparse.ArgumentParser:
    """Construct the CLI argument parser for marian_conv."""
    parser = argparse.ArgumentParser(description="Bergamot Marian -> TMNT/TVOC/TSLT converter.")
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("pairs", help="List pinned Bergamot language pairs.")

    p_fetch = sub.add_parser("fetch", help="Download and SHA-256 verify pinned pair inputs.")
    p_fetch.add_argument("--pair", default="ko-en")
    p_fetch.add_argument("--cache-dir", type=Path, required=True)

    p_conv = sub.add_parser("convert", help="Convert Bergamot binary assets into TMNT/TVOC/TSLT.")
    p_conv.add_argument("--pair", default="ko-en")
    p_conv.add_argument("--input-dir", type=Path, required=True)
    p_conv.add_argument("--out-dir", type=Path, required=True)
    p_conv.add_argument("--max-seq", type=int, default=DEFAULT_MAX_SEQ)
    p_conv.add_argument("--allow-unpinned", action="store_true")

    p_gold = sub.add_parser("emit-golden", help="Generate reference greedy decode goldens.")
    p_gold.add_argument("--pair", default="ko-en")
    p_gold.add_argument("--model-dir", type=Path, required=True)
    p_gold.add_argument("--out-json", type=Path, required=True)
    p_gold.add_argument("--zig-runner", type=Path)

    p_insp = sub.add_parser("inspect", help="Validate TMNT/TVOC containers and print header.")
    p_insp.add_argument("--tmnt", type=Path, required=True)
    p_insp.add_argument("--tvoc", type=Path)
    return parser


def _dispatch(args: argparse.Namespace) -> int:
    """Execute one parsed marian_conv subcommand."""
    if args.cmd == "pairs":
        return _cmd_pairs()
    if args.cmd == "fetch":
        out = fetch_pair_inputs(args.pair, args.cache_dir)
        print(f"verified inputs in {out}")
        return 0
    if args.cmd == "convert":
        meta = convert_pair_from_dir(
            args.pair,
            args.input_dir,
            args.out_dir,
            max_seq=args.max_seq,
            allow_unpinned=args.allow_unpinned,
        )
        print(json.dumps(meta, indent=2, ensure_ascii=True))
        return 0
    if args.cmd == "emit-golden":
        sentences = DEFAULT_SENTENCES.get(args.pair, DEFAULT_KOEN_SENTENCES)
        golden = generate_golden(args.pair, args.model_dir, sentences, zig_runner=args.zig_runner)
        args.out_json.parent.mkdir(parents=True, exist_ok=True)
        args.out_json.write_text(
            json.dumps(golden, indent=2, ensure_ascii=True) + "\n", encoding="utf-8"
        )
        return 0
    return _cmd_inspect(args.tmnt, args.tvoc)


def main(argv: list[str] | None = None) -> int:
    """Entry point for the Bergamot Marian converter CLI."""
    parser = _build_parser()
    args = parser.parse_args(argv)
    return _dispatch(args)


if __name__ == "__main__":
    sys.exit(main())
