#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: shellcheck + shfmt for first-party shell scripts.

ShellCheck (correctness, at ``--severity=style`` plus the opt-in checks listed
in :data:`SHELLCHECK_ENABLE`) and shfmt (formatting, 2-space case-indented) are
the shell equivalents of ruff.  This wrapper fails on any finding -- no
grandfathering.  Both tools must be on PATH (or named via ``SHELLCHECK`` /
``SHFMT``); without them the gate skips locally unless ``--require`` is passed,
which CI uses to fail on a missing tool.

``style`` is the tightest severity ShellCheck offers, so nothing is filtered by
level.  The opt-in checks are the ones that are both *fixable in place* and map
to a defect class this tree has actually shipped -- unquoted expansions, values
that are read but never assigned, ``which`` instead of ``command -v``.  Two
opt-in checks are deliberately NOT enabled; see ``docs/STYLE_GUIDE.md`` and the
comment on :data:`SHELLCHECK_DISABLED_OPTIONAL` for the measurements behind
that call.

Run::

    check_shell.py             # gate (fail on any finding)
    check_shell.py --require   # fail (not skip) if a tool is absent
    check_shell.py --selftest  # prove the gate fires and stays quiet

Exit 0 if clean, exit 1 on findings, exit 2 on tool error.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# relative path -> {SC code: count}
Findings = dict[str, dict[str, int]]

# shfmt style: 2-space indent, indent switch-case branches (matches the C side).
SHFMT_ARGS = ("-i", "2", "-ci")

# `style` is ShellCheck's tightest severity -- nothing is filtered out by level.
SHELLCHECK_SEVERITY = "style"

# Opt-in checks. ShellCheck ships these off by default, so no severity setting
# reaches them; each has to be named. Every one below is fixable in place.
SHELLCHECK_ENABLE = (
    "avoid-negated-conditions",
    "avoid-nullary-conditions",
    "check-unassigned-uppercase",
    "deprecate-which",
    "quote-safe-variables",
    "useless-use-of-cat",
)

# Deliberately NOT enabled. Re-measured for #363 on the first-party shell files
# at ShellCheck 0.11.0, on top of the severity + opt-in set above:
#
#   check-set-e-suppressed (SC2310/SC2311) -- 90 findings / 24 files.
#     Unsatisfiable by construction, and verified form by form: it fires on
#     `fn || rc=$?`, on the rewrite its own help text recommends
#     (`if fn; then rc=0; else rc=$?; fi`), on a bare one-line predicate, on
#     `! fn` and on `fn && ...`. The only two forms it ACCEPTS are worse than
#     the ones it rejects -- `set +e; fn; rc=$?; set -e` passes while a
#     brace-bodied callee still runs past a mid-body failure, and
#     `( fn ); rc=$?` passes while aborting the parent outright. Enabling it
#     would mean ~90 inline disables and would push authors toward the form it
#     cannot see. The signal is real, so it is covered instead by
#     scripts/utils/check_errexit_masking.py, which fires only where a
#     first-party function with two or more failable commands is invoked with
#     its status masked. The runtime regression for the specific gate-suite
#     failure remains asserted by suite_errexit_selftest in scripts/ci.sh.
#   check-extra-masked-returns (SC2312) -- 163 findings / 34 files.
#     Overwhelmingly command substitutions on commands that cannot meaningfully
#     fail (uname, date -Iseconds, basename, id -un) or inside `< <(...)`
#     process substitutions with no single-statement rewrite. Fixing them means
#     hoisting each into a preceding assignment: some of that is worth doing and
#     some is pure motion, and a blanket enable cannot tell the two apart. The
#     subset that actually matters -- masking a FUNCTION's status -- is exactly
#     what check_errexit_masking.py now covers.
#   require-variable-braces (SC2250) -- 2901 findings / 60 files.
#     Presentation. `$var` and `${var}` are identical outside the
#     disambiguation cases, which ShellCheck already flags separately at the
#     level of correctness.
#   require-double-brackets (SC2292) -- 176 findings / 21 files.
#     Presentation, and actively wrong here: `[` is correct in the POSIX-sh
#     scripts in this tree, so this would push them toward bash-only for no
#     behavioural gain.
#   add-default-case (SC2249) -- 56 findings / 15 files.
#     A default case is right for a dispatch on external input, which this tree
#     already writes; it is noise on an exhaustive match over a fixed internal
#     enum, and the check cannot distinguish them.
SHELLCHECK_DISABLED_OPTIONAL = (
    "check-set-e-suppressed",
    "check-extra-masked-returns",
    "require-variable-braces",
    "require-double-brackets",
    "add-default-case",
)

