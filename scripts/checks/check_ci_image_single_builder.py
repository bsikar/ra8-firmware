#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Fail if anything but the one blessed script builds the ``ra8-ci`` image.

WHY THIS EXISTS
===============
``just ci`` boots a locally-built image tagged ``ra8-ci:latest``.  #521 made
that image a pure function of its allowlisted build context -- it carries the
context's sha256 as an OCI label, and ``scripts/ci/devcontainer_image.sh`` is
the ONE thing that builds it, rebuilding rather than reusing a cached image
whose label disagrees with the tree.  That guarantee holds only while
``devcontainer_image.sh`` is the *only* builder.  It was not before: the
deleted ``inner-local.sh`` -- unreferenced, predating
``RA8_GATE_REGISTRY``, carrying hand-copied gate bodies -- also ran
``docker build -t ra8-ci:latest`` with the old "present, so reuse it forever"
logic.  Nothing would have noticed a new one appearing (#528).

This is the same hole ``check_ci_parity.py`` closes for workflow ``run:``
bodies: a second, drifting home for a thing that must have exactly one.  The
image needs the equivalent.

WHAT IT FORBIDS, PRECISELY
--------------------------
A container-image BUILD (``docker build`` / ``podman build`` /
``buildah bud`` / the ``"${RUNTIME[@]}" build`` array form) whose ``-t`` / ``--tag``
target names the ``ra8-ci`` image, in any first-party file under ``scripts/``,
``infra/``, ``.github/`` or ``just/`` OTHER than
``scripts/ci/devcontainer_image.sh``.

Naming it precisely matters, because ``ra8-ci`` is also a runner LABEL
(``runs-on: ra8-ci``), a filesystem PATH (``/var/lib/ra8-ci/``) and a scale-set
NAME all over the tree -- none of which build anything.  So the rule keys on a
build invocation AND a tag argument, not on the string appearing:

  * ``ra8-ci`` and ``ra8-ci:latest`` match; the tag may be given literally or
    through a shell variable this checker resolves within the same file (that
    is how ``devcontainer_image.sh`` itself spells it:
    ``IMAGE_TAG="${RA8_CI_IMAGE:-ra8-ci:latest}"`` then ``-t "$IMAGE_TAG"``).
  * ``ra8-ci-runner:v2`` and ``ra8-devcontainer:latest`` do NOT match -- the
    ``ci_runner`` role builds those and is a different, legitimate subject.
  * The retired ``ra8-firmware-dev`` and ``ra8-firmware-test`` tags are
    forbidden even on a run-only command: either one would recreate a second,
    unversioned developer image beside the digest-labelled ``ra8-ci`` image.
  * Any other direct build from ``.devcontainer/Dockerfile`` is forbidden
    outside the deployed runner-image role. A new tag cannot evade the rule.

SCOPE, HONESTLY
---------------
Command reconstruction joins shell backslash continuations and YAML
``cmd:``/``run:`` block scalars, so both a resurrected shell builder and a new
Ansible ``command:`` one are caught.  Markdown is out of scope: a doc is not a
build path.  A NON-VACUITY FLOOR asserts the one known builder is still
detected on every real run, so a reconstruction that quietly stopped matching
fails loudly instead of reporting a clean, empty tree.

Run with ``--selftest`` to prove both directions; ``--list`` to print the
builders it currently sees.

Exit 0 when the sole builder is the only one, 1 when a second builder exists,
2 when the scan itself collapsed (the floor is not met).
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from selftest_assert import expect, report

# The single blessed builder, repo-relative. If this file is renamed, update
# this constant IN THE SAME CHANGE -- a stale value makes the floor below fail
# loudly rather than letting the check silently trust the wrong file.
SOLE_BUILDER = "scripts/ci/devcontainer_image.sh"

# This checker embeds ``docker build -t ra8-ci:latest`` fixtures in its own
# selftest strings; it is a detector, not a build path, so it excludes itself.
# The fixtures are exercised through the temp-dir selftest, so nothing is lost.
SELF = "scripts/checks/check_ci_image_single_builder.py"

# The four trees a builder could hide in. Markdown is deliberately not here.
SCOPE_DIRS = ("scripts/", "infra/", ".github/", "just/")
SCOPE_FILES = frozenset({"justfile"})

# Vendored SOUP is governed by its upstream boundary, never by this checker.
THIRD_PARTY_PREFIXES = ("libs/third_party/", "apps/shared_libs/third_party/")

# The deployed Actions runner intentionally layers its own image from the same
# context. It is provisioned infrastructure rather than a developer image and
# carries a separately checked contract.
DEPLOYED_RUNNER_BUILDER = "infra/ansible/roles/ci_runner/tasks/main.yml"

# A build verb: docker/podman [buildx] build, buildah bud, or a runtime taken
# from a shell array/variable (``"${RUNTIME[@]}" build``) as devcontainer_image
# itself spells it.
BUILD_RE = re.compile(
    r"\b(?:docker|podman)(?:\s+buildx)?\s+build\b"
    r"|\bbuildah\s+bud\b"
    r'|\}"?\s+build\b'
)

# A -t / --tag argument and its value (quoted, or up to the next space).
TAG_RE = re.compile(r"""(?:--tag|(?<![\w-])-t)(?:=|\s+)("[^"]*"|'[^']*'|\S+)""")

# A shell VAR=value assignment, for resolving ``-t "$VAR"``.
ASSIGN_RE = re.compile(r"^\s*(?:export\s+)?([A-Za-z_]\w*)=(.*)$")

# A ``$VAR`` / ``${VAR}`` / ``${VAR:-default}`` reference, whole-token.
VARREF_RE = re.compile(r"\$\{?(?P<var>[A-Za-z_]\w*)(?::-(?P<default>[^}]*))?\}?")

# The forbidden image, as an IMAGE NAME (before any ``:tag``), anchored so
# ``ra8-ci-runner`` and ``ra8-devcontainer`` do not match but a registry
# prefix (``localhost/ra8-ci``) does.
CI_IMAGE_RE = re.compile(r"(?:^|/)ra8-ci(?::|$)")
LEGACY_DEV_IMAGE_RE = re.compile(r"(?<![\w-])ra8-firmware-(?:dev|test)(?::|\b)")
DEVCONTAINER_CONTEXT_RE = re.compile(r"(?<![\w.])\.devcontainer(?:[/\"'\s]|$)")

# A ``cmd:``/``run:``/``shell:``/``script:`` YAML block scalar opener.
BLOCK_KEY_RE = re.compile(
    r"^(?P<indent>\s*)(?:-\s+)?(?:[\w.]+\s+)?(?:cmd|run|shell|script):\s*[|>][+-]?\s*$"
)

# A surrounding quote pair is at least the two quote characters themselves.
MIN_QUOTED_LEN = 2

# Variable-resolution recursion bound: deep enough for ``$A -> $B -> literal``,
# shallow enough that a reference cycle terminates instead of looping.
MAX_RESOLVE_DEPTH = 6

# The tree cannot plausibly have zero build invocations under these three dirs:
# devcontainer_image.sh, the ci_runner role and the report scripts all build
# images. A scan that finds none has broken. Floor is expressed as "the sole
# builder must be found", which is stronger and self-describing.


def _repo_root() -> Path:
    """The repository root, via git."""
    return Path(
        subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],  # noqa: S607 -- fixed argv
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
    )


