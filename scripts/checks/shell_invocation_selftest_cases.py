# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Inert case data for the structural shell-caller policy selftest."""

from __future__ import annotations

from shell_entrypoint_policy import (
    ShellDialect,
    ShellPolicy,
    ShellSecurity,
    ShellUsage,
)

INSTALL = "scripts/ci/install_unicorn.sh"
INFRA = "infra/network/ap_openwrt.sh"
EXTENSIONLESS = "scripts/git/pre-commit"
SOURCED = "scripts/hil/lib/bench_lock.sh"
DUAL = "scripts/ci/lib/tool_env.sh"
RELEASE_LOADER_REL = "scripts/dev/provision_dev_box_toolchain.sh"
RELEASE_LOADER_CALL = (
    'source_release_selftest_helper_from "$main" "$helper" '
    '"$expected_dir" "$expected_digest" || return 1'
)
RELEASE_FIXTURE_REL = "scripts/dev/provision_dev_box_toolchain_selftest.bash"
RELEASE_FIXTURE_CALL = (
    'source_release_selftest_helper_from "$main" "$helper" "$directory" "$digest" || status=$?'
)


def _policy(
    security: ShellSecurity,
    usage: ShellUsage,
    *,
    executable: bool,
    privileged_parent: bool = False,
) -> ShellPolicy:
    """Build one compact inert typed-policy fixture."""
    return ShellPolicy(
        security,
        usage,
        ShellDialect.BASH,
        executable=executable,
        source_requires_privileged_parent=privileged_parent,
    )


POLICIES = {
    INSTALL: _policy(ShellSecurity.PRIVILEGED, ShellUsage.ENTRY, executable=True),
    INFRA: _policy(ShellSecurity.PRIVILEGED, ShellUsage.ENTRY, executable=True),
    EXTENSIONLESS: _policy(ShellSecurity.PRIVILEGED, ShellUsage.ENTRY, executable=True),
    SOURCED: _policy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.SOURCED_ONLY,
        executable=False,
        privileged_parent=True,
    ),
    DUAL: _policy(ShellSecurity.PORTABLE, ShellUsage.DUAL_USE, executable=False),
}
PRIVILEGED_CALLER = _policy(
    ShellSecurity.PRIVILEGED,
    ShellUsage.ENTRY,
    executable=True,
)
PORTABLE_CALLER = _policy(ShellSecurity.PORTABLE, ShellUsage.ENTRY, executable=True)
SOURCED_CALLER = _policy(
    ShellSecurity.PRIVILEGED,
    ShellUsage.SOURCED_ONLY,
    executable=False,
    privileged_parent=True,
)
UNBOUND_SOURCED_CALLER = _policy(
    ShellSecurity.PRIVILEGED,
    ShellUsage.SOURCED_ONLY,
    executable=False,
)
NONEXECUTABLE_CALLER = _policy(
    ShellSecurity.PRIVILEGED,
    ShellUsage.ENTRY,
    executable=False,
)
SH_CALLER = ShellPolicy(
    ShellSecurity.PRIVILEGED,
    ShellUsage.ENTRY,
    ShellDialect.POSIX_SH,
    executable=True,
    source_requires_privileged_parent=False,
)

