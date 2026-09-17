#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Keep config/docs_site_slots.json honest about the documentation site (#900).

ADR-0005 splits the site across four generators: the Markdown hub at ``/``, the
C ABI Doxygen reference at ``/api/c/``, Zig autodoc at ``/api/zig/`` and
rustdoc at ``/api/rust/``.  scripts/builders/docs_site.sh mounts whichever of
those are wired into one tree.  Because the four arrive on different branches,
the manifest carries a *state* per slot, and a state is exactly the kind of
thing that rots: a builder lands, nobody flips the slot, and the published site
quietly loses a whole language.

So this checker compares every state against the tree and fails on drift:

  * the slot set or its URL paths drifting from ADR-0005,
  * ``wired`` without its builder actually in the tree,
  * ``pending`` when the builder has since landed (flip it to wired),
  * ``absent`` carrying a builder, or a mount point that escapes the site root,
  * ``pending``/``absent`` with no reason, or a reason too thin to act on,
  * Rust sources in the tree while the rustdoc slot is not wired.

Usage:
  python3 scripts/checks/check_docs_site_slots.py [--check]
  python3 scripts/checks/check_docs_site_slots.py --selftest
  python3 scripts/checks/check_docs_site_slots.py --emit-plan
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
MANIFEST = REPO_ROOT / "config" / "docs_site_slots.json"

# The contract ADR-0005 fixes: slot id -> published URL path.
ADR_SLOTS = {
    "hub": "/",
    "capi": "/api/c/",
    "zig": "/api/zig/",
    "rust": "/api/rust/",
}
STATES = ("wired", "pending", "absent")
MIN_REASON = 20


def mount_for(url_path: str) -> str:
    return url_path.strip("/")


