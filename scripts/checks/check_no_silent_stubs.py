#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: no function shall exist only to satisfy the linker.

A *silent stub* is a function whose body does no work -- it discards its
arguments and hands back a canned answer -- yet whose presence lets the build
link and the program claim a capability it does not have.  The failure mode is
worse than a missing feature: the tool links clean, advertises support, and
fails at runtime (or, worse, silently succeeds having done nothing).

``tools/rabook_imagepack/webp_stub.c`` and ``tools/media_dl/webp_stub.c`` were the
motivating case.  Each defined the real symbol
``ra8_jof_priv_webp_transcode()``, discarded both arguments and returned
``k_ra8_err_not_supported`` -- purely so the JOF producer would link
without compiling the vendored libwebp decoder.  A complete WebP decoder was
already vendored, wrapped, tested and fuzzed in this very repository; the tools
just were not compiling it.  Both tools shipped a WebP feature that could never
work.

Calibration
-----------
Most no-op bodies in this tree are *legitimate* -- an early revision of this
gate flagged 27 candidates of which 24 were correct by design:

  * platform alternatives (``tools/ra8_emulator/src/display/board_view_stub.c`` is the
    headless stand-in for the Cocoa window layer on Linux CI; it reports
    failure honestly so callers take their headless branch);
  * the fail-closed ``#else`` half of the placeholder-crypto guard, which
    ``check_stub_crypto_guarded.py`` *requires* to return a hard error;
  * callbacks matching a vtable / registry signature that genuinely have
    nothing to do (USBX activate / deactivate hooks, an empty ISR completion
    callback, a board_sim MMIO write handler for a deliberately inert
    peripheral);
  * MMIO read handlers that return module state rather than a constant.

A gate that fires on those is noise, and a noisy gate gets disabled -- which is
worse than no gate.  So this one does not ask "is the body empty?".  It asks
the two much narrower questions that separate the webp stubs from all 24:

Rule SHADOW
    A no-op body that provides a *second* definition of a symbol which is also
    defined for real elsewhere in first-party code.  This is always a defect:
    whichever definition the linker picks, one of them is a lie, and the build
    silently disables working code that exists in the tree.  This is exactly
    the webp case, and it has no legitimate form -- a platform alternative
    supplies the *only* definition in its build, never a competing one.

Rule CANNED
    A function that returns an explicit "unsupported / unimplemented" error
    constant, discards every parameter, and has no other statement.  Such a
    function claims an operation was attempted and refused, when in fact no
    implementation exists at all.  Callbacks returning ``void``/``bool``/a
    pointer, and handlers returning module state, are outside this rule by
    construction.

Hardware waiver
---------------
A capability whose *hardware does not physically exist yet* may legitimately
have no implementation.  Such a function is waived by carrying an explicit
marker naming the missing part, in the form CLAUDE.md mandates::

    TODO(ESP32-C6 radio module not yet on the bench): ...

The marker must name something -- a bare ``TODO`` or an empty ``TODO()`` is
rejected, so the waiver cannot become a catch-all.  It is keyed on the marker
and not on a filename pattern, so it waives exactly the function that carries
it and nothing else in the file.  Note that a waiver is *not* available to
Rule SHADOW: if a real implementation exists in the tree, the hardware plainly
exists too.

Run::

    check_no_silent_stubs.py             # scan the whole tree
    check_no_silent_stubs.py FILE ...    # scan listed files
    check_no_silent_stubs.py --selftest  # prove the detector both fires and
                                         # stays silent on the right inputs

Exit 0 if no silent stub is found, 1 otherwise.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

# First-party roots. libs/third_party (SOUP) and libs/fonts (generated) are out
# of scope, matching every other repo gate.
ROOTS = ("libs", "src", "tools", "examples", "port")
EXCLUDED = ("libs/third_party", "libs/fonts")

# Error constants that mean "this operation has no implementation". A function
# whose entire body hands one of these back, having discarded its arguments,
# has not refused an operation -- it never had one.
CANNED_ERRORS = frozenset(
    {
        "k_ra8_err_not_supported",
        "k_ra8_err_not_implemented",
        "k_ra8_err_unsupported",
        "k_ra8_err_unimplemented",
    }
)

# A waiver must name the missing hardware: TODO(<something>). Bare TODO or
# TODO() is deliberately not accepted.
WAIVER_RE = re.compile(r"TODO\(\s*([^)]*?)\s*\)")

