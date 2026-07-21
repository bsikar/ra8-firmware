#!/usr/bin/env python3
"""Per-app size visualizer for ra8-firmware.

Walks every examples/ek_ra8d2/<app>/build/<app>.elf, runs
`arm-none-eabi-size --format=sysv` on it, parses the output, and
emits two markdown tables to stdout:

  1. A combined sortable summary table (one row per app) with
     text / data / bss / total columns, sorted by total size.
  2. A per-app section breakdown (only when --verbose is passed).

Optionally writes the rendered output to docs/APP_SIZES.md when
`--write` is passed. If no .elf files are found anywhere under
examples/ek_ra8d2/, the script prints a build hint and exits 0
(so it never blocks make all / CI).

Usage:

    python3 scripts/report/app_sizes.py
    python3 scripts/report/app_sizes.py --write
    python3 scripts/report/app_sizes.py --verbose

Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
APPS_DIR = REPO_ROOT / "examples" / "ek_ra8d2"
DEFAULT_OUT = REPO_ROOT / "docs" / "APP_SIZES.md"
SIZE_TOOL = "arm-none-eabi-size"

# Section -> bucket. Anything matching .text* lands in "text", and
# so on. .debug_* / .ARM.attributes / .comment are dropped from the
# headline numbers because they do not consume on-target memory.
TEXT_PREFIXES = (
    ".text",
    ".vectors",
    ".rodata",
    ".init",
    ".fini",
    ".gnu.sgstubs",
    ".ARM.exidx",
    ".ARM.extab",
    ".init_array",
    ".fini_array",
    ".preinit_array",
)
DATA_PREFIXES = (".data", ".sdata", ".tdata")
BSS_PREFIXES = (".bss", ".sbss", ".tbss", ".stack_canary", ".heap", ".stack", ".noinit")
DROP_PREFIXES = (
    ".debug_",
    ".ARM.attributes",
    ".comment",
    ".symtab",
    ".strtab",
    ".shstrtab",
    ".option_setting_",
)


@dataclass
class AppSizes:
    """Aggregated size buckets for one application ELF."""

    name: str
    elf: Path
    text: int = 0
    data: int = 0
    bss: int = 0
    sections: list[tuple] = field(default_factory=list)

    @property
    def total(self) -> int:
        """Sum of text + data + bss in bytes (on-target footprint)."""
        return self.text + self.data + self.bss


def find_size_tool() -> str | None:
    """Locate arm-none-eabi-size on PATH."""
    return shutil.which(SIZE_TOOL)


def collect_elfs() -> list[Path]:
    """Every built app ELF currently on disk.

    Reports what HAS been built rather than what could be: an app that was
    never compiled is simply absent from the size table, not zero-sized.
    """
    out: list[Path] = []
    if not APPS_DIR.is_dir():
        return out
    for app_dir in sorted(APPS_DIR.iterdir()):
        if not app_dir.is_dir():
            continue
        elf = app_dir / "build" / f"{app_dir.name}.elf"
        if elf.is_file():
            out.append(elf)
    return out


_MIN_SYSV_PARTS = 2  # arm-none-eabi-size sysv output has at least "name size" columns
_BYTES_PER_KIB = 1024  # binary kilobyte


def run_size(size_tool: str, elf: Path) -> AppSizes:
    """Bucket one ELF's sections into text / data / bss totals.

    Uses the sysv format because the default Berkeley output collapses
    sections into fixed columns; sysv lists each section by name, which is
    what allows the buckets to be assigned by name rather than by position.
    """
    result = subprocess.run(  # noqa: S603  # trusted: size_tool comes from shutil.which
        [size_tool, "--format=sysv", str(elf)],
        check=True,
        capture_output=True,
        text=True,
    )
    sizes = AppSizes(name=elf.parent.parent.name, elf=elf)
    for raw_line in result.stdout.splitlines():
        line = raw_line.strip()
        if not line or line.startswith(("section", elf.name)):
            continue
        if line.startswith("Total"):
            continue
        parts = line.split()
        if len(parts) < _MIN_SYSV_PARTS:
            continue
        section = parts[0]
        try:
            size = int(parts[1])
        except ValueError:
            continue
        if any(section.startswith(p) for p in DROP_PREFIXES):
            continue
        sizes.sections.append((section, size))
        if any(section.startswith(p) for p in TEXT_PREFIXES):
            sizes.text += size
        elif any(section.startswith(p) for p in DATA_PREFIXES):
            sizes.data += size
        elif any(section.startswith(p) for p in BSS_PREFIXES):
            sizes.bss += size
        # Otherwise: silently ignored (option_setting_*, etc.).
    return sizes


def fmt_bytes(n: int) -> str:
    """Return a humanised byte count: `1234` -> `1234 (1.2 KiB)`."""
    if n < _BYTES_PER_KIB:
        return f"{n}"
    return f"{n} ({n / _BYTES_PER_KIB:.1f} KiB)"


def render_summary(rows: list[AppSizes]) -> str:
    """Render the sorted-by-total combined summary table."""
    rows = sorted(rows, key=lambda r: r.total, reverse=True)
    lines = [
        "## Combined summary (sorted by total)",
        "",
        "| App | text | data | bss | total |",
        "|-----|-----:|-----:|----:|------:|",
    ]
    lines.extend(
        f"| `{r.name}` | {fmt_bytes(r.text)} | {fmt_bytes(r.data)} "
        f"| {fmt_bytes(r.bss)} | {fmt_bytes(r.total)} |"
        for r in rows
    )
    if rows:
        n = len(rows)
        smallest = min(rows, key=lambda r: r.total)
        largest = max(rows, key=lambda r: r.total)
        mean = sum(r.total for r in rows) / float(n)
        lines += [
            "",
            "### Stats",
            "",
            f"- Apps measured: **{n}**",
            f"- Smallest: `{smallest.name}` ({fmt_bytes(smallest.total)})",
            f"- Largest:  `{largest.name}` ({fmt_bytes(largest.total)})",
            f"- Mean total: {mean:.0f} bytes ({mean / 1024.0:.1f} KiB)",
        ]
    return "\n".join(lines)


def render_per_app(rows: list[AppSizes]) -> str:
    """Render per-app text/data/bss/total table block."""
    lines = ["## Per-app text/data/bss totals", ""]
    for r in sorted(rows, key=lambda r: r.name):
        lines += [
            f"### `{r.name}`",
            "",
            "| section | size |",
            "|---------|-----:|",
            f"| text  | {fmt_bytes(r.text)} |",
            f"| data  | {fmt_bytes(r.data)} |",
            f"| bss   | {fmt_bytes(r.bss)} |",
            f"| total | {fmt_bytes(r.total)} |",
            "",
            f"_ELF: `{r.elf.relative_to(REPO_ROOT)}`_",
            "",
        ]
    return "\n".join(lines)


def render_full(rows: list[AppSizes]) -> str:
    """Render the full markdown document."""
    head = (
        "# ra8-firmware -- per-application size report\n\n"
        "Auto-generated by `scripts/report/app_sizes.py`. Re-run via\n"
        "`make app-sizes`. Numbers come from "
        "`arm-none-eabi-size --format=sysv` on each\n"
        "`examples/ek_ra8d2/<app>/build/<app>.elf`.\n\n"
        "- `text`  -- combined .vectors / .text / .rodata / .gnu.sgstubs / "
        "  .init_array / .ARM.exidx (everything that lives in MRAM at "
        "  runtime).\n"
        "- `data`  -- combined .data / .sdata (initialized RAM).\n"
        "- `bss`   -- combined .bss / .stack_canary / .noinit (zero-init "
        "  RAM).\n"
        "- `total` -- text + data + bss (on-target footprint; "
        "  `.debug_*`, `.ARM.attributes`, and `.option_setting_*` are\n"
        "  excluded because they do not consume target memory).\n\n"
    )
    return head + render_summary(rows) + "\n\n" + render_per_app(rows) + "\n"


def main(argv: list[str] | None = None) -> int:
    """Entry point: collect ELFs, run size, render markdown."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--write", action="store_true", help=f"Also write the output to {DEFAULT_OUT}."
    )
    parser.add_argument(
        "--verbose", action="store_true", help="Print the per-app section breakdown too."
    )
    args = parser.parse_args(argv)

    size_tool = find_size_tool()
    if size_tool is None:
        print(
            f"{SIZE_TOOL} not found on PATH; skipping. "
            "(install arm-none-eabi-gnu-toolchain to run this report)",
            file=sys.stderr,
        )
        return 0

    elfs = collect_elfs()
    if not elfs:
        print("no examples/ek_ra8d2/<app>/build/<app>.elf found -- build with `make apps` first")
        return 0

    rows: list[AppSizes] = []
    for elf in elfs:
        try:
            rows.append(run_size(size_tool, elf))
        except subprocess.CalledProcessError as exc:  # noqa: PERF203  # one ELF failure must not abort the rest
            print(f"WARN: {SIZE_TOOL} failed for {elf}: {exc}", file=sys.stderr)

    rendered = render_full(rows)
    print(rendered)

    if args.write:
        DEFAULT_OUT.parent.mkdir(parents=True, exist_ok=True)
        DEFAULT_OUT.write_text(rendered, encoding="ascii")
        print(f"wrote {DEFAULT_OUT.relative_to(REPO_ROOT)}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