def load_manifest(path: Path) -> dict:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def tracked_files(repo_root: Path) -> list[str]:
    """Every tracked path, from git when available, else a filesystem walk."""
    try:
        out = subprocess.run(
            ["git", "-C", str(repo_root), "ls-files"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        return [line for line in out.splitlines() if line]
    except (OSError, subprocess.CalledProcessError):
        return [
            str(p.relative_to(repo_root))
            for p in repo_root.rglob("*")
            if p.is_file() and ".git/" not in str(p.relative_to(repo_root))
        ]


def has_rust_sources(files: list[str]) -> bool:
    return any(f.endswith("Cargo.toml") or f.endswith(".rs") for f in files)


def validate(manifest: dict, files: list[str]) -> list[str]:
    """Return every problem with this manifest against this file list."""
    errors: list[str] = []
    present = set(files)

    site_root = manifest.get("site_root")
    if not isinstance(site_root, str) or not site_root.startswith("build/"):
        errors.append(
            f"site_root must be a path under build/ (generated output is never committed), got {site_root!r}"
        )

    slots = manifest.get("slots")
    if not isinstance(slots, list) or not slots:
        return errors + ["slots must be a non-empty list"]

    ids = [s.get("id") for s in slots]
    if sorted(i for i in ids if isinstance(i, str)) != sorted(ADR_SLOTS):
        errors.append(
            f"slot ids {sorted(str(i) for i in ids)} drift from ADR-0005 {sorted(ADR_SLOTS)}"
        )
    if len(set(ids)) != len(ids):
        errors.append("duplicate slot ids")

    seen_mounts: dict[str, str] = {}
    for slot in slots:
        sid = slot.get("id")
        where = f"slot {sid!r}"

        expected_url = ADR_SLOTS.get(sid)
        if expected_url is None:
            errors.append(f"{where}: unknown slot id, ADR-0005 defines {sorted(ADR_SLOTS)}")
            continue
        if slot.get("url_path") != expected_url:
            errors.append(
                f"{where}: url_path {slot.get('url_path')!r} != ADR-0005 {expected_url!r}"
            )

        mount = slot.get("mount")
        if mount != mount_for(expected_url):
            errors.append(
                f"{where}: mount {mount!r} does not match url_path {expected_url!r}"
            )
        if isinstance(mount, str):
            if mount.startswith("/") or ".." in Path(mount).parts:
                errors.append(f"{where}: mount {mount!r} escapes the site root")
            if mount in seen_mounts:
                errors.append(
                    f"{where}: mount {mount!r} collides with slot {seen_mounts[mount]!r}"
                )
            seen_mounts[mount] = sid

        if not slot.get("title"):
            errors.append(f"{where}: needs a title, it is what the site index prints")

        state = slot.get("state")
        if state not in STATES:
            errors.append(f"{where}: state {state!r} not one of {STATES}")
            continue

        builder = slot.get("builder")
        default_path = slot.get("default_path")
        reason = slot.get("reason")

        if state == "absent":
            if builder is not None or default_path is not None:
                errors.append(
                    f"{where}: state 'absent' must carry no builder and no default_path"
                )
        else:
            if not isinstance(builder, str) or not builder:
                errors.append(f"{where}: state {state!r} needs a builder path")
                continue
            if not isinstance(default_path, str) or not default_path.startswith("build/"):
                errors.append(
                    f"{where}: default_path must be the builder's own output under build/, got {default_path!r}"
                )
            built = builder in present
            if state == "wired" and not built:
                errors.append(
                    f"{where}: state 'wired' but {builder} is not in the tree"
                )
            if state == "pending" and built:
                errors.append(
                    f"{where}: state 'pending' but {builder} has landed, flip this slot to 'wired'"
                )

        if state != "wired":
            if not isinstance(reason, str) or len(reason.strip()) < MIN_REASON:
                errors.append(
                    f"{where}: state {state!r} needs a reason saying where the generator is coming from"
                )
        elif reason not in (None, ""):
            errors.append(f"{where}: a wired slot carries no reason")

    by_id = {s.get("id"): s for s in slots}
    rust = by_id.get("rust")
    if rust is not None and rust.get("state") != "wired" and has_rust_sources(files):
        errors.append(
            "rust sources are in the tree but the rustdoc slot is not wired: "
            "ADR-0005 publishes them at /api/rust/"
        )

    if not any(s.get("mount") == "" for s in slots):
        errors.append("no slot mounts at the site root, the site would have no landing page")

    return errors


def emit_plan(manifest: dict) -> str:
    """Build plan for docs_site.sh: id, state, mount, builder, default_path, title.

    Fields are separated by US (0x1f), not by tabs: a slot legitimately has
    empty fields (the hub mounts at the site root, an absent slot has no
    builder), and bash `read` treats tabs as IFS whitespace, so consecutive
    tabs collapse and every later field shifts one to the left.
    """
    lines = []
    for slot in manifest["slots"]:
        lines.append(
            "\x1f".join(
                [
                    slot["id"],
                    slot["state"],
                    slot.get("mount") or "",
                    slot.get("builder") or "",
                    slot.get("default_path") or "",
                    slot.get("title") or "",
                ]
            )
        )
    return "\n".join(lines)


def _base_manifest() -> dict:
    return {
        "site_root": "build/docs/site",
        "slots": [
            {
                "id": "hub",
                "title": "Markdown hub",
                "url_path": "/",
                "mount": "",
                "state": "wired",
                "builder": "scripts/builders/docs_hub.sh",
                "default_path": "build/docs/hub",
                "reason": None,
            },
            {
                "id": "capi",
                "title": "C ABI reference",
                "url_path": "/api/c/",
                "mount": "api/c",
                "state": "wired",
                "builder": "scripts/builders/docs_capi.sh",
                "default_path": "build/docs/api/c",
                "reason": None,
            },
            {
                "id": "zig",
                "title": "Zig autodoc",
                "url_path": "/api/zig/",
                "mount": "api/zig",
                "state": "pending",
                "builder": "scripts/builders/docs_zig.sh",
                "default_path": "build/docs/api/zig",
                "reason": "arrives with the zig autodoc slice on zig/dev",
            },
            {
                "id": "rust",
                "title": "Rust crates",
                "url_path": "/api/rust/",
                "mount": "api/rust",
                "state": "absent",
                "builder": None,
                "default_path": None,
                "reason": "no Cargo.toml in the tree yet, the slot is reserved",
            },
        ],
    }


def selftest() -> int:
    good_files = [
        "scripts/builders/docs_hub.sh",
        "scripts/builders/docs_capi.sh",
        "README.md",
    ]
    probes: list[tuple[str, dict, list[str], bool]] = []

    probes.append(("clean manifest passes", _base_manifest(), good_files, True))

    m = _base_manifest()
    m["slots"][1]["state"] = "wired"
    probes.append(
        ("wired slot without its builder fails", m, ["scripts/builders/docs_hub.sh"], False)
    )

    m = _base_manifest()
    probes.append(
        (
            "pending slot whose builder landed fails",
            m,
            good_files + ["scripts/builders/docs_zig.sh"],
            False,
        )
    )

    m = _base_manifest()
    m["slots"][2]["reason"] = "later"
    probes.append(("thin reason fails", m, good_files, False))

    m = _base_manifest()
    m["slots"][1]["url_path"] = "/api/capi/"
    probes.append(("url_path drifting from ADR-0005 fails", m, good_files, False))

    m = _base_manifest()
    m["slots"][1]["mount"] = "../escape"
    probes.append(("mount escaping the site root fails", m, good_files, False))

    m = _base_manifest()
    m["slots"][1]["mount"] = ""
    probes.append(("colliding mounts fail", m, good_files, False))

    m = _base_manifest()
    m["slots"].pop(0)
    probes.append(("missing slot fails", m, good_files, False))

    m = _base_manifest()
    probes.append(
        (
            "a Cargo.toml in the tree forces the rustdoc slot",
            m,
            good_files + ["crates/ra8_sys/Cargo.toml"],
            False,
        )
    )

    m = _base_manifest()
    m["slots"][3]["builder"] = "scripts/builders/docs_rust.sh"
    probes.append(("absent slot carrying a builder fails", m, good_files, False))

    m = _base_manifest()
    m["slots"][1]["default_path"] = "docs/api/c"
    probes.append(("default_path outside build/ fails", m, good_files, False))

    m = _base_manifest()
    m["site_root"] = "docs/site"
    probes.append(("committed site_root fails", m, good_files, False))

    failures = 0
    for name, manifest, files, want_ok in probes:
        errors = validate(manifest, files)
        ok = not errors
        if ok != want_ok:
            failures += 1
            print(f"SELFTEST FAIL: {name}: {errors}")
        else:
            print(f"  ok  {name}")
    total = len(probes)
    print(f"selftest: {total - failures}/{total} probes passed")
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="validate the manifest (default)")
    parser.add_argument("--selftest", action="store_true", help="run the built-in probes")
    parser.add_argument("--emit-plan", action="store_true", help="US-separated build plan for docs_site.sh")
    parser.add_argument("--manifest", default=str(MANIFEST))
    args = parser.parse_args()

    if args.selftest:
        return selftest()

    manifest_path = Path(args.manifest)
    if not manifest_path.is_file():
        print(f"check_docs_site_slots: {manifest_path} not found", file=sys.stderr)
        return 1
    manifest = load_manifest(manifest_path)

    if args.emit_plan:
        errors = validate(manifest, tracked_files(REPO_ROOT))
        if errors:
            for err in errors:
                print(f"check_docs_site_slots: {err}", file=sys.stderr)
            return 1
        print(emit_plan(manifest))
        return 0

    errors = validate(manifest, tracked_files(REPO_ROOT))
    if errors:
        print("check_docs_site_slots: the documentation site manifest is out of date")
        for err in errors:
            print(f"  - {err}")
        return 1

    slots = manifest["slots"]
    wired = [s["id"] for s in slots if s["state"] == "wired"]
    pending = [s["id"] for s in slots if s["state"] == "pending"]
    absent = [s["id"] for s in slots if s["state"] == "absent"]
    print(
        "check_docs_site_slots: OK "
        f"({len(wired)} wired: {', '.join(wired) or 'none'}; "
        f"{len(pending)} pending: {', '.join(pending) or 'none'}; "
        f"{len(absent)} absent: {', '.join(absent) or 'none'})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