# Keywords that can precede a parenthesised block but are not function
# definitions.
NOT_A_FUNCTION = frozenset(
    {"if", "for", "while", "switch", "return", "sizeof", "do", "else", "catch"}
)

# A function definition: an optional return type, a name, a parameter list with
# no nested braces or semicolons, then an opening brace. Newlines are allowed
# between the parameter list and the brace -- the house style puts the brace of
# a definition on its own line, and missing that made an early revision of this
# detector report zero findings on a file that was a known stub.
FUNCTION_RE = re.compile(
    r"(?:^|\n)[ \t]*(?:[A-Za-z_][\w \t\*\n]*?)\b(\w+)[ \t\n]*\(([^;{}]*)\)[ \t\n]*\{",
    re.DOTALL,
)

DISCARD_RE = re.compile(r"\(\s*void\s*\)\s*(\w+)\Z")
RETURN_CONST_RE = re.compile(r"return\s+([A-Za-z_]\w*|-?\d+|nullptr|NULL|true|false)\Z")
PARAM_NAME_RE = re.compile(r"(\w+)\s*(?:\[\s*\])?\s*\Z")


def blank_noncode(text: str) -> str:
    """Blank out comments and string/char literals, preserving offsets.

    Offsets are preserved (and newlines kept) so that reported line numbers and
    brace matching still refer to the original file.
    """
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append(re.sub(r"[^\n]", " ", text[i:end]))
            i = end
        elif c == "/" and i + 1 < n and text[i + 1] == "/":
            end = text.find("\n", i)
            end = n if end < 0 else end
            out.append(" " * (end - i))
            i = end
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def body_span(text: str, brace_idx: int) -> tuple[int, int] | None:
    """Return the (start, end) offsets of a function body by brace matching."""
    depth = 0
    for i in range(brace_idx, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return (brace_idx + 1, i)
    return None


def param_names(params: str) -> set[str]:
    """Extract the declared parameter NAMES from a C parameter list.

    Feeds the discard test in ``classify_body``, which has to know whether a
    ``(void)x;`` names a real parameter or some unrelated local -- so what is
    wanted here is the identifiers, with the types thrown away.

    ``void`` as the whole list yields the empty set rather than a name, which
    is what makes a zero-parameter function fall out as "discards everything
    it was given" and stay eligible for the CANNED rule.
    """
    names: set[str] = set()
    for raw_part in params.split(","):
        part = raw_part.strip()
        if not part or part == "void":
            continue
        m = PARAM_NAME_RE.search(part)
        if m:
            names.add(m.group(1))
    return names


def classify_body(body: str, params: str) -> str | None:
    """Return the returned constant if the body is a pure discard-and-return.

    Returns None when the body does real work. A body qualifies only when every
    statement is either a ``(void)param;`` discard or a single ``return
    <constant>;``, AND at least one discarded name is an actual parameter --
    that last condition is what separates "ignores its inputs" from a handler
    that legitimately returns module state.
    """
    statements = [s.strip() for s in body.split(";") if s.strip()]
    if not statements:
        return None  # a genuinely empty body is a callback shape, not a stub
    discards: list[str] = []
    returns: list[str] = []
    for stmt in statements:
        m = DISCARD_RE.match(stmt)
        if m:
            discards.append(m.group(1))
            continue
        m = RETURN_CONST_RE.match(stmt)
        if m:
            returns.append(m.group(1))
            continue
        return None  # any other statement means real work
    if len(returns) != 1:
        return None
    declared = param_names(params)
    # Takes parameters: at least one must actually be discarded. That is what
    # separates "ignores its inputs" from a handler that legitimately returns
    # module state.
    if declared and not (set(discards) & declared):
        return None
    # Takes NO parameters, so "discards every parameter" is vacuously true and
    # the discard test above cannot speak. Only a canned-error return qualifies
    # here: `ra8_widget_calibrate(void) { return k_ra8_err_not_supported; }` is
    # every bit the stub its one-argument form is, but a bare `return s_state;`
    # getter is module state and must stay silent. Requiring an empty discard
    # list keeps a body that pokes at file-scope names out of the "pure canned
    # return" shape.
    if not declared and (discards or returns[0] not in CANNED_ERRORS):
        return None
    return returns[0]


def waiver_in(text: str) -> str | None:
    """Return the named missing dependency from a TODO(...) marker, if any."""
    for m in WAIVER_RE.finditer(text):
        named = m.group(1).strip()
        if named:
            return named
    return None


def preceding_doc(text: str, start: int) -> str:
    """Return the ~40 lines before a definition (its comment block)."""
    head = text[:start]
    return "\n".join(head.splitlines()[-40:])


def in_failclosed_crypto_guard(raw: str, offset: int) -> bool:
    """True if the offset sits in the #else half of the placeholder-crypto guard.

    ``check_stub_crypto_guarded.py`` REQUIRES those bodies to return a hard
    error so a production image cannot ship fake crypto. Firing on them would
    pit one gate against another.
    """
    guard = "defined(RA8_INSECURE_STUB_CRYPTO)"
    if guard not in raw[:offset]:
        return False
    idx = raw.rfind(guard, 0, offset)
    between = raw[idx:offset]
    return "#else" in between and between.count("#endif") == 0


def scan_text(raw: str, path: str) -> list[dict]:
    """Find discard-and-return functions in one translation unit."""
    code = blank_noncode(raw)
    found: list[dict] = []
    for m in FUNCTION_RE.finditer(code):
        name, params = m.group(1), m.group(2)
        if name in NOT_A_FUNCTION:
            continue
        span = body_span(code, m.end() - 1)
        if span is None:
            continue
        returned = classify_body(code[span[0] : span[1]], params)
        if returned is None:
            continue
        if in_failclosed_crypto_guard(raw, m.start()):
            continue
        region = preceding_doc(raw, m.start()) + raw[m.start() : span[1]]
        found.append(
            {
                "path": path,
                "line": code[: m.start()].count("\n") + 1,
                "name": name,
                "returns": returned,
                "waiver": waiver_in(region),
            }
        )
    return found


def real_definitions(files: list[Path]) -> dict[str, list[str]]:
    """Map symbol -> files defining it with a body that does real work."""
    defs: dict[str, list[str]] = {}
    for path in files:
        raw = path.read_text(errors="replace")
        code = blank_noncode(raw)
        for m in FUNCTION_RE.finditer(code):
            name = m.group(1)
            if name in NOT_A_FUNCTION:
                continue
            span = body_span(code, m.end() - 1)
            if span is None:
                continue
            if classify_body(code[span[0] : span[1]], m.group(2)) is None:
                defs.setdefault(name, []).append(str(path))
    return defs


def first_party_sources(explicit: list[str]) -> list[Path]:
    """Enumerate the tracked first-party .c files under ROOTS.

    Tracked, not globbed. A bare rglob also sweeps in build output -- every
    configured app leaves a CMake compiler-probe TU at
    ``<app>/build/CMakeFiles/*/CompilerIdC/CMakeCCompilerId.c`` -- so the set
    scanned depended on whether the caller had built, and generated code got
    held to a first-party rule. CI never saw it (it runs against a clean
    ``git archive HEAD`` snapshot) but the pre-commit hook runs in the working
    tree, which is exactly where a spurious finding costs the most trust.
    ``git ls-files`` is also what the copyright and @since gates enumerate with.
    """
    if explicit:
        return [Path(p) for p in explicit]
    try:
        pathspec = [f"{root}/**/*.c" for root in ROOTS]
        listed = subprocess.run(  # noqa: S603  # fixed argv, no shell
            ["git", "ls-files", "-z", "--", *pathspec],  # noqa: S607  # trusted: fixed git argv
            capture_output=True,
            text=True,
            check=True,
        ).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        sys.exit(
            f"check_no_silent_stubs.py: FATAL -- cannot list tracked sources: {exc}\n"
            "  This gate enumerates via git and must not fall back to a glob:\n"
            "  a glob silently scans build output and changes the verdict."
        )
    return [
        Path(name)
        for name in listed.split("\0")
        if name and not any(name.startswith(x) for x in EXCLUDED)
    ]


def analyse(files: list[Path]) -> list[tuple[str, dict]]:
    """Return (rule, finding) violations across the given files."""
    defs = real_definitions(files)
    violations: list[tuple[str, dict]] = []
    for path in files:
        for f in scan_text(path.read_text(errors="replace"), str(path)):
            # Rule SHADOW: a real definition of the same symbol exists elsewhere.
            shadowed = [d for d in defs.get(f["name"], []) if d != f["path"]]
            if shadowed:
                f["shadows"] = shadowed
                violations.append(("SHADOW", f))
                continue
            # Rule CANNED: claims an operation was refused when none exists.
            if f["returns"] in CANNED_ERRORS and not f["waiver"]:
                violations.append(("CANNED", f))
    return violations


SELFTEST_CASES: list[tuple[str, str, bool, str]] = [
    (
        "shadowing stub (the webp case)",
        """
        ra8_err_t real_thing(state_t* st, pull_t* pfx)
        {
          st->count += 1;
          return do_work(st, pfx);
        }
        """,
        False,
        "real.c",
    ),
    (
        "shadowing stub (the webp case)",
        """
        ra8_err_t real_thing(state_t* st, pull_t* pfx)
        {
          (void)st;
          (void)pfx;
          return k_ra8_err_not_supported;
        }
        """,
        True,
        "stub.c",
    ),
    (
        "canned unsupported with no implementation anywhere",
        """
        ra8_err_t lonely_feature(uint32_t mask)
        {
          (void)mask;
          return k_ra8_err_not_supported;
        }
        """,
        True,
        "canned.c",
    ),
    (
        "hardware-blocked, waived by a named TODO",
        """
        /* TODO(ESP32-C6 radio module has been ordered but has not arrived) */
        ra8_err_t wifi_connect(const char* ssid)
        {
          (void)ssid;
          return k_ra8_err_not_supported;
        }
        """,
        False,
        "waived.c",
    ),
    (
        "bare TODO is not a waiver",
        """
        /* TODO: wire this up later */
        ra8_err_t someday(uint32_t x)
        {
          (void)x;
          return k_ra8_err_not_supported;
        }
        """,
        True,
        "bare_todo.c",
    ),
    (
        "thin wrapper delegating to a real call",
        """
        ra8_err_t wrapper(uint8_t* buf, size_t len)
        {
          return backend_write(buf, len);
        }
        """,
        False,
        "wrapper.c",
    ),
    (
        "platform alternative reporting failure honestly",
        """
        view_t* board_view_open(uint16_t w, uint16_t h, const char* title)
        {
          (void)w;
          (void)h;
          (void)title;
          return nullptr;
        }
        """,
        False,
        "platform.c",
    ),
    (
        "intentionally empty ISR / vtable callback",
        """
        static void on_complete(void* ctx, uint16_t status)
        {
          (void)ctx;
          (void)status;
        }
        """,
        False,
        "callback.c",
    ),
    (
        "MMIO read handler returning module state",
        """
        static uint64_t reset1_read(uc_engine* uc, uint64_t addr, unsigned size)
        {
          (void)uc;
          (void)addr;
          (void)size;
          return s_rstsr1;
        }
        """,
        False,
        "mmio.c",
    ),
    (
        "zero-parameter canned return is still a stub",
        """
        ra8_err_t ra8_widget_calibrate(void)
        {
          return k_ra8_err_not_supported;
        }
        """,
        True,
        "zero_param.c",
    ),
    (
        "zero-parameter getter returning module state",
        """
        uint32_t ra8_time_ms(void)
        {
          return s_tick_ms;
        }
        """,
        False,
        "getter.c",
    ),
    (
        "fail-closed half of the placeholder-crypto guard",
        """
        #if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_SIMULATOR_MODE)
        ra8_err_t ra8_rsip_tamper_enable(uint32_t sources)
        {
          return simulate(sources);
        }
        #else
        ra8_err_t ra8_rsip_tamper_enable(uint32_t sources)
        {
          (void)sources;
          return k_ra8_err_not_supported;
        }
        #endif
        """,
        False,
        "crypto.c",
    ),
]


def selftest(tmp: Path) -> int:
    """Assert the detector fires on stubs and stays silent on legitimate code."""
    failures: list[str] = []
    groups: dict[str, list[tuple[str, str, bool]]] = {}
    for label, body, should_fire, fname in SELFTEST_CASES:
        groups.setdefault(label, []).append((body, fname, should_fire))

    for label, cases in groups.items():
        d = tmp / re.sub(r"\W+", "_", label)
        d.mkdir(parents=True, exist_ok=True)
        files = []
        expect: dict[str, bool] = {}
        for body, fname, should_fire in cases:
            p = d / fname
            p.write_text(body)
            files.append(p)
            expect[str(p)] = should_fire
        fired = {v[1]["path"] for v in analyse(files)}
        for path, should_fire in expect.items():
            did = path in fired
            if did != should_fire:
                verb = "did not fire" if should_fire else "fired"
                failures.append(f"  {label} [{Path(path).name}]: gate {verb} (unexpected)")

    if failures:
        print("check_no_silent_stubs.py --selftest: FAILED\n", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1
    total = len(SELFTEST_CASES)
    fires = sum(1 for c in SELFTEST_CASES if c[2])
    print(
        f"check_no_silent_stubs.py --selftest: PASS "
        f"({total} cases: {fires} must fire, {total - fires} must stay silent)"
    )
    return 0


def _scan_set_is_usable(files: list[Path]) -> bool:
    """Whether the resolved scan set can be trusted; prints FATAL when not.

    Fail loudly rather than silently passing on a broken scan: a gate that
    reports success because it looked at nothing is worse than no gate. The
    roots resolve relative to the current directory, so running this from
    anywhere but the repository root finds nothing.
    """
    if not files:
        print(
            "check_no_silent_stubs.py: FATAL -- no first-party sources found.\n"
            f"Expected .c files under {', '.join(ROOTS)} relative to the current\n"
            "directory. Run this from the repository root.",
            file=sys.stderr,
        )
        return False
    missing = [str(p) for p in files if not p.is_file()]
    if missing:
        print(
            "check_no_silent_stubs.py: FATAL -- these paths do not exist:\n  "
            + "\n  ".join(missing),
            file=sys.stderr,
        )
        return False
    return True


def _report_violations(violations: list[tuple[str, dict]]) -> None:
    """List every stub found, then explain what each rule means and how to fix it.

    The trailing prose is long on purpose: both rules reject code that
    compiles, links and looks deliberate, so a bare file:line would leave a
    reader with no idea why the gate objects.
    """
    print(
        f"check_no_silent_stubs.py: {len(violations)} silent stub(s) found:\n",
        file=sys.stderr,
    )
    for rule, f in sorted(violations, key=lambda v: (v[1]["path"], v[1]["line"])):
        print(f"  [{rule}] {f['path']}:{f['line']}  {f['name']}()", file=sys.stderr)
        if rule == "SHADOW":
            for other in f["shadows"]:
                print(f"           real implementation lives in {other}", file=sys.stderr)
        else:
            print(f"           discards its arguments, returns {f['returns']}", file=sys.stderr)
    print(
        "\n[SHADOW] A second, do-nothing definition of a symbol that is really\n"
        "implemented elsewhere in this tree. Whichever one the linker picks, the\n"
        "build silently disables working code. Compile the real implementation\n"
        "instead of redefining the symbol -- if it did not link, fix the build\n"
        "recipe, do not fake the symbol.\n"
        "\n[CANNED] The function reports that an operation was refused when no\n"
        "implementation exists at all. Implement it, or delete it and update\n"
        "every call site in the same change.\n"
        "\nIf -- and only if -- the capability is blocked on hardware that does\n"
        "not physically exist yet, mark it with a TODO naming the missing part:\n"
        "  TODO(ESP32-C6 radio module ordered, not yet on the bench)\n"
        "A bare TODO with no named dependency is not a waiver, and no waiver is\n"
        "available when a real implementation already exists in the tree.",
        file=sys.stderr,
    )


def main(argv: list[str]) -> int:
    """Scan first-party C for SHADOW and CANNED stubs, or run the detector selftest.

    An empty source set is FATAL rather than clean, and the message says to run
    from the repository root: the roots are resolved relative to the current
    directory, so invoking this from elsewhere finds nothing and would
    otherwise report a stub-free tree it never opened.

    CI runs ``--selftest`` before the scan for the same reason in the other
    direction -- a detector whose patterns stopped matching would also report
    a clean tree, and only an assertion in both directions can tell the two
    apart.

    Returns 0 when no stub is found, 1 on a finding, on an empty source set,
    on a named file that does not exist, or on a failing selftest.
    """
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("files", nargs="*", help="specific files to scan")
    ap.add_argument(
        "--selftest",
        action="store_true",
        help="prove the detector fires on stubs and not on legitimate code",
    )
    args = ap.parse_args(argv[1:])

    if args.selftest:
        with tempfile.TemporaryDirectory() as td:
            return selftest(Path(td))

    files = first_party_sources(args.files)
    if not _scan_set_is_usable(files):
        return 1

    violations = analyse(files)
    if not violations:
        print(f"check_no_silent_stubs.py: OK ({len(files)} files scanned, no silent stubs)")
        return 0

    _report_violations(violations)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