RELEASE_LOADER_CASES = (
    (RELEASE_LOADER_REL, PRIVILEGED_CALLER, RELEASE_LOADER_CALL, False, "exact loader rejected"),
    (
        RELEASE_FIXTURE_REL,
        SOURCED_CALLER,
        RELEASE_FIXTURE_CALL,
        False,
        "exact fixture loader rejected",
    ),
    (
        RELEASE_LOADER_REL,
        PRIVILEGED_CALLER,
        f"source_release_selftest_helper_from() {{\n  :\n}}\n{RELEASE_LOADER_CALL}",
        False,
        "loader definition treated as a call",
    ),
    (
        "scripts/dev/other.sh",  # PATHREF-OK: synthetic wrong-loader fixture
        PRIVILEGED_CALLER,
        RELEASE_LOADER_CALL,
        True,
        "wrong file accepted",
    ),
    (RELEASE_LOADER_REL, None, RELEASE_LOADER_CALL, True, "wrong caller accepted"),
    (
        RELEASE_LOADER_REL,
        PORTABLE_CALLER,
        RELEASE_LOADER_CALL,
        True,
        "portable caller accepted",
    ),
    (
        RELEASE_LOADER_REL,
        SOURCED_CALLER,
        RELEASE_LOADER_CALL,
        True,
        "sourced-only caller accepted",
    ),
    (
        RELEASE_LOADER_REL,
        NONEXECUTABLE_CALLER,
        RELEASE_LOADER_CALL,
        True,
        "non-executable caller accepted",
    ),
    (RELEASE_LOADER_REL, SH_CALLER, RELEASE_LOADER_CALL, True, "non-Bash caller accepted"),
    (
        RELEASE_FIXTURE_REL,
        PRIVILEGED_CALLER,
        RELEASE_FIXTURE_CALL,
        True,
        "fixture entry caller accepted",
    ),
    (
        RELEASE_FIXTURE_REL,
        UNBOUND_SOURCED_CALLER,
        RELEASE_FIXTURE_CALL,
        True,
        "fixture unbound source parent accepted",
    ),
    (
        "scripts/dev/other_selftest.bash",  # PATHREF-OK: synthetic wrong-selftest fixture
        SOURCED_CALLER,
        RELEASE_FIXTURE_CALL,
        True,
        "fixture wrong file accepted",
    ),
    (
        RELEASE_FIXTURE_REL,
        SOURCED_CALLER,
        RELEASE_FIXTURE_CALL.replace("source_release_selftest_helper_from", "source"),
        True,
        "fixture wrong function accepted",
    ),
    (RELEASE_FIXTURE_REL, SOURCED_CALLER, "", True, "missing fixture loader call accepted"),
    (
        RELEASE_FIXTURE_REL,
        SOURCED_CALLER,
        f"{RELEASE_FIXTURE_CALL} extra",
        True,
        "extra fixture loader argument accepted",
    ),
    (
        RELEASE_FIXTURE_REL,
        SOURCED_CALLER,
        RELEASE_FIXTURE_CALL.replace('"$main" "$helper"', '"$helper" "$main"'),
        True,
        "reordered fixture loader arguments accepted",
    ),
    (
        RELEASE_FIXTURE_REL,
        SOURCED_CALLER,
        RELEASE_FIXTURE_CALL.replace('"$digest"', '"$helper"'),
        True,
        "substituted fixture loader argument accepted",
    ),
    (
        RELEASE_FIXTURE_REL,
        SOURCED_CALLER,
        RELEASE_FIXTURE_CALL.replace(' "$main"', ' \\\n  "$main"'),
        True,
        "continued fixture loader argv accepted",
    ),
    (
        RELEASE_FIXTURE_REL,
        SOURCED_CALLER,
        RELEASE_FIXTURE_CALL.replace("source_release_selftest_helper_from ", "$loader "),
        True,
        "indirect fixture loader command accepted",
    ),
    (
        RELEASE_LOADER_REL,
        PRIVILEGED_CALLER,
        RELEASE_LOADER_CALL.replace("source_release_selftest_helper_from", "source"),
        True,
        "wrong loader function accepted",
    ),
    (RELEASE_LOADER_REL, PRIVILEGED_CALLER, "", True, "missing loader call accepted"),
    (
        RELEASE_LOADER_REL,
        PRIVILEGED_CALLER,
        f"{RELEASE_LOADER_CALL} extra",
        True,
        "extra loader argument accepted",
    ),
    (
        RELEASE_LOADER_REL,
        PRIVILEGED_CALLER,
        RELEASE_LOADER_CALL.replace('"$main" "$helper"', '"$helper" "$main"'),
        True,
        "reordered loader arguments accepted",
    ),
    (
        RELEASE_LOADER_REL,
        PRIVILEGED_CALLER,
        RELEASE_LOADER_CALL.replace('"$expected_digest"', '"$helper"'),
        True,
        "substituted loader argument accepted",
    ),
    (
        RELEASE_LOADER_REL,
        PRIVILEGED_CALLER,
        f"{RELEASE_LOADER_CALL}\n{RELEASE_LOADER_CALL}",
        True,
        "duplicate loader call accepted",
    ),
    (
        RELEASE_FIXTURE_REL,
        SOURCED_CALLER,
        f"{RELEASE_FIXTURE_CALL}\n{RELEASE_FIXTURE_CALL}",
        True,
        "duplicate fixture loader call accepted",
    ),
    (
        RELEASE_LOADER_REL,
        PRIVILEGED_CALLER,
        RELEASE_LOADER_CALL.replace(' "$main"', ' \\\n  "$main"'),
        True,
        "continued loader argv accepted",
    ),
    (
        RELEASE_LOADER_REL,
        PRIVILEGED_CALLER,
        RELEASE_LOADER_CALL.replace("source_release_selftest_helper_from ", "$loader "),
        True,
        "indirect loader command accepted",
    ),
)

