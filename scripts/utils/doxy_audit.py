#!/usr/bin/env python3
"""Doxygen documentation gap auditor for ra8d2-firmware.

Walks every .c/.h under libs/, src/, port/ (excluding libs/third_party/),
locates every function definition/prototype, and checks the immediately
preceding Doxygen block for the required tags listed in CLAUDE.md
("Doxygen Documentation Requirements").

Output:
  - docs/DOXYGEN_GAPS.csv  full row-per-function table
  - docs/DOXYGEN_GAPS.md   human-readable summary

Audit-only: never edits source files.
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path
from collections import Counter, defaultdict

REPO_ROOT = Path(__file__).resolve().parents[2]
SCAN_DIRS = ["libs", "src", "port"]
EXCLUDE_PARTS = {"third_party", "build", ".git"}

# Required tags for every function (per CLAUDE.md)
REQUIRED_SCALAR = ["@brief", "@details", "@return", "@retval", "@note", "@since"]
# @pre and @post require a minimum of 2 each (NASA Rule 5)
REQUIRED_MIN2 = ["@pre", "@post"]

FUNC_RE = re.compile(
    # return-type tokens (allow pointers, qualifiers, attributes)
    r"^[ \t]*"
    r"(?P<ret>(?:(?:static|inline|extern|const|volatile|register|signed|unsigned|"
    r"struct|union|enum|__attribute__\s*\(\([^)]*\)\)|[A-Za-z_][A-Za-z_0-9]*)\s+|\*\s*)+)"
    r"(?P<name>[A-Za-z_][A-Za-z_0-9]*)\s*"
    r"\((?P<args>[^;{]*)\)\s*"
    r"(?:__attribute__\s*\(\([^)]*\)\)\s*)?"
    r"(?P<term>[{;])",
    re.MULTILINE,
)

# Things that look like function decls but aren't
NON_FUNC_NAMES = {
    "if", "for", "while", "switch", "return", "sizeof", "typeof",
    "do", "else", "case", "goto", "static_assert", "_Static_assert",
    "alignof", "_Alignof", "defined",
}


def strip_comments(src: str) -> str:
    """Remove block & line comments without changing line numbers."""
    out = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        # preserve string literals
        if c == '"' or c == "'":
            quote = c
            out.append(c)
            i += 1
            while i < n:
                ch = src[i]
                out.append(ch)
                if ch == "\\" and i + 1 < n:
                    out.append(src[i + 1])
                    i += 2
                    continue
                i += 1
                if ch == quote:
                    break
            continue
        if c == "/" and i + 1 < n:
            nxt = src[i + 1]
            if nxt == "/":
                # line comment, keep newlines
                while i < n and src[i] != "\n":
                    i += 1
                continue
            if nxt == "*":
                i += 2
                while i + 1 < n and not (src[i] == "*" and src[i + 1] == "/"):
                    if src[i] == "\n":
                        out.append("\n")
                    i += 1
                i += 2
                continue
        out.append(c)
        i += 1
    return "".join(out)


def find_preceding_doxy(src: str, func_offset: int):
    """Return (block_text, has_block) for the doxygen block immediately
    preceding func_offset, or ("", False) if none."""
    # walk backward over whitespace
    j = func_offset - 1
    while j >= 0 and src[j] in " \t\n\r":
        j -= 1
    if j < 1 or src[j] != "/" or src[j - 1] != "*":
        return "", False
    # find start of block
    end = j + 1
    k = j - 2
    while k >= 1 and not (src[k] == "/" and src[k + 1] == "*"):
        k -= 1
    if k < 0:
        return "", False
    block = src[k:end]
    # Doxygen blocks start with /** or /*!
    if not (block.startswith("/**") or block.startswith("/*!")):
        return "", False
    return block, True


def parse_args(args_text: str):
    """Return list of parameter names. (void) -> []."""
    s = args_text.strip()
    if s == "" or s == "void":
        return []
    # strip nested attributes
    s = re.sub(r"__attribute__\s*\(\([^)]*\)\)", "", s)
    params = []
    depth = 0
    cur = []
    for ch in s:
        if ch == "(" or ch == "[" or ch == "{":
            depth += 1
            cur.append(ch)
        elif ch == ")" or ch == "]" or ch == "}":
            depth -= 1
            cur.append(ch)
        elif ch == "," and depth == 0:
            params.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if cur:
        params.append("".join(cur).strip())

    names = []
    for p in params:
        if not p or p == "void":
            continue
        if p == "...":
            continue
        # remove default-ish noise; strip array brackets
        p2 = re.sub(r"\[[^\]]*\]", "", p).strip()
        # function pointer: extract (*name)
        fp = re.search(r"\(\s*\*\s*([A-Za-z_][A-Za-z_0-9]*)\s*\)", p2)
        if fp:
            names.append(fp.group(1))
            continue
        # last identifier token is the name
        toks = re.findall(r"[A-Za-z_][A-Za-z_0-9]*", p2)
        if not toks:
            continue
        # filter out type keywords if it's the only one
        if len(toks) == 1 and toks[0] in {
            "int", "char", "short", "long", "float", "double", "void",
            "signed", "unsigned", "bool", "size_t", "ssize_t",
        }:
            # unnamed parameter, count as positional
            names.append(f"arg{len(names)}")
            continue
        names.append(toks[-1])
    return names


def is_returning_void(ret: str) -> bool:
    r = ret.strip()
    # collapse whitespace
    r = re.sub(r"\s+", " ", r)
    # strip qualifiers
    r = re.sub(r"\b(static|inline|extern|__attribute__\(\([^)]*\)\))\b", "", r).strip()
    # pointer return is not void
    if "*" in r:
        return False
    # exact "void"
    return r == "void" or r.endswith(" void")


def audit_file(path: Path):
    try:
        raw = path.read_text(encoding="utf-8", errors="replace")
    except Exception:
        return []
    src_no_comments = strip_comments(raw)

    rows = []
    for m in FUNC_RE.finditer(src_no_comments):
        name = m.group("name")
        if name in NON_FUNC_NAMES:
            continue
        ret = m.group("ret")
        args = m.group("args")
        # filter out things like "return foo(x);" patterns
        if "return" in ret.split():
            continue
        # filter typedef/struct lines
        if re.search(r"\btypedef\b", ret):
            continue
        # Reject call expressions misparsed as declarations.
        # 1) An assignment target on the LHS: the args group spilled past the
        #    real closing paren and now contains '=' or an extra ')'. This is
        #    the `*foo(off) = bar;` / `*foo() = ...;` shape used heavily by
        #    inline-accessor register writes.
        if "=" in args or ")" in args:
            continue
        # 2) Return-type token is *only* a dereference (`*` or `&`) with no
        #    real type name -- those are call expressions, not declarations.
        ret_stripped = ret.strip()
        if ret_stripped in ("*", "&") or re.fullmatch(r"[*&\s]+", ret_stripped):
            continue
        # 3) Full matched text starting with `*name(` or `&name(` is a call
        #    expression (deref/address-of of an inline accessor return).
        full = m.group(0).lstrip()
        if full.startswith("*") or full.startswith("&"):
            continue
        # skip definitions of macros (shouldn't appear since stripped, but safety)
        # determine the line number in original file
        # m.start() is an offset into src_no_comments; since strip_comments
        # preserves newlines, line numbers in stripped == line numbers in raw.
        line_no = src_no_comments.count("\n", 0, m.start()) + 1

        args = parse_args(args)

        # Get the doxy block from the *original* source so comments are present
        # We need the offset in raw. Use line_no to approximate.
        # Find the function start in raw by line-based search.
        raw_lines = raw.splitlines(keepends=True)
        if line_no - 1 >= len(raw_lines):
            continue
        offset_in_raw = sum(len(l) for l in raw_lines[: line_no - 1])
        block, has_block = find_preceding_doxy(raw, offset_in_raw)

        missing = []

        if not has_block:
            # Treat as everything missing
            missing.append("@brief")
            missing.append("@details")
            for a in args:
                missing.append(f"@param[{a}]")
            if not is_returning_void(ret):
                missing.append("@return")
                missing.append("@retval")
            missing.append("@pre")
            missing.append("@post")
            missing.append("@note")
            missing.append("@since")
        else:
            # @brief
            if "@brief" not in block:
                missing.append("@brief")
            if "@details" not in block:
                missing.append("@details")
            # @param[in/out/in,out] <name>
            for a in args:
                # match @param[...] name OR @param name (any direction)
                pat = re.compile(
                    r"@param(?:\s*\[[^\]]*\])?\s+" + re.escape(a) + r"\b"
                )
                if not pat.search(block):
                    missing.append(f"@param[{a}]")
            # @return / @retval (only if non-void)
            if not is_returning_void(ret):
                if "@return" not in block and "@returns" not in block:
                    missing.append("@return")
                if "@retval" not in block:
                    missing.append("@retval")
            # @pre/@post require >=2
            n_pre = len(re.findall(r"@pre\b", block))
            n_post = len(re.findall(r"@post\b", block))
            if n_pre < 2:
                missing.append(f"@pre(<2:{n_pre})")
            if n_post < 2:
                missing.append(f"@post(<2:{n_post})")
            if "@note" not in block:
                missing.append("@note")
            if "@since" not in block:
                missing.append("@since")

        if not missing:
            rows.append((str(path.relative_to(REPO_ROOT)), line_no, name, [], "ok"))
            continue

        # severity
        has_brief_or_param_miss = any(
            t == "@brief" or t.startswith("@param[") for t in missing
        )
        if has_brief_or_param_miss:
            severity = "high"
        elif any(
            t in ("@return", "@retval") or t.startswith("@pre") or t.startswith("@post")
            for t in missing
        ):
            severity = "medium"
        else:
            severity = "low"

        rows.append(
            (str(path.relative_to(REPO_ROOT)), line_no, name, missing, severity)
        )

    return rows


def run_check() -> int:
    """Strict gate: exit 0 if zero gaps, else exit 1 with offender list.

    Used by the pre-commit hook to keep documentation coverage at 100%.
    Does not write CSV/MD outputs -- read-only audit.
    """
    all_rows = []
    for top in SCAN_DIRS:
        root = REPO_ROOT / top
        if not root.is_dir():
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in EXCLUDE_PARTS]
            for fn in filenames:
                if not (fn.endswith(".c") or fn.endswith(".h")):
                    continue
                p = Path(dirpath) / fn
                rel_parts = p.relative_to(REPO_ROOT).parts
                if any(part in EXCLUDE_PARTS for part in rel_parts):
                    continue
                all_rows.extend(audit_file(p))

    gap_rows = [r for r in all_rows if r[3]]
    if not gap_rows:
        print("doxy_audit --check: gaps=0 (PASS)")
        return 0

    print(f"doxy_audit --check: gaps={len(gap_rows)} (FAIL)")
    print("Offending functions (file:line  function  -- missing tags):")
    # cap output to 50 lines so the hook stays readable
    cap = 50
    for src, line, name, missing, _sev in gap_rows[:cap]:
        print(f"  {src}:{line}  {name}  --  {';'.join(missing)}")
    if len(gap_rows) > cap:
        print(f"  ... and {len(gap_rows) - cap} more")
    print("")
    print("Refresh the audit report by running:")
    print("  python3 scripts/utils/doxy_audit.py")
    return 1


def main() -> int:
    if "--check" in sys.argv[1:]:
        return run_check()

    all_rows = []
    for top in SCAN_DIRS:
        root = REPO_ROOT / top
        if not root.is_dir():
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            # prune excluded dirs
            dirnames[:] = [d for d in dirnames if d not in EXCLUDE_PARTS]
            for fn in filenames:
                if not (fn.endswith(".c") or fn.endswith(".h")):
                    continue
                p = Path(dirpath) / fn
                rel_parts = p.relative_to(REPO_ROOT).parts
                if any(part in EXCLUDE_PARTS for part in rel_parts):
                    continue
                all_rows.extend(audit_file(p))

    # sort by file path then line
    all_rows.sort(key=lambda r: (r[0], r[1]))

    total_funcs = len(all_rows)
    gap_rows = [r for r in all_rows if r[3]]
    total_gaps = len(gap_rows)
    total_missing_tags = sum(len(r[3]) for r in gap_rows)

    # write CSV
    csv_path = REPO_ROOT / "docs" / "DOXYGEN_GAPS.csv"
    with csv_path.open("w", encoding="ascii") as f:
        f.write("source_file,line,function_name,missing_tags,severity\n")
        for src, line, name, missing, sev in gap_rows:
            f.write(f"{src},{line},{name},{';'.join(missing)},{sev}\n")

    # tag frequency (collapse @param[*] -> @param, @pre(<2:n) -> @pre, etc.)
    tag_counter = Counter()
    for r in gap_rows:
        for t in r[3]:
            if t.startswith("@param["):
                tag_counter["@param"] += 1
            elif t.startswith("@pre"):
                tag_counter["@pre"] += 1
            elif t.startswith("@post"):
                tag_counter["@post"] += 1
            else:
                tag_counter[t] += 1

    # per-file gap counts
    file_counter = Counter(r[0] for r in gap_rows)

    # per-module (top-level dir within libs/, src/, port/)
    def module_of(path: str) -> str:
        parts = path.split("/")
        if len(parts) >= 2:
            return f"{parts[0]}/{parts[1]}"
        return parts[0]

    module_counter = Counter(module_of(r[0]) for r in gap_rows)

    # write Markdown
    md_path = REPO_ROOT / "docs" / "DOXYGEN_GAPS.md"
    lines = []
    lines.append("# Doxygen Documentation Gap Report")
    lines.append("")
    lines.append("Audit-only report generated by `scripts/utils/doxy_audit.py` against the")
    lines.append("Doxygen Documentation Requirements in `CLAUDE.md`. Scope: `libs/`,")
    lines.append("`src/`, `port/` (excludes `libs/third_party/`).")
    lines.append("")
    lines.append("## Summary")
    lines.append("")
    lines.append(f"- Total functions audited: {total_funcs}")
    lines.append(f"- Functions with gaps: {total_gaps}")
    lines.append(f"- Total missing-tag instances: {total_missing_tags}")
    lines.append("")
    lines.append("## Most-frequently-missing tags")
    lines.append("")
    lines.append("| Tag | Count |")
    lines.append("|-----|-------|")
    for tag, cnt in tag_counter.most_common():
        lines.append(f"| `{tag}` | {cnt} |")
    lines.append("")
    lines.append("## Worst 10 modules by gap count")
    lines.append("")
    lines.append("| Module | Functions with gaps |")
    lines.append("|--------|---------------------|")
    for mod, cnt in module_counter.most_common(10):
        lines.append(f"| `{mod}` | {cnt} |")
    lines.append("")
    lines.append("## Top 30 files by gap count")
    lines.append("")
    lines.append("| File | Functions with gaps |")
    lines.append("|------|---------------------|")
    for fn, cnt in file_counter.most_common(30):
        lines.append(f"| `{fn}` | {cnt} |")
    lines.append("")
    lines.append("## Severity legend")
    lines.append("")
    lines.append("- `high`: missing `@brief` or any `@param[...]`")
    lines.append("- `medium`: missing `@return` / `@retval` / `@pre` / `@post`")
    lines.append("- `low`: only optional / informational tags missing (`@note`, `@since`, ...)")
    lines.append("")
    lines.append("See `docs/DOXYGEN_GAPS.csv` for the full row-by-row data.")
    lines.append("")
    lines.append("## Audit history")
    lines.append("")
    lines.append("| Date | Functions with gaps | Missing-tag instances |")
    lines.append("|------|---------------------|-----------------------|")
    lines.append("| 2026-05-02 (original)                 | 2557 | 20328 |")
    lines.append("| 2026-05-02 (refresh)                  | 663  | 4935  |")
    lines.append(f"| 2026-05-02 (auditor false-pos fix)    | {total_gaps} | {total_missing_tags} |")
    lines.append("")

    # cap at 200 lines if necessary
    if len(lines) > 200:
        lines = lines[:200]

    md_path.write_text("\n".join(lines), encoding="ascii")

    print(f"functions={total_funcs} gaps={total_gaps} missing_tags={total_missing_tags}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
