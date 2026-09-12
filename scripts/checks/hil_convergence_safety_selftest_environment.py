# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Environment and runner mutation cases for the HIL safety self-test."""

from __future__ import annotations

from collections.abc import Callable

Scan = Callable[[dict[str, str]], list[str]]
Mutate = Callable[[dict[str, str], str, str, str], dict[str, str]]
RemoveManifestMember = Callable[[dict[str, str], str], dict[str, str]]
RemoveLoopMember = Callable[[dict[str, str], str, str, str], dict[str, str]]


class EnvironmentCaseError(ValueError):
    """A structural environment self-test no longer has its expected cases."""


def runner_runtime_directory_cases(
    inputs: dict[str, str], scan: Scan, mutate: Mutate
) -> list[tuple[str, bool]]:
    """Return runtime-directory validation and allowlist mutations."""
    mutations = (
        (
            "relative Ansible runtime directory acceptance",
            "if not path.is_absolute():",
            "if False:",
        ),
        (
            "Ansible runtime directory owner-check removal",
            "metadata.st_uid != os.getuid()",
            "False",
        ),
        (
            "Ansible runtime directory mode-check removal",
            "stat.S_IMODE(metadata.st_mode) != PRIVATE_DIRECTORY_MODE",
            "False",
        ),
        (
            "private Ansible runtime mode widening",
            "PRIVATE_DIRECTORY_MODE = 0o700",
            "PRIVATE_DIRECTORY_MODE = 0o755",
        ),
        (
            "Ansible runtime environment allowlist widening",
            '("ANSIBLE_LOCAL_TEMP", "ANSIBLE_SSH_CONTROL_PATH_DIR")',
            '("ANSIBLE_LOCAL_TEMP", "ANSIBLE_SSH_CONTROL_PATH_DIR", "TMPDIR")',
        ),
        (
            "Ansible runtime directory validator bypass",
            "value = _private_runtime_directory(environment, key)",
            "value = environment.get(key)",
        ),
    )
    return [
        (label, bool(scan(mutate(inputs, "fleet_runner", old, new))))
        for label, old, new in mutations
    ]


def _environment_variable_cases(
    inputs: dict[str, str], scan: Scan, mutate: Mutate
) -> list[tuple[str, bool]]:
    """Return environment-variable sanitizer mutations."""
    return [
        (
            "HIL caller-selected tool venv fires",
            bool(
                scan(
                    mutate(
                        inputs,
                        "hil_just",
                        'export RA8_TOOL_VENV := ""',
                        'export RA8_TOOL_VENV := env("RA8_TOOL_VENV", "")',
                    )
                )
            ),
        ),
        (
            "Ansible setup sanitizer removal fires",
            bool(
                scan(
                    mutate(
                        inputs,
                        "setup_ansible",
                        "unset PYTHONHOME PYTHONPATH RA8_TOOL_VENV",
                        ": # sanitizer removed",
                    )
                )
            ),
        ),
        (
            "toolchain provision TMPDIR sanitizer removal fires",
            bool(
                scan(
                    mutate(
                        inputs,
                        "provision_toolchain",
                        "unset PYTHONHOME PYTHONPATH RA8_TOOL_VENV TMPDIR",
                        "unset PYTHONHOME PYTHONPATH RA8_TOOL_VENV",
                    )
                )
            ),
        ),
    ]


def _environment_path_cases(
    inputs: dict[str, str], scan: Scan, mutate: Mutate
) -> list[tuple[str, bool]]:
    """Return fixed-path interpreter and shell mutations."""
    return [
        (
            "HIL caller PATH inheritance fires",
            bool(
                scan(
                    mutate(
                        inputs,
                        "hil_just",
                        "export PATH := `/bin/bash -p "
                        "scripts/ci/lib/host_tool_path.sh --print-path`",
                        'export PATH := env("PATH", "")',
                    )
                )
            ),
        ),
        (
            "toolchain provision PATH poisoning fires",
            bool(
                scan(
                    mutate(
                        inputs,
                        "provision_toolchain",
                        "PATH=/usr/local/bin:/usr/bin:/bin",
                        "PATH=${PATH}",
                    )
                )
            ),
        ),
        (
            "infra bootstrap PATH Bash fires",
            bool(
                scan(
                    mutate(
                        inputs,
                        "infra_bootstrap",
                        '/bin/bash -p "${ROOT}/scripts/dev/setup_ansible.sh"',
                        'bash "${ROOT}/scripts/dev/setup_ansible.sh"',
                    )
                )
            ),
        ),
    ]


def _environment_sanitizer_cases(
    inputs: dict[str, str], scan: Scan, mutate: Mutate
) -> list[tuple[str, bool]]:
    """Return every public environment-sanitizer mutation."""
    return _environment_variable_cases(inputs, scan, mutate) + _environment_path_cases(
        inputs, scan, mutate
    )