SHELL_CASES = (
    (f"/bin/bash -p {INSTALL}\n", 0, "exact privileged argv rejected"),
    (f"/bin/bash -p {INSTALL} --prefix /tmp/x\n", 0, "target arguments rejected"),
    (f"/bin/bash -p -- {INSTALL}\n", 1, "extra interpreter option accepted"),
    (f"./{INSTALL}\n", 0, "direct verified entry rejected"),
    (f"$ROOT/{INSTALL} --help\n", 0, "literal rooted direct entry rejected"),
    (f"/usr/bin/bash {INSTALL}\n", 1, "/usr/bin/bash accepted"),
    (f"/usr/local/bin/bash {INSTALL}\n", 1, "/usr/local/bin/bash accepted"),
    (f'"/bin/bash" {INSTALL}\n', 1, "quoted weak Bash accepted"),
    (f"/bin/ba\\\nsh {INSTALL}\n", 1, "split weak Bash accepted"),
    ("bash scripts/ci/\\\ninstall_unicorn.sh\n", 1, "split target accepted"),
    (
        f"/bin/bash -p {INSTALL}\nbash scripts/ci/\\\ninstall_unicorn.sh\n",
        1,
        "exact plus weak child accepted",
    ),
    (f'runner=/bin/bash; "$runner" {INSTALL}\n', 1, "variable Bash accepted"),
    (f'script={INSTALL}; /bin/bash -p "$script"\n', 1, "variable target accepted"),
    (f"/bin/bash -p {INFRA}\n", 0, "infra target rejected"),
    (f"/bin/bash -p {EXTENSIONLESS}\n", 0, "extensionless target rejected"),
    (f"/bin/bash -p {SOURCED}\n", 1, "sourced-only target executed"),
    (f"source {SOURCED}\n", 1, "unprivileged source parent accepted"),
    (f"source {DUAL}\n", 0, "portable dual-use source rejected"),
    (f"exec /bin/bash -p {INSTALL} --help\n", 0, "exec prefix rejected"),
    (f"FILES=(\n  {SOURCED}\n)\n", 0, "array data treated as a sourced-helper launch"),
    (
        'OWNER_BASH=/bin/bash\n[[ -x "$OWNER_BASH" ]]\n',
        0,
        "conditional path data treated as an indirect launch",
    ),
    (f"/bin/bash -p -n {INSTALL}\n", 1, "unregistered interpreter-option accepted"),
    (
        f"printf '%s\\n' item | xargs -P 2 -I{{}} /bin/bash -p {INSTALL} {{}}\n",
        0,
        "xargs protected child argv rejected",
    ),
    (
        f"printf '%s\\n' item | xargs -P 2 -I{{}} bash {INSTALL} {{}}\n",
        1,
        "xargs weak child argv accepted",
    ),
)