EXCLUDE_FRAGMENTS = (
    "libs/third_party/",
    "libs/fonts/",
    "port/threadx/",
    "/build/",
    "/build-cov/",
    "/_deps/",
    "node_modules/",
)


def _shellcheck_args() -> list[str]:
    """The severity + opt-in flags every ShellCheck invocation here shares."""
    return [
        f"--severity={SHELLCHECK_SEVERITY}",
        "--enable=" + ",".join(SHELLCHECK_ENABLE),
    ]


def _find(env_var: str, name: str) -> str | None:
    env = os.environ.get(env_var)
    if env and Path(env).exists():
        return env
    return shutil.which(name)


def _git_ls(*pathspec: str) -> list[str]:
    """Tracked plus untracked-but-not-ignored paths matching `pathspec`.

    Enumerates via ``git ls-files`` instead of a filesystem walk so locally
    present, git-excluded trees (``.git/info/exclude`` entries such as
    ``recon/`` vendor drops) never enter the gate -- a raw ``rglob`` scanned
    them and failed commits on third-party findings CI can never see.
    """
    git_tool = shutil.which("git") or "git"
    proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
        [git_tool, "ls-files", "--cached", "--others", "--exclude-standard", "--", *pathspec],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        sys.stderr.write(f"git ls-files failed (exit {proc.returncode})\n")
        sys.exit(2)
    return [ln.strip() for ln in proc.stdout.splitlines() if ln.strip()]


def _has_shell_shebang(rel: str) -> bool:
    """True when `rel` opens with a ``#!`` line naming sh, bash or zsh."""
    try:
        with (REPO_ROOT / rel).open("rb") as handle:
            first = handle.readline(200)
    except OSError:
        return False
    if not first.startswith(b"#!"):
        return False
    line = first.decode("utf-8", errors="replace")
    return any(tok in line for tok in ("bash", "zsh", "/sh", "env sh"))


def first_party_scripts() -> list[str]:
    """First-party shell scripts: by ``*.sh`` suffix OR by shebang.

    The shebang sweep is not hypothetical. Every git hook in ``scripts/git/``
    -- pre-commit, pre-push, commit-msg, post-merge, post-commit,
    post-checkout -- is an extensionless bash script, so a suffix-only scope
    left the hooks that enforce this entire tree as the only shell in it that
    nothing shellchecked or shfmt'd. That is the #296/#332/#358/#359/#360
    defect class exactly: a scope narrower than the thing it claims to cover,
    reporting clean.
    """
    by_suffix = _git_ls("*.sh")
    known = set(by_suffix)
    by_shebang = [rel for rel in _git_ls() if rel not in known and _has_shell_shebang(rel)]
    out = [
        rel
        for rel in known.union(by_shebang)
        if not any(frag in f"/{rel}" for frag in EXCLUDE_FRAGMENTS)
    ]
    return sorted(out)


def _run_shellcheck(tool: str, files: list[str], cwd: Path | None = None) -> Findings:
    proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
        [tool, *_shellcheck_args(), "-f", "json", *files],
        cwd=cwd or REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode not in (0, 1):
        sys.stderr.write(proc.stderr)
        sys.stderr.write(f"shellcheck failed (exit {proc.returncode})\n")
        sys.exit(2)
    findings: Findings = {}
    for item in json.loads(proc.stdout or "[]"):
        rel = item["file"]
        code = f"SC{item['code']}"
        findings.setdefault(rel, {})
        findings[rel][code] = findings[rel].get(code, 0) + 1
    return findings


def _run_shfmt(tool: str, files: list[str], cwd: Path | None = None) -> list[str]:
    proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
        [tool, "-l", *SHFMT_ARGS, *files],
        cwd=cwd or REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    return sorted(line.strip() for line in proc.stdout.splitlines() if line.strip())


