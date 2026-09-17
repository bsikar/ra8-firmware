# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Read the Cortex-M33 (CPU1) warning bar itself, not just who is attached to it.

``check_cpu1_warning_profile.py`` answers WHICH translation units the CPU1
first-party profile is attached to (#843).  On its own that is worth nothing:
an emptied or quietly narrowed profile would still report every CPU1 source as
"profile-covered", and the gate would stay green over a bar that no longer
holds ``-Werror``.  This module is the other half.  It reads
``ra8_cpu1_warning_profile()`` out of ``cmake/ra8_add_app.cmake`` and the
canonical M85 first-party set out of ``cmake/ra8_warnings.cmake``, and holds
four things:

  * the CPU1 bar still sets ``-Wall``, ``-Wextra``, ``-Werror``,
    ``-Wstack-usage`` and ``-fstack-usage``, with a positive integer frame
    budget (``-Wstack-usage=0`` is how the host build turns the frame gate
    OFF, so a present flag is not proof of a live gate);
  * every warning flag in the M85 set is on the CPU1 bar, or is a DECLARED
    divergence carrying its reason.  That parity used to be a prose "keep it
    in step with" comment with nothing measuring it, and it was already false:
    ``-Wconversion`` was in the M85 set and not on the M33 bar;
  * a declared divergence that no longer describes a real difference FAILS as
    stale, so the table can only shrink without review;
  * both attachment sites take their flags from that ONE function and neither
    spells a warning flag of its own, because a second copy of the bar is how
    the helper-owned and app-added paths drift apart while both still read
    "covered".

Either cmake file missing, the profile function gone, or either list parsing
to no flags at all is FATAL, never clean.

The fixtures and the case table live here too, so the checker's ``--selftest``
drives them.  Nothing here reads a build tree: this is CMake intent, and the
compile-commands / ``.su`` evidence is produced per change by a real ARM
cross-build and recorded in review.
"""

from __future__ import annotations

import re
import textwrap
from pathlib import Path

# The helper names this module and the checker agree on; the checker imports
# them from here so the two cannot name different functions.
HELPER = "ra8_add_cpu1_image"
FIRST_PARTY_HELPER = "ra8_cpu1_add_first_party_sources"


def strip_comments(text: str) -> str:
    """Drop ``#`` comments so commentary cannot register as a flag."""
    out = []
    for line in text.splitlines():
        hash_at = line.find("#")
        out.append(line if hash_at < 0 else line[:hash_at])
    return "\n".join(out)


ADD_APP_REL = "cmake/ra8_add_app.cmake"
WARNINGS_REL = "cmake/ra8_warnings.cmake"
PROFILE_FUNCTION = "ra8_cpu1_warning_profile"
M85_FUNCTION = "ra8_target_enable_project_warnings"
# The two sites that put the profile onto sources.  Both must take it FROM
# PROFILE_FUNCTION: a hand-spelled list at either one is exactly how the
# helper-owned and app-added paths drift apart while both still read "covered".
PROFILE_CALLERS = (HELPER, FIRST_PARTY_HELPER)
# The bar #843 is about.  Coverage means nothing without these.
REQUIRED_BAR = ("-Wall", "-Wextra", "-Werror", "-Wstack-usage", "-fstack-usage")
# M85 canonical flags deliberately NOT on the M33 bar, each with the reason a
# reviewer needs.  Empty today: the M33 profile carries the whole M85 set.
DECLARED_DIVERGENCE: dict[str, str] = {}

# Warning-class flags only.  ``-Wl,--gc-sections`` is a linker option, not a
# diagnostic, and the CPU1 link options are full of them.
FLAG_RE = re.compile(r"-W(?!l,)[A-Za-z][A-Za-z0-9=+_.${}-]*|-fstack-usage")


def read_cmake(root: Path, rel: str) -> str | None:
    """Comment-stripped text of one cmake file, or None when unreadable."""
    try:
        return strip_comments((root / rel).read_text(encoding="utf-8"))
    except OSError:
        return None


def function_body(text: str, name: str) -> str | None:
    """Body of ``function(<name> ...)`` up to its ``endfunction``, or None."""
    match = re.search(rf"function\s*\(\s*{re.escape(name)}\b", text)
    if match is None:
        return None
    end = text.find("endfunction", match.end())
    return text[match.end() : end if end >= 0 else len(text)]


def flag_name(flag: str) -> str:
    """``-Wstack-usage=2048`` and ``-Wstack-usage=${N}`` are the same option."""
    return flag.split("=", 1)[0]


def flags_in(body: str) -> list[str]:
    """Every warning-class flag token in a cmake function body, in order."""
    return FLAG_RE.findall(body)


def profile_flags(root: Path) -> list[str] | None:
    """The CPU1 bar as ``ra8_cpu1_warning_profile()`` spells it."""
    text = read_cmake(root, ADD_APP_REL)
    if text is None:
        return None
    body = function_body(text, PROFILE_FUNCTION)
    return None if body is None else flags_in(body)


def m85_flags(root: Path) -> list[str] | None:
    """The canonical first-party set the CPU1 bar claims to be in step with.

    Read through the generator expressions, because that is how
    ``cmake/ra8_warnings.cmake`` spells the C-only and GNU-only half of it.
    """
    text = read_cmake(root, WARNINGS_REL)
    if text is None:
        return None
    body = function_body(text, M85_FUNCTION)
    return None if body is None else flags_in(body)


def stack_usage_findings(profile: list[str]) -> list[str]:
    """Refuse a frame budget that is not a positive integer literal.

    ``-Wstack-usage=0`` is how the host build turns the frame gate OFF
    (cmake/ra8_warnings.cmake says so).  On the M33 bar that would be a silent
    weakening dressed as a present flag, so the value is read, not just the
    option name.
    """
    for flag in profile:
        if flag_name(flag) != "-Wstack-usage":
            continue
        value = flag.split("=", 1)[1] if "=" in flag else ""
        if value.isdigit() and int(value) > 0:
            return []
        return [
            f"{ADD_APP_REL}: {PROFILE_FUNCTION}() sets '{flag}'; the CPU1 frame "
            "budget must be a positive integer literal, or the flag is present "
            "and the gate is off"
        ]
    return []


def bar_findings(profile: list[str]) -> list[str]:
    """The CPU1 profile must actually contain the bar #843 is about."""
    have = {flag_name(flag) for flag in profile}
    findings = [
        f"{ADD_APP_REL}: {PROFILE_FUNCTION}() no longer sets {required}; "
        "every CPU1 source this gate calls profile-covered is covered by "
        "nothing"
        for required in REQUIRED_BAR
        if required not in have
    ]
    return findings + stack_usage_findings(profile)


def parity_findings(
    profile: list[str],
    m85: list[str],
    divergence: dict[str, str],
) -> list[str]:
    """Hold the CPU1 bar to the M85 set it claims to be kept in step with."""
    on_m33 = {flag_name(flag) for flag in profile}
    on_m85 = {flag_name(flag) for flag in m85}
    findings = [
        f"{name}: in the M85 first-party set ({WARNINGS_REL}) and not on the "
        f"CPU1 bar ({ADD_APP_REL}); put it on the M33 profile, or declare the "
        "divergence with its reason in DECLARED_DIVERGENCE"
        for name in sorted(on_m85 - on_m33)
        if name not in divergence
    ]
    for name in sorted(divergence):
        if name in on_m33:
            findings.append(
                f"{name}: declared as an M33/M85 divergence but the CPU1 bar "
                "now carries it; drop the declaration"
            )
        elif name not in on_m85:
            findings.append(
                f"{name}: declared as an M33/M85 divergence but the M85 set no "
                "longer carries it either; drop the declaration"
            )
    return findings


def single_source_findings(text: str) -> list[str]:
    """Both attachment sites must read the bar from one function.

    A literal warning flag inside either caller is a second copy of the bar,
    and a copy is what lets the helper-owned and app-added paths diverge while
    this gate still calls both "covered".
    """
    findings: list[str] = []
    for caller in PROFILE_CALLERS:
        body = function_body(text, caller)
        if body is None:
            continue
        if f"{PROFILE_FUNCTION}(" not in body.replace(" ", ""):
            findings.append(
                f"{ADD_APP_REL}: {caller}() does not call {PROFILE_FUNCTION}(); "
                "the two CPU1 attachment sites must take the bar from one place"
            )
        findings += [
            f"{ADD_APP_REL}: {caller}() spells warning flag {flag} itself; the "
            f"bar lives in {PROFILE_FUNCTION}() only"
            for flag in sorted(set(flags_in(body)))
        ]
    return findings


def profile_findings(
    root: Path,
    divergence: dict[str, str] | None = None,
) -> list[str]:
    """Every disagreement between the bar as claimed and the bar as written."""
    profile = profile_flags(root)
    m85 = m85_flags(root)
    text = read_cmake(root, ADD_APP_REL)
    if profile is None or m85 is None or text is None:
        return []
    declared = DECLARED_DIVERGENCE if divergence is None else divergence
    return (
        bar_findings(profile)
        + parity_findings(profile, m85, declared)
        + single_source_findings(text)
    )


def profile_vacuity_error(root: Path) -> str:
    """Non-empty when the bar itself could not be read, so nothing was judged."""
    if read_cmake(root, ADD_APP_REL) is None:
        return f"{ADD_APP_REL} is missing or unreadable; the CPU1 bar cannot be read"
    if read_cmake(root, WARNINGS_REL) is None:
        return f"{WARNINGS_REL} is missing or unreadable; the M85 set cannot be read"
    profile = profile_flags(root)
    if not profile:
        return (
            f"{PROFILE_FUNCTION}() is missing from {ADD_APP_REL} or sets no "
            "warning flag; a bar that cannot be read must not report as held"
        )
    if not m85_flags(root):
        return (
            f"{M85_FUNCTION}() is missing from {WARNINGS_REL} or sets no warning "
            "flag; the parity rule would compare against nothing"
        )
    text = read_cmake(root, ADD_APP_REL) or ""
    missing = [caller for caller in PROFILE_CALLERS if function_body(text, caller) is None]
    if missing:
        return (
            f"{', '.join(missing)}() missing from {ADD_APP_REL}; this gate reads "
            "those calls to decide what is profile-covered"
        )
    return ""


# A minimal pair of cmake files carrying the bar: the CPU1 profile function,
# the two callers that attach it, and the M85 canonical set it must match.
FIXTURE_ADD_APP = """\
    function(ra8_cpu1_warning_profile _out_var)
      set(${_out_var}
          -Wall
          -Wextra
          -Werror
          -Wconversion
          -Wstack-usage=2048
          -fstack-usage
          PARENT_SCOPE
      )
    endfunction()

    function(ra8_cpu1_add_first_party_sources _target)
      ra8_cpu1_warning_profile(_c1fp_warnings)
      target_sources(${_target} PRIVATE ${ARGN})
      set_property(SOURCE ${ARGN} APPEND PROPERTY COMPILE_OPTIONS ${_c1fp_warnings})
    endfunction()

    function(ra8_add_cpu1_image)
      ra8_cpu1_warning_profile(_c1_warnings)
      set_source_files_properties(${_c1_srcs} PROPERTIES COMPILE_OPTIONS "${_c1_warnings}")
      target_link_options(${C1_NAME}.elf PRIVATE -Wl,--gc-sections)
    endfunction()
    """

FIXTURE_WARNINGS = """\
    function(ra8_target_enable_project_warnings target)
      target_compile_options(
        ${target}
        PRIVATE -Wall
                -Wextra
                -Werror
                -Wconversion
                $<$<COMPILE_LANG_AND_ID:C,GNU>:-Wstack-usage=${RA8_WARN_STACK_USAGE_BYTES}>
                $<$<COMPILE_LANG_AND_ID:C,GNU>:-fstack-usage>
      )
    endfunction()

    function(ra8_target_reject_warning_demotions target)
      if(_ra8_option MATCHES "(^|:)-Wno-error($|[=>])")
      endif()
    endfunction()
    """


def bar_cases() -> list[tuple[str, dict[str, object], str]]:
    """(name, run_case kwargs, expected substring) for the bar rules.

    Every case runs over the canonical inventory and the opt-in listfile, so
    the tree half is silent and whatever fires comes from the bar.
    """
    no_werror = FIXTURE_ADD_APP.replace("          -Werror\n", "")
    zero_budget = FIXTURE_ADD_APP.replace("-Wstack-usage=2048", "-Wstack-usage=0")
    no_fstack = FIXTURE_ADD_APP.replace("          -fstack-usage\n", "")
    m85_gains = FIXTURE_WARNINGS.replace(
        "                -Wconversion\n",
        "                -Wconversion\n                -Wshadow\n",
    )
    hand_spelled = FIXTURE_ADD_APP.replace(
        "      ra8_cpu1_warning_profile(_c1_warnings)\n",
        "      set(_c1_warnings -Wall -Wextra)\n",
    )
    no_profile_call = FIXTURE_ADD_APP.replace(
        "      ra8_cpu1_warning_profile(_c1fp_warnings)\n", ""
    )
    empty_profile = FIXTURE_ADD_APP.replace("          -Wall\n", "").replace(
        "          -Wextra\n", ""
    )
    for flag in ("-Werror", "-Wconversion", "-Wstack-usage=2048", "-fstack-usage"):
        empty_profile = empty_profile.replace(f"          {flag}\n", "")
    return [
        ("canonical pair is quiet", {}, ""),
        ("bar without -Werror fires", {"add_app": no_werror}, "no longer sets -Werror"),
        ("bar without -fstack-usage fires", {"add_app": no_fstack}, "-fstack-usage"),
        ("zero frame budget fires", {"add_app": zero_budget}, "positive integer literal"),
        ("an M85 flag missing from the M33 bar fires", {"warnings": m85_gains}, "-Wshadow"),
        (
            "a declared divergence covers that flag",
            {"warnings": m85_gains, "divergence": {"-Wshadow": "measured: reddens the M33 build"}},
            "",
        ),
        (
            "a divergence the bar now carries is stale",
            {"divergence": {"-Wconversion": "stale reason"}},
            "drop the declaration",
        ),
        (
            "a divergence the M85 set dropped is stale",
            {"divergence": {"-Wlogical-op": "stale reason"}},
            "no longer carries it either",
        ),
        ("a hand-spelled list at a caller fires", {"add_app": hand_spelled}, "spells warning flag"),
        (
            "a caller that never reads the profile fires",
            {"add_app": no_profile_call},
            "does not call",
        ),
        ("a flagless profile function fails closed", {"add_app": empty_profile}, "sets no "),
        (
            "a missing ra8_add_app.cmake fails closed",
            {"drop_cmake": ADD_APP_REL},
            "cannot be read",
        ),
        (
            "a missing ra8_warnings.cmake fails closed",
            {"drop_cmake": WARNINGS_REL},
            "cannot be read",
        ),
    ]


def build_bar_fixture(base: Path, add_app: str, warnings: str) -> None:
    """Write the two cmake files carrying the bar into a fixture tree."""
    for rel, text in ((ADD_APP_REL, add_app), (WARNINGS_REL, warnings)):
        path = base / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(textwrap.dedent(text), encoding="utf-8")