def strip_comment(line: str) -> str:
    """Drop a shell/YAML ``#`` comment, respecting single and double quotes.

    A ``#`` starts a comment only at the start of the line or after
    whitespace; one glued to a word (``foo#bar``, a URL fragment) is data.
    """
    in_s = in_d = False
    for i, char in enumerate(line):
        if char == "'" and not in_d:
            in_s = not in_s
        elif char == '"' and not in_s:
            in_d = not in_d
        elif char == "#" and not in_s and not in_d and (i == 0 or line[i - 1].isspace()):
            return line[:i]
    return line


def logical_commands(text: str) -> list[str]:
    """Reconstruct whole shell commands from `text`.

    Joins backslash line-continuations and YAML ``cmd:``/``run:`` block
    scalars, so a ``-t`` argument on a different physical line than its
    ``build`` verb is still seen as one command. Comments are stripped first.
    """
    lines = text.split("\n")
    total = len(lines)
    out: list[str] = []
    i = 0
    while i < total:
        block = BLOCK_KEY_RE.match(lines[i])
        if block:
            key_indent = len(block.group("indent"))
            body: list[str] = []
            j = i + 1
            while j < total:
                bl = lines[j]
                if bl.strip() == "":
                    j += 1
                    continue
                if (len(bl) - len(bl.lstrip())) <= key_indent:
                    break
                body.append(strip_comment(bl).strip().rstrip("\\").strip())
                j += 1
            out.append(" ".join(part for part in body if part))
            i = j
            continue
        cur = strip_comment(lines[i])
        while cur.rstrip().endswith("\\") and i + 1 < total:
            cur = cur.rstrip()[:-1] + " " + strip_comment(lines[i + 1])
            i += 1
        out.append(cur)
        i += 1
    return out