PYTHON_CASES = (
    (
        f'import subprocess\nsubprocess.run(["/bin/bash", "-p", "{INSTALL}"])\n',
        0,
        "safe Python argv rejected",
    ),
    (
        f'import subprocess\nsubprocess.run(["/bin/bash", "{INSTALL}"])\n',
        1,
        "weak Python argv accepted",
    ),
    (
        f'import subprocess\na=["/usr/bin/bash", "{INSTALL}"]\nsubprocess.run(a)\n',
        1,
        "Python argv dataflow accepted",
    ),
    (
        f'FIXTURE = "/usr/bin/bash {INSTALL}"\n',
        0,
        "inert Python fixture string treated as a process launch",
    ),
)

SURFACE_CASES = (
    (f'{{"command":"/bin/bash","args":["{INFRA}"]}}', "json", 1, "JSON weak"),
    (
        f'{{"command":"/bin/bash","args":["-p","{INFRA}"]}}',
        "json",
        0,
        "grouped JSON argv was re-scanned as detached args",
    ),
    (f"run: /usr/bin/bash {INSTALL}\n", "yaml", 1, "YAML weak argv accepted"),
    (
        f"run: >-\n  /bin/bash -p\n  {INSTALL}\n  --help\n",
        "yaml",
        0,
        "YAML folded exact privileged argv rejected",
    ),
    (
        f"run: >-\n  bash\n  {INSTALL}\n  --help\n",
        "yaml",
        1,
        "YAML folded weak argv accepted",
    ),
    (
        f"run: >-\n  /bin/bash -p\n  {{{{ (repo_dir ~ '/{INSTALL}') | quote }}}}\n",
        "yaml",
        0,
        "YAML quoted literal-path template rejected",
    ),
    (
        f"run: >-\n  bash\n  {{{{ (repo_dir ~ '/{INSTALL}') | quote }}}}\n",
        "yaml",
        1,
        "YAML weak templated argv accepted",
    ),
    (
        f"run: >-\n  /bin/bash -p\n  {{{{ (repo_dir ~ '/{INSTALL}') }}}}\n",
        "yaml",
        1,
        "YAML unquoted path template accepted",
    ),
    (
        f"description: /usr/bin/bash {INSTALL}\n",
        "yaml",
        0,
        "YAML data field treated as a command",
    ),
    (f"RUN /usr/local/bin/bash {INSTALL}\n", "docker", 1, "Docker weak argv"),
    (
        f"```shell\n/bin/bash {INSTALL}\n```\n",
        "markdown",
        1,
        "Markdown shell weak argv accepted",
    ),
    (
        f'```json\n{{"command":"/bin/bash","args":["{INFRA}"]}}\n```\n',
        "markdown",
        1,
        "Markdown JSON weak argv accepted",
    ),
    (
        f"The historical spelling `/usr/bin/bash {INSTALL}` is prose.\n",
        "markdown",
        0,
        "Markdown prose treated as executable",
    ),
)

JUST_CASES = (
    (
        f"safe image:\n    #!/bin/bash -p\n    source {SOURCED}\n",
        0,
        "privileged Just source context rejected",
    ),
    (
        f"weak image:\n    #!/usr/bin/env bash\n    source {SOURCED}\n",
        1,
        "portable Just source context accepted",
    ),
    (f"direct:\n    @/bin/bash -p {INSTALL}\n", 0, "Just quiet prefix broke exact argv"),
    (
        'clean := "/usr/bin/env -i PATH=/usr/bin:/bin"\n'
        f"setup:\n    {{{{ clean }}}} /bin/bash -p {INSTALL}\n",
        0,
        "Just clean-environment prefix broke exact protected argv",
    ),
)