def _authenticated_uv_runner_cases(
    inputs: dict[str, str], scan: Scan, mutate: Mutate
) -> list[tuple[str, bool]]:
    """Return authenticated uv-runner mutations."""
    return [
        (
            "infra authenticated uv runner removal fires",
            bool(
                scan(
                    mutate(
                        inputs,
                        "infra_sh",
                        "--run --no-config sync",
                        "--no-config sync",
                    )
                )
            ),
        ),
        (
            "WSL authenticated uv runner removal fires",
            bool(
                scan(
                    mutate(
                        inputs,
                        "fleet_wsl",
                        '--run "$@"',
                        '--verify-cache "$@"',
                    )
                )
            ),
        ),
    ]


def _container_helper_cases(
    inputs: dict[str, str], scan: Scan, mutate: Mutate
) -> list[tuple[str, bool]]:
    """Return devcontainer uv-helper mutations."""
    return [
        (
            "devcontainer uv helper allowlist removal fires",
            bool(
                scan(
                    mutate(
                        inputs,
                        "dockerignore",
                        "!scripts/dev/bootstrap_uv_exec.py\n",
                        "",
                    )
                )
            ),
        ),
        (
            "devcontainer uv helper COPY removal fires",
            bool(
                scan(
                    mutate(
                        inputs,
                        "dockerfile",
                        "     scripts/dev/bootstrap_uv_exec.py \\\n",
                        "",
                    )
                )
            ),
        ),
        (
            "devcontainer uv helper canonical-input removal fires",
            bool(
                scan(
                    mutate(
                        inputs,
                        "devcontainer_image",
                        "644 scripts/dev/bootstrap_uv_exec.py\n",
                        "",
                    )
                )
            ),
        ),
    ]


def _runner_helper_cases(
    inputs: dict[str, str],
    scan: Scan,
    remove_manifest_member: RemoveManifestMember,
    remove_loop_member: RemoveLoopMember,
) -> list[tuple[str, bool]]:
    """Return HIL and CI runner uv-helper mutations."""
    return [
        (
            "HIL uv helper staging removal fires",
            bool(scan(remove_manifest_member(inputs, "bootstrap_uv_exec.py"))),
        ),
        (
            "CI runner uv helper readback removal fires",
            bool(
                scan(
                    remove_loop_member(
                        inputs,
                        "ci_runner",
                        "Read back every staged root-context authority byte-for-byte",
                        "scripts/dev/bootstrap_uv_exec.py",
                    )
                )
            ),
        ),
        (
            "CI runner uv helper presence-proof removal fires",
            bool(
                scan(
                    remove_loop_member(
                        inputs,
                        "ci_runner",
                        "Assert both Dockerfiles and every locked Python input arrived",
                        "scripts/dev/bootstrap_uv_exec.py",
                    )
                )
            ),
        ),
    ]


def _wsl_helper_cases(inputs: dict[str, str], scan: Scan, mutate: Mutate) -> list[tuple[str, bool]]:
    """Return WSL uv-helper archive and path-proof mutations."""
    return [
        (
            "WSL uv helper archive removal fires",
            bool(
                scan(
                    mutate(
                        inputs,
                        "fleet_wsl_stage",
                        '    "scripts/dev/bootstrap_uv_exec.py",\n',
                        "",
                    )
                )
            ),
        ),
        (
            "WSL uv helper path-proof removal fires",
            bool(
                scan(
                    mutate(
                        inputs,
                        "fleet_wsl",
                        '        f"{stage}/scripts/dev/bootstrap_uv_exec.py",\n',
                        "",
                    )
                )
            ),
        ),
    ]


def cases(
    inputs: dict[str, str],
    scan: Scan,
    mutate: Mutate,
    remove_manifest_member: RemoveManifestMember,
    remove_loop_member: RemoveLoopMember,
) -> list[tuple[str, bool]]:
    """Return public environment and installer-boundary mutations."""
    sanitizer_cases = _environment_sanitizer_cases(inputs, scan, mutate)
    uv_runner_cases = _authenticated_uv_runner_cases(inputs, scan, mutate)
    container_cases = _container_helper_cases(inputs, scan, mutate)
    runner_cases = _runner_helper_cases(inputs, scan, remove_manifest_member, remove_loop_member)
    wsl_cases = _wsl_helper_cases(inputs, scan, mutate)
    if not all((sanitizer_cases, uv_runner_cases, container_cases, runner_cases, wsl_cases)):
        message = "environment case helper returned no mutation cases"
        raise EnvironmentCaseError(message)
    return sanitizer_cases + uv_runner_cases + container_cases + runner_cases + wsl_cases