def collect_assignments(text: str) -> dict[str, str]:
    """Every ``VAR=value`` in `text`, values with one layer of quotes removed."""
    assigns: dict[str, str] = {}
    for raw in text.split("\n"):
        match = ASSIGN_RE.match(strip_comment(raw))
        if match:
            assigns[match.group(1)] = _unquote(match.group(2).strip())
    return assigns


def _unquote(token: str) -> str:
    """Strip one balanced pair of surrounding single or double quotes."""
    token = token.strip()
    if len(token) >= MIN_QUOTED_LEN and token[0] == token[-1] and token[0] in "\"'":
        return token[1:-1]
    return token


def resolve(token: str, assigns: dict[str, str], depth: int = 0) -> str:
    """Resolve a ``$VAR`` / ``${VAR:-default}`` token against `assigns`.

    Bounded recursion; an unknown variable with no default resolves to itself,
    so an unresolvable tag simply fails to match rather than crashing.
    """
    token = _unquote(token)
    if depth > MAX_RESOLVE_DEPTH:
        return token
    ref = VARREF_RE.fullmatch(token)
    if not ref:
        return token
    var, default = ref.group("var"), ref.group("default")
    if var in assigns:
        return resolve(assigns[var], assigns, depth + 1)
    if default is not None:
        return resolve(default, assigns, depth + 1)
    return token


def ci_tags_built(text: str) -> list[str]:
    """The resolved ``ra8-ci`` tags this file's build commands target.

    Empty when the file builds no ``ra8-ci`` image -- whether it builds nothing,
    builds a different image, or references ``ra8-ci`` only as a label or path.
    """
    assigns = collect_assignments(text)
    hits: list[str] = []
    for command in logical_commands(text):
        if not BUILD_RE.search(command):
            continue
        for raw_tag in TAG_RE.findall(command):
            resolved = resolve(raw_tag, assigns)
            if CI_IMAGE_RE.search(resolved):
                hits.append(resolved)
    return hits


def legacy_dev_images(text: str) -> list[str]:
    """Retired developer image names in active commands or assignments."""
    commands = "\n".join(logical_commands(text))
    return sorted(set(LEGACY_DEV_IMAGE_RE.findall(commands)))


def builds_devcontainer_context(text: str) -> bool:
    """Whether a build command names the repository devcontainer context."""
    assignments = collect_assignments(text)
    context_vars = {
        name
        for name, value in assignments.items()
        if DEVCONTAINER_CONTEXT_RE.search(resolve(value, assignments))
        or DEVCONTAINER_CONTEXT_RE.search(value)
    }
    for command in logical_commands(text):
        if not BUILD_RE.search(command):
            continue
        if DEVCONTAINER_CONTEXT_RE.search(command):
            return True
        if any(re.search(rf"\$\{{?{re.escape(name)}(?:\}}|\b)", command) for name in context_vars):
            return True
    return False


def in_scope(rel: str) -> bool:
    """True for a first-party non-Markdown file under one of the scope dirs."""
    return (
        (rel.startswith(SCOPE_DIRS) or rel in SCOPE_FILES)
        and not rel.endswith(".md")
        and rel != SELF
        and not rel.startswith(THIRD_PARTY_PREFIXES)
    )


def scoped_files(root: Path) -> list[str]:
    """Every in-scope git-tracked file, repo-relative and sorted."""
    proc = subprocess.run(
        ["git", "ls-files", "-z"],  # noqa: S607 -- git from PATH is intended
        cwd=root,
        capture_output=True,
        text=True,
        check=True,
    )
    return sorted(rel for rel in proc.stdout.split("\0") if rel and in_scope(rel))


