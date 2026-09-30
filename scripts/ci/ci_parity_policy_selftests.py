# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Policy-violation fixtures for ``check_ci_parity.py``.

The production checker owns the policy; this companion owns the synthetic
trees that prove each violation shape is still rejected.  It lives beside
``ci_parity_scan_selftest.py`` for the same two reasons: the checker stays
focused on enforcement, and production parity scans never load
temporary-fixture machinery.
"""

from __future__ import annotations

from pathlib import Path

from check_ci_parity import (
    FORBIDDEN_IN_INFRA,
    _check_infra_step,
    _check_managed_runner_dependencies,
    check_setup_just_policy,
    unregistered_gate_errors,
)


def _unregistered_gate_selftest() -> int:
    """Prove the registry-row direction catches a gate nothing can run.

    Both directions are asserted, and so is the marker's reason floor: a
    detector that stopped matching reports a clean tree, and so does an
    escape hatch that accepts anything.
    """
    import tempfile  # noqa: PLC0415  # selftest-only temporary fixtures

    cases: list[tuple[str, str, dict[str, str], bool]] = [
        (
            "registered gate passes",
            "gate_ascii() (\n  set -e\n)\n",
            {"ascii": "fast"},
            False,
        ),
        (
            "unregistered gate is caught",
            "gate_orphan() (\n  set -e\n)\n",
            {"ascii": "fast"},
            True,
        ),
        (
            "underscores map to the hyphenated row",
            "gate_arch_caps() (\n  set -e\n)\n",
            {"arch-caps": "fast"},
            False,
        ),
        (
            "declared-unregistered gate is allowed",
            "# ci-parity: unregistered -- zig/dev only, no row on this branch\n"
            "gate_orphan() (\n  set -e\n)\n",
            {"ascii": "fast"},
            False,
        ),
        (
            "marker with a token reason is rejected",
            "# ci-parity: unregistered -- wip\ngate_orphan() (\n  set -e\n)\n",
            {"ascii": "fast"},
            True,
        ),
        (
            "marker detached by a code line does not carry",
            "# ci-parity: unregistered -- a real reason, long enough\n"
            "some_other_thing() (\n  :\n)\n"
            "gate_orphan() (\n  set -e\n)\n",
            {"ascii": "fast"},
            True,
        ),
    ]
    failures = 0
    for label, body, registry, should_fail in cases:
        with tempfile.TemporaryDirectory() as tmp:
            gates = Path(tmp)
            (gates / "fixture.sh").write_text(body, encoding="utf-8")
            errors = unregistered_gate_errors(registry, gates_dir=gates)
        failed = bool(errors)
        status = "ok" if failed == should_fail else "FAIL"
        if failed != should_fail:
            failures += 1
        print(f"  [{status}] unregistered-gate: {label}")

    # A discovery floor: an empty gates tree must not read as clean.
    with tempfile.TemporaryDirectory() as tmp:
        errors = unregistered_gate_errors({"ascii": "fast"}, gates_dir=Path(tmp))
    status = "ok" if errors else "FAIL"
    if not errors:
        failures += 1
    print(f"  [{status}] unregistered-gate: empty gates tree refuses to report clean")
    return failures


def _infra_mention_selftest() -> int:
    """Prove a path a provisioning step only NAMES is not read as running it.

    Both directions, because the fix is a narrowing: an echoed path must stop
    firing, and every real invocation shape must keep firing.
    """
    prefix = "# ci-parity: infra -- installs the pinned compiler on the runner\n"
    cases = [
        ("echoed path is a mention", 'echo "    scripts/checks/check_zig_dist_pins.py"', False),
        ("printf'd path is a mention", "printf '%s\\n' 'scripts/checks/x.py'", False),
        ("indented echo inside a brace group", '{ echo "scripts/checks/x.py"; }', False),
        ("comment naming a path is a mention", "# see scripts/checks/x.py for the pin", False),
        ("real invocation still fires", "python3 scripts/checks/x.py", True),
        ("echo then a real invocation still fires", "echo hi && python3 scripts/checks/x.py", True),
        (
            "echo then invocation after a semicolon fires",
            "echo hi; python3 scripts/checks/x.py",
            True,
        ),
        ("quoted path given to another command fires", 'bash -c "scripts/checks/x.py"', True),
        ("host-test driver still fires", "tests/run_all.sh", True),
    ]
    failures = 0
    for label, body, should_fire in cases:
        errors = _check_infra_step("w", prefix + body, "installs the pinned compiler on the runner")
        fired = bool(errors)
        status = "ok" if fired == should_fire else "FAIL"
        if fired != should_fire:
            failures += 1
        print(f"  [{status}] infra-mention: {label}")
    return failures


def _infra_smuggling_selftest() -> int:
    """Prove an infra marker cannot hide either legacy or current checks."""
    prefix = "# ci-parity: infra -- pretends to be provisioning\n"
    cases = (
        ("a checker", prefix + "python3 scripts/checks/check_file_size.py"),
        ("a Just check", prefix + "just checks::local"),
    )
    failures = 0
    for label, body in cases:
        caught = any(pattern.search(body) for pattern, _ in FORBIDDEN_IN_INFRA)
        print(f"  [{'ok' if caught else 'FAIL'}] infra step smuggling {label} is rejected")
        if not caught:
            failures += 1
    return failures


def _setup_just_pin_selftest() -> int:
    """Prove setup-just is hosted-only, exactly pinned, and non-vacuous."""
    import tempfile  # noqa: PLC0415  # selftest-only temporary fixtures

    expected = "1.40.0"
    fixtures = (
        ("hosted exact pin", "ubuntu-latest", "with:\n          just-version: 1.40.0\n", False),
        ("hosted missing pin", "ubuntu-latest", "", True),
        (
            "hosted mismatched pin",
            "ubuntu-latest",
            "with:\n          just-version: 1.58.0\n",
            True,
        ),
    )
    failures = 0
    for label, runs_on, with_block, must_fire in fixtures:
        text = (
            "name: probe\n"
            "on: push\n"
            "jobs:\n"
            "  probe:\n"
            f"    runs-on: {runs_on}\n"
            "    steps:\n"
            "      - uses: extractions/setup-just@v3\n"
            f"        {with_block}"
        )
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / "probe.yml").write_text(text, encoding="utf-8")
            fired = bool(check_setup_just_policy(directory, expected))
        ok = fired == must_fire
        failures += 0 if ok else 1
        print(f"  [{'ok' if ok else 'FAIL'}] setup-just: {label}")

    failures += _setup_just_managed_selftest(expected)

    with tempfile.TemporaryDirectory() as tmp:
        fired = bool(check_setup_just_policy(Path(tmp), expected))
    ok = fired
    failures += 0 if ok else 1
    print(f"  [{'ok' if ok else 'FAIL'}] setup-just: empty action census is rejected")
    return failures


def _setup_just_managed_selftest(expected: str) -> int:
    """Prove Ansible-managed runners reject the hosted setup action."""
    import tempfile  # noqa: PLC0415  # selftest-only temporary fixtures

    failures = 0
    for label, runs_on in (
        ("managed ra8-ci action", "ra8-ci"),
        ("managed self-hosted action", "[self-hosted, hil, ra8d2]"),
    ):
        text = (
            "name: probe\n"
            "on: push\n"
            "jobs:\n"
            "  hosted:\n"
            "    runs-on: ubuntu-latest\n"
            "    steps:\n"
            "      - uses: extractions/setup-just@v3\n"
            "        with:\n"
            "          just-version: 1.40.0\n"
            "  managed:\n"
            f"    runs-on: {runs_on}\n"
            "    steps:\n"
            "      - uses: extractions/setup-just@v3\n"
            "        with:\n"
            "          just-version: 1.40.0\n"
        )
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / "probe.yml").write_text(text, encoding="utf-8")
            errors = check_setup_just_policy(directory, expected)
        fired = any("Ansible-managed runner" in error for error in errors)
        ok = fired and not any("no non-managed" in error for error in errors)
        failures += 0 if ok else 1
        print(f"  [{'ok' if ok else 'FAIL'}] setup-just: {label} is rejected")

    return failures


def _managed_runner_dependencies_selftest() -> int:
    """Prove only Ansible-managed jobs reject workflow-time provisioning."""
    cases = (
        (
            "ra8-ci apt install",
            {"runs-on": "ra8-ci", "steps": [{"run": "sudo apt-get install -y graphviz"}]},
            True,
        ),
        (
            "self-hosted setup-python",
            {
                "runs-on": ["self-hosted", "hil", "ra8d2"],
                "steps": [{"uses": "actions/setup-python@v5"}],
            },
            True,
        ),
        (
            "managed gate invocation",
            {
                "runs-on": "ra8-ci",
                "steps": [{"run": "just quality::local::gate lint-yaml"}],
            },
            False,
        ),
        (
            "hosted fork provisioning",
            {
                "runs-on": "ubuntu-latest",
                "steps": [{"run": "sudo apt-get install -y clang-format-22"}],
            },
            False,
        ),
    )
    failures = 0
    for label, job, must_fire in cases:
        fired = bool(_check_managed_runner_dependencies("probe", job))
        ok = fired == must_fire
        failures += 0 if ok else 1
        print(f"  [{'ok' if ok else 'FAIL'}] managed runner: {label}")
    return failures