# --------------------------------------------------------------------------
# Self-test
# --------------------------------------------------------------------------
# Each case is (label, filename, body, must_fire). The "must fire" half proves
# the gate still detects every class it claims to enforce -- a check silently
# dropped from SHELLCHECK_ENABLE, or a severity quietly relaxed back to
# `warning`, turns one of these green and fails the selftest. The "must stay
# quiet" half proves the bar is survivable: the tricky-but-correct forms this
# tree actually uses must not be flagged, or the gate becomes noise people
# route around.
SELFTEST_CASES: tuple[tuple[str, str, str, bool], ...] = (
    # ---- must FIRE ------------------------------------------------------
    (
        "SC2086 unquoted expansion (info-level: invisible at the old warning bar)",
        "fire_unquoted.sh",
        "#!/usr/bin/env bash\nf=/a/b\nrm -f $f\n",
        True,
    ),
    (
        "SC2006 legacy backticks (style-level: needs severity=style)",
        "fire_backtick.sh",
        '#!/usr/bin/env bash\nd="`date`"\necho "$d"\n',
        True,
    ),
    (
        "SC2248 unquoted safe variable (opt-in: quote-safe-variables)",
        "fire_quotesafe.sh",
        "#!/usr/bin/env bash\nrc=0\nexit $rc\n",
        True,
    ),
    (
        "SC2154 referenced but never assigned (opt-in: check-unassigned-uppercase)",
        "fire_unassigned.sh",
        '#!/usr/bin/env bash\necho "${NEVER_SET_ANYWHERE}"\n',
        True,
    ),
    (
        "SC2230 which instead of command -v (opt-in: deprecate-which)",
        "fire_which.sh",
        "#!/usr/bin/env bash\nwhich gcc >/dev/null\n",
        True,
    ),
    (
        "SC2002 useless cat (opt-in: useless-use-of-cat)",
        "fire_uuoc.sh",
        "#!/usr/bin/env bash\ncat /etc/hosts | grep -q localhost\n",
        True,
    ),
    (
        "SC2244 nullary condition (opt-in: avoid-nullary-conditions)",
        "fire_nullary.sh",
        '#!/usr/bin/env bash\nv=x\nif [ "$v" ]; then echo hi; fi\n',
        True,
    ),
    (
        "SC2164 cd without a failure guard (warning-level baseline)",
        "fire_cd.sh",
        '#!/usr/bin/env bash\ncd /nonexistent-selftest-dir\necho "ran on regardless"\n',
        True,
    ),
    # ---- must stay QUIET ------------------------------------------------
    (
        "empty-array guard for bash 3.2 set -u",
        "quiet_array_guard.sh",
        "#!/usr/bin/env bash\nset -euo pipefail\nargs=()\n"
        'if [[ -n "${HOME:-}" ]]; then args+=(--home "$HOME"); fi\n'
        "printf '%s\\n' ${args[@]+\"${args[@]}\"}\n",
        False,
    ),
    (
        "printf with a constant format and variable arguments",
        "quiet_printf.sh",
        "#!/usr/bin/env bash\nset -euo pipefail\ncolor=$'\\033[0;32m'\n"
        'printf \'%sdone%s\\n\' "$color" "$color"\n',
        False,
    ),
    (
        "deliberate word-split routed through an array",
        "quiet_array_split.sh",
        "#!/usr/bin/env bash\nset -euo pipefail\nextra=()\n"
        'read -r -a extra <<<"--flag value"\n'
        "printf '<%s>' ${extra[@]+\"${extra[@]}\"}\n",
        False,
    ),
    (
        "command -v guard, quoted status variable, braced condition",
        "quiet_idiomatic.sh",
        "#!/usr/bin/env bash\nset -euo pipefail\nrc=0\n"
        "if ! command -v gcc >/dev/null 2>&1; then rc=1; fi\n"
        'exit "$rc"\n',
        False,
    ),
)

# A file shfmt must reformat: 4-space indent where SHFMT_ARGS demands 2.
SELFTEST_SHFMT_BAD = (
    "fire_shfmt.sh",
    '#!/usr/bin/env bash\nif true; then\n    echo "four-space indent"\nfi\n',
)