def find_builders(root: Path, rels: list[str]) -> dict[str, list[str]]:
    """Map each in-scope file that builds ``ra8-ci`` to the tags it targets."""
    builders: dict[str, list[str]] = {}
    for rel in rels:
        try:
            text = (root / rel).read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue  # a binary or unreadable file is not a build script
        tags = ci_tags_built(text)
        if tags:
            builders[rel] = tags
    return builders


def find_context_builders(root: Path, rels: list[str]) -> list[str]:
    """Files directly building the repository devcontainer context."""
    hits: list[str] = []
    for rel in rels:
        try:
            text = (root / rel).read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        if builds_devcontainer_context(text):
            hits.append(rel)
    return sorted(hits)


def find_legacy_images(root: Path, rels: list[str]) -> dict[str, list[str]]:
    """Map files still naming a retired developer image."""
    hits: dict[str, list[str]] = {}
    for rel in rels:
        try:
            text = (root / rel).read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        names = legacy_dev_images(text)
        if names:
            hits[rel] = names
    return hits


# --------------------------------------------------------------------------
# selftest
# --------------------------------------------------------------------------

# The real builder's shape: an indirected tag with a ``:-`` default, exactly as
# devcontainer_image.sh writes it. The floor and the "allowed" case both lean
# on this being detected.
GOOD_SOLE = (
    'IMAGE_TAG="${RA8_CI_IMAGE:-ra8-ci:latest}"\n'
    "require_runtime\n"
    '"${RUNTIME[@]}" build \\\n'
    '  --label "$LABEL_KEY=$want" \\\n'
    '  -t "$IMAGE_TAG" \\\n'
    '  -f "$CONTEXT_DIR/Dockerfile" \\\n'
    '  "$CONTEXT_DIR"\n'
)

# A resurrected standalone builder, the #528 threat, literal tag.
BAD_LITERAL = (
    "#!/usr/bin/env bash\ndocker build -t ra8-ci:latest -f .devcontainer/Dockerfile .devcontainer\n"
)

# The same threat, hiding the tag behind a variable.
BAD_INDIRECT = 'IMG="ra8-ci:latest"\npodman build -t "$IMG" .devcontainer\n'

# A new Ansible role building ra8-ci through a folded block scalar.
BAD_ANSIBLE = (
    "- name: Build it\n"
    "  ansible.builtin.command:\n"
    "    cmd: >-\n"
    "      buildah bud --isolation chroot\n"
    "      -t ra8-ci:latest\n"
    "      -f ctx/.devcontainer/Dockerfile\n"
    "      ctx/.devcontainer\n"
)

# The ci_runner role: a legitimate, different subject via Jinja variables.
OK_RUNNER = (
    "- name: Build the runner image\n"
    "  ansible.builtin.command:\n"
    "    cmd: >-\n"
    "      buildah bud --isolation chroot\n"
    "      -t {{ ci_runner_image }}\n"
    "      -f ctx/runner/Dockerfile ctx/runner\n"
)

# A different image tag and context entirely is unrelated.
OK_OTHER_TAG = (
    'IMAGE_TAG="ra8-firmware-docs:latest"\n'
    'docker build -t "$IMAGE_TAG" -f docs/container/Dockerfile docs/container\n'
)

BAD_LEGACY_RUN = "docker run --rm ra8-firmware-dev:latest just tests::build\n"
BAD_OTHER_CONTEXT_TAG = (
    "docker build -t local-dev:latest -f .devcontainer/Dockerfile .devcontainer\n"
)
BAD_INDIRECT_CONTEXT = (
    'CONTEXT_DIR="$REPO_ROOT/.devcontainer"\n'
    'docker build -t local-dev:latest -f "$CONTEXT_DIR/Dockerfile" "$CONTEXT_DIR"\n'
)

# ra8-ci as a runner label and a path -- no build at all.
OK_LABEL_PATH = "runs-on: ra8-ci\nlabels: [ra8-ci]\ndir: /var/lib/ra8-ci/build-context\n"

# Uses (not builds) the image, and the -t there is a tty flag, not a tag.
OK_RUN_ONLY = "docker run -t ra8-ci:latest just ci\n"


# Each case: (should the detector fire?, fixture text, assertion label).
_DETECTION_CASES = (
    (True, GOOD_SOLE, "the sole-builder shape is detected as a builder"),
    (True, BAD_LITERAL, "a literal 'docker build -t ra8-ci:latest' is detected"),
    (True, BAD_INDIRECT, "a variable-indirected ra8-ci build is detected"),
    (True, BAD_ANSIBLE, "a folded-YAML buildah bud of ra8-ci is detected"),
    (False, OK_RUNNER, "the ci_runner Jinja-var build is NOT flagged"),
    (False, OK_OTHER_TAG, "an unrelated image build is NOT flagged"),
    (False, OK_LABEL_PATH, "ra8-ci as a label/path is NOT flagged"),
    (False, OK_RUN_ONLY, "'docker run -t ra8-ci' (run, not build) is NOT flagged"),
)


def _selftest_detection(failures: list[str]) -> None:
    """Assert ci_tags_built fires and stays quiet on the right inputs."""
    for should_fire, text, label in _DETECTION_CASES:
        expect(bool(ci_tags_built(text)) is should_fire, label, failures)
    expect(
        bool(legacy_dev_images(BAD_LEGACY_RUN)),
        "a retired developer image is detected even on run-only use",
        failures,
    )
    expect(
        not legacy_dev_images(OK_RUN_ONLY),
        "the canonical ra8-ci run is not a retired-image finding",
        failures,
    )
    expect(
        builds_devcontainer_context(BAD_OTHER_CONTEXT_TAG),
        "a differently-tagged devcontainer build is detected",
        failures,
    )
    expect(
        builds_devcontainer_context(BAD_INDIRECT_CONTEXT),
        "a variable-indirected devcontainer context is detected",
        failures,
    )
    expect(
        not builds_devcontainer_context(OK_OTHER_TAG),
        "an unrelated image context stays out of scope",
        failures,
    )


def _selftest_end_to_end(failures: list[str]) -> None:
    """Assert the offender computation over a synthetic tree, both directions."""
    # A stand-in for a resurrected second builder; no such file exists.
    second = "scripts/ci/inner-local.sh"  # PATHREF-OK: selftest fixture path
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "scripts/ci").mkdir(parents=True)
        (root / "scripts/checks").mkdir(parents=True)
        (root / SOLE_BUILDER).write_text(GOOD_SOLE, encoding="utf-8")
        rels = [SOLE_BUILDER]

        builders = find_builders(root, rels)
        offenders = sorted(set(builders) - {SOLE_BUILDER})
        expect(
            SOLE_BUILDER in builders,
            "clean tree: the sole builder is detected (the floor)",
            failures,
        )
        expect(not offenders, "clean tree: no offenders", failures)

        (root / second).write_text(BAD_LITERAL, encoding="utf-8")
        rels.append(second)
        builders = find_builders(root, rels)
        offenders = sorted(set(builders) - {SOLE_BUILDER})
        expect(
            offenders == [second],
            "a second builder is reported as an offender",
            failures,
        )


def _selftest_extended_end_to_end(failures: list[str]) -> None:
    """Assert context/tag regressions are reported over a synthetic tree."""
    context_rel = "just/devcontainer.just"  # PATHREF-OK: selftest fixture path
    legacy_rel = "scripts/ci/old-wrapper.sh"  # PATHREF-OK: selftest fixture path
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "just").mkdir()
        (root / "scripts/ci").mkdir(parents=True)
        (root / context_rel).write_text(BAD_OTHER_CONTEXT_TAG, encoding="utf-8")
        (root / legacy_rel).write_text(BAD_LEGACY_RUN, encoding="utf-8")
        rels = [context_rel, legacy_rel]
        expect(
            find_context_builders(root, rels) == [context_rel],
            "a second devcontainer-context builder is reported",
            failures,
        )
        expect(
            sorted(find_legacy_images(root, rels)) == [legacy_rel],
            "a retired developer image use is reported",
            failures,
        )