def selftest(tmp: Path) -> int:
    """Assert the gate fires on every enforced class and stays quiet otherwise."""
    sc_tool = _find("SHELLCHECK", "shellcheck")
    fmt_tool = _find("SHFMT", "shfmt")
    if not sc_tool or not fmt_tool:
        missing = " and ".join(
            n for n, t in (("shellcheck", sc_tool), ("shfmt", fmt_tool)) if not t
        )
        sys.stderr.write(f"check_shell.py --selftest: {missing} not found\n")
        return 2

    failures: list[str] = []

    for label, fname, body, must_fire in SELFTEST_CASES:
        path = tmp / fname
        path.write_text(body)
        fired = bool(_run_shellcheck(sc_tool, [fname], cwd=tmp))
        if fired != must_fire:
            verb = "did not fire" if must_fire else "fired"
            codes = _run_shellcheck(sc_tool, [fname], cwd=tmp).get(fname, {})
            failures.append(f"  shellcheck {verb} (unexpected): {label} {codes or ''}")

    fmt_name, fmt_body = SELFTEST_SHFMT_BAD
    (tmp / fmt_name).write_text(fmt_body)
    if not _run_shfmt(fmt_tool, [fmt_name], cwd=tmp):
        failures.append(f"  shfmt did not flag misindented file: {fmt_name}")
    quiet_name = "quiet_shfmt.sh"
    (tmp / quiet_name).write_text('#!/usr/bin/env bash\nif true; then\n  echo "ok"\nfi\n')
    if _run_shfmt(fmt_tool, [quiet_name], cwd=tmp):
        failures.append(f"  shfmt flagged a correctly formatted file: {quiet_name}")

    if failures:
        sys.stderr.write("check_shell.py --selftest: FAILED\n\n")
        sys.stderr.write("\n".join(failures) + "\n")
        return 1

    fires = sum(1 for c in SELFTEST_CASES if c[3]) + 1
    quiets = sum(1 for c in SELFTEST_CASES if not c[3]) + 1
    print(
        f"check_shell.py --selftest: PASS "
        f"({fires + quiets} cases: {fires} must fire, {quiets} must stay quiet)"
    )
    return 0


def _report(checks: Findings, fmt: list[str]) -> None:
    if checks:
        sys.stderr.write("check_shell.py: shellcheck finding(s):\n")
        for relfile in sorted(checks):
            for code, count in sorted(checks[relfile].items()):
                sys.stderr.write(f"  {relfile}: {code} x{count}\n")
    if fmt:
        joined = " ".join(SHFMT_ARGS)
        sys.stderr.write(f"check_shell.py: file(s) need `shfmt -w {joined}`:\n")
        for relfile in fmt:
            sys.stderr.write(f"  {relfile}\n")
    sys.stderr.write(
        "\nFix the finding or reformat. A `# shellcheck disable=SCxxxx` needs an\n"
        "inline reason on the same line saying why the finding does not apply.\n"
    )


def main(argv: list[str]) -> int:
    if "--selftest" in argv[1:]:
        with tempfile.TemporaryDirectory() as td:
            return selftest(Path(td))

    # Scope introspection for check_lint_coverage.py -- see the note in
    # check_ruff.py's main(). Deliberately independent of whether shellcheck
    # and shfmt are installed: the question is what this gate COVERS, and a
    # missing tool must not silently shrink the answer to nothing.
    if "--list-files" in argv[1:]:
        print("\n".join(first_party_scripts()))
        return 0

    sc_tool = _find("SHELLCHECK", "shellcheck")
    fmt_tool = _find("SHFMT", "shfmt")
    if not sc_tool or not fmt_tool:
        missing = " and ".join(
            n for n, t in (("shellcheck", sc_tool), ("shfmt", fmt_tool)) if not t
        )
        msg = f"check_shell.py: {missing} not found"
        if "--require" in argv[1:]:
            sys.stderr.write(msg + " (--require set)\n")
            sys.exit(1)
        print(msg + " -- skipping (install to enforce locally).")
        sys.exit(0)

    files = first_party_scripts()
    if not files:
        print("check_shell.py: no shell scripts to scan")
        return 0
    checks = _run_shellcheck(sc_tool, files)
    fmt = _run_shfmt(fmt_tool, files)
    if not checks and not fmt:
        print(
            f"check_shell.py: clean ({len(files)} file(s), "
            f"severity={SHELLCHECK_SEVERITY} + {len(SHELLCHECK_ENABLE)} opt-in check(s))."
        )
        return 0
    _report(checks, fmt)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