def _selftest_floor_on_real_tree(failures: list[str]) -> None:
    """The real tree must still contain the sole builder -- the non-vacuity floor."""
    resolved = True
    try:
        root = _repo_root()
    except (subprocess.CalledProcessError, FileNotFoundError):
        resolved = False
    expect(resolved, "git rev-parse resolves the repo root", failures)
    if not resolved:
        return
    builders = find_builders(root, scoped_files(root))
    offenders = sorted(set(builders) - {SOLE_BUILDER})
    context_builders = find_context_builders(root, scoped_files(root))
    allowed_context_builders = {SOLE_BUILDER, DEPLOYED_RUNNER_BUILDER}
    context_offenders = sorted(set(context_builders) - allowed_context_builders)
    legacy = find_legacy_images(root, scoped_files(root))
    expect(
        SOLE_BUILDER in builders,
        f"the real tree still detects {SOLE_BUILDER} as the builder",
        failures,
    )
    expect(not offenders, f"the real tree has no second builder (saw {offenders})", failures)
    expect(
        not context_offenders,
        f"the real tree has no second devcontainer-context builder (saw {context_offenders})",
        failures,
    )
    expect(not legacy, f"the real tree has no retired developer image tag (saw {legacy})", failures)


def selftest() -> int:
    """Prove the detector fires and stays quiet, and that the floor holds."""
    print("check_ci_image_single_builder.py --selftest")
    failures: list[str] = []
    _selftest_detection(failures)
    _selftest_end_to_end(failures)
    _selftest_extended_end_to_end(failures)
    _selftest_floor_on_real_tree(failures)
    return report(failures)


def report_extended_violations(root: Path, rels: list[str]) -> bool:
    """Report direct context builders and retired developer image names."""
    failed = False
    context_builders = find_context_builders(root, rels)
    allowed_context_builders = {SOLE_BUILDER, DEPLOYED_RUNNER_BUILDER}
    context_offenders = sorted(set(context_builders) - allowed_context_builders)
    for rel in context_offenders:
        print(
            f"  {rel}: directly builds .devcontainer under a second image contract",
            file=sys.stderr,
        )
        failed = True
    legacy = find_legacy_images(root, rels)
    for rel, names in sorted(legacy.items()):
        print(f"  {rel}: uses retired developer image {' '.join(names)}", file=sys.stderr)
        failed = True
    if failed:
        print(
            f"Route writable developer runs through scripts/ci/devcontainer_run.sh;\n"
            f"only {SOLE_BUILDER} may build its digest-labelled image.",
            file=sys.stderr,
        )
    return failed


def main(argv: list[str]) -> int:
    """Fail if any file but the sole builder builds the ``ra8-ci`` image."""
    ap = argparse.ArgumentParser(description="One builder for ra8-ci:latest, and only one.")
    ap.add_argument("--selftest", action="store_true", help="assert both directions")
    ap.add_argument("--list", action="store_true", help="print every detected ra8-ci builder")
    args = ap.parse_args(argv[1:])

    if args.selftest:
        return selftest()

    root = _repo_root()
    rels = scoped_files(root)
    builders = find_builders(root, rels)

    if args.list:
        for rel in sorted(builders):
            print(f"{rel}: {' '.join(builders[rel])}")
        return 0

    # Non-vacuity floor: if the one builder we KNOW exists is not detected, the
    # reconstruction has broken and every "clean" verdict below is worthless.
    if SOLE_BUILDER not in builders:
        print(
            "check_ci_image_single_builder.py: FATAL -- the known builder\n"
            f"  {SOLE_BUILDER}\n"
            "  was NOT detected building ra8-ci. Either the file was renamed (update\n"
            "  SOLE_BUILDER in this checker) or the command reconstruction stopped\n"
            "  matching (a collapsed scan must fail, not report clean).",
            file=sys.stderr,
        )
        return 2

    offenders = sorted(set(builders) - {SOLE_BUILDER})
    if offenders:
        print(
            f"\n{len(offenders)} file(s) build the ra8-ci image besides {SOLE_BUILDER}:\n",
            file=sys.stderr,
        )
        for rel in offenders:
            print(f"  {rel}: builds {' '.join(builders[rel])}", file=sys.stderr)
        print(
            "\nra8-ci:latest must have exactly one builder so its context-digest\n"
            "staleness guarantee cannot be bypassed (#521, #528). Route this build\n"
            f"through {SOLE_BUILDER}, or -- if it is a different image -- give it a\n"
            "different tag (the ci_runner role builds ra8-ci-runner / ra8-devcontainer).",
            file=sys.stderr,
        )
        return 1

    if report_extended_violations(root, rels):
        return 1

    print(
        "check_ci_image_single_builder.py: one digest-labelled developer image; "
        f"{SOLE_BUILDER} is its only builder."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
