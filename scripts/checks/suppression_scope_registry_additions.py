# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Reviewed authority classifications added by the 2026-08 suppression audit.

The reviewed identities are held in bounded per-subtype helpers so no single
function exceeds the NASA Power of 10 Rule 4 length cap. ``additional_schema_groups``
concatenates them in the original order, so the value it returns is unchanged.
"""

from __future__ import annotations


def _identities(value: str) -> tuple[str, ...]:
    """Split one whitespace-separated identity block into exact entries."""
    return tuple(value.split())


def _positive_scope_literal_1_p1() -> tuple[str, ...]:
    """Reviewed positive-scope identities (literal), part 1 of 2."""
    return _identities(
        """
        checks/check_chapter_map_freshness.py:ARTEFACT
        checks/check_chapter_map_freshness.py:GENERATOR
        checks/check_chapter_map_freshness.py:SOURCE_PDF
        checks/check_hil_privilege_boundary.py:CALLER_PATHS
        checks/check_hil_privilege_boundary.py:HELPER_REL
        checks/check_hil_privilege_boundary.py:MANIFEST_REL
        checks/check_hil_privilege_boundary.py:POLICY_TEMPLATE_REL
        checks/check_hil_privilege_boundary.py:ROLE_ENTRY_REL
        checks/check_hil_privilege_boundary.py:ROLE_REL
        checks/check_hil_privilege_boundary.py:WORKFLOW_PATHS
        checks/check_hil_rig_contract.py:ANSIBLE
        checks/check_hil_rig_contract.py:BENCH_ANSIBLE
        checks/check_hil_rig_contract.py:CONTRACT
        checks/check_hil_rig_contract.py:ETH_SCRIPT
        checks/check_hil_rig_contract.py:EXAMPLE
        checks/check_hil_rig_contract.py:GATE
        checks/check_hil_rig_contract.py:HIL_DOC
        checks/check_hil_rig_contract.py:HIL_SUITE
        checks/check_hil_rig_contract.py:PARSER
        checks/check_hil_rig_contract.py:REMOTE_GDB_ARGS
        checks/check_hil_rig_contract.py:RIG_ENV
        checks/check_hook_parity.py:BOOTSTRAP_REQUIREMENTS
        checks/check_hook_parity.py:CANDIDATE_BOUNDARY_MODULES
        checks/check_hook_parity.py:MUTATION_HELPER
        checks/check_hook_parity.py:TRUSTED_MUTATIONS
        checks/check_shell_just_invocations.py:ROOT_JUSTFILE
        checks/check_shell_just_invocations.py:WORKSPACE_JUSTFILE
        checks/hil_cache_repair_rules.py:ENTRYPOINT_POLICY
        checks/hil_cache_repair_rules.py:SHEBANG_CHECKER
        checks/hil_convergence_safety_policy.py:AD2_ENTRY
        checks/hil_convergence_safety_policy.py:AD2_ROLE
        checks/hil_convergence_safety_policy.py:BASE_WORKFLOW_PATHS
        checks/hil_convergence_safety_policy.py:BENCH_DEFAULTS
        checks/hil_convergence_safety_policy.py:BENCH_ENTRY
        checks/hil_convergence_safety_policy.py:BENCH_GUARD
        checks/hil_convergence_safety_policy.py:BENCH_ROLE
        """
    )


def _positive_scope_literal_1_p2() -> tuple[str, ...]:
    """Reviewed positive-scope identities (literal), part 2 of 2."""
    return _identities(
        """
        checks/hil_convergence_safety_policy.py:C6_ENTRY
        checks/hil_convergence_safety_policy.py:C6_ROLE
        checks/hil_convergence_safety_policy.py:DECLARATION
        checks/hil_convergence_safety_policy.py:DEV_ENTRY
        checks/hil_convergence_safety_policy.py:DEV_GUARD
        checks/hil_convergence_safety_policy.py:DEV_HANDLER
        checks/hil_convergence_safety_policy.py:DEV_IMAGE_LOCK
        checks/hil_convergence_safety_policy.py:DEV_MAIN
        checks/hil_convergence_safety_policy.py:DEV_MAIN_ENTRY
        checks/hil_convergence_safety_policy.py:DEV_ROLE
        checks/hil_convergence_safety_policy.py:DIRECT_DEPENDENCIES
        checks/hil_convergence_safety_policy.py:FLEET
        checks/hil_convergence_safety_policy.py:FLEET_BENCH
        checks/hil_convergence_safety_policy.py:FLEET_MODEL
        checks/hil_convergence_safety_policy.py:FLEET_PATH_AUTHORITY
        checks/hil_convergence_safety_policy.py:FLEET_REACH
        checks/hil_convergence_safety_policy.py:FLEET_RUNNER
        checks/hil_convergence_safety_policy.py:FLEET_RUNNER_MODEL
        checks/hil_convergence_safety_policy.py:FLEET_WSL
        checks/hil_convergence_safety_policy.py:FLEET_WSL_STAGE
        checks/hil_convergence_safety_policy.py:GATE
        checks/hil_convergence_safety_policy.py:HIL_JUST
        checks/hil_convergence_safety_policy.py:IDLE_HELPER
        checks/hil_convergence_safety_policy.py:PLAYBOOKS
        checks/hil_convergence_safety_policy.py:WORKFLOW
        checks/hil_convergence_safety_v9.py:DEV_DIR
        checks/hil_convergence_safety_v9.py:HIL_LIB
        checks/markdown_reference_policy.py:AUTHORED_VENDOR_INDEXES
        checks/markdown_reference_policy.py:QUALIFICATION_RELEASE_SOURCES
        checks/markdown_reference_policy.py:REPO_PREFIXES
        checks/markdown_reference_policy.py:TOOL_PRIVATE_OWNERSHIP_INDEXES
        checks/markdown_reference_policy.py:TOOL_PRIVATE_VENDOR_SOURCES
        """
    )


def _positive_scope_string_leaves_2_p1() -> tuple[str, ...]:
    """Reviewed positive-scope identities (string-leaves), part 1 of 1."""
    return _identities(
        """
        checks/shell_entrypoint_policy.py:_BASE_SHELL_POLICIES
        checks/shell_entrypoint_policy_ci.py:CI_POLICY_ROWS
        checks/shell_entrypoint_policy_hil.py:HIL_POLICY_ROWS
        """
    )


def _anti_vacuity_floor_literal_3_p1() -> tuple[str, ...]:
    """Reviewed anti-vacuity-floor identities (literal), part 1 of 1."""
    return _identities(
        """
        checks/check_chapter_map_freshness.py:EXPECTED_CHAPTERS
        checks/check_third_party_patches.py:MIN_ABBREV
        checks/check_hil_convergence_safety.py:CONTROLLER_AUTH_ARGC
        checks/check_hil_convergence_safety.py:EXPECTED_APT_INDEX
        checks/check_hil_convergence_safety.py:REMOTE_VERIFY_ARGC
        checks/check_hil_convergence_safety.py:REQUIRED_THAW_CALLS
        checks/check_hil_privilege_boundary.py:SPAWN_ARGC
        checks/check_shell_just_invocations.py:MIN_CALLER_FILES
        checks/check_shell_just_invocations.py:MIN_PROTECTED_SCRIPTS
        checks/hil_convergence_safety_roles.py:FLEET_PAYLOAD_KEYS
        checks/hil_convergence_safety_roles.py:LOCAL_AUTH_ARGC
        checks/hil_convergence_safety_roles.py:REMOTE_VERIFY_ARGC
        checks/hil_convergence_safety_roles.py:ROLE_PREFIX_LENGTH
        checks/hil_privileged_helper_selftest.py:EXPECTED_NETWORK_MUTATIONS
        checks/markdown_reference_policy.py:MIN_FIRST_PARTY_MARKDOWN
        checks/markdown_reference_policy.py:MIN_LINK_REFERENCES
        checks/markdown_reference_policy.py:MIN_PATH_REFERENCES
        checks/markdown_reference_policy.py:MIN_TRACKED_MARKDOWN
        checks/markdown_reference_policy.py:MIN_VENDOR_MARKDOWN
        """
    )


def _allowed_token_literal_4_p1() -> tuple[str, ...]:
    """Reviewed allowed-token identities (literal), part 1 of 1."""
    return _identities(
        """
        checks/check_hil_rig_contract.py:FIELDS
        checks/check_justfiles.py:CI_SH_CALL_RE
        checks/check_justfiles.py:CI_SH_SWITCHES
        checks/check_justfiles.py:CI_SH_VALUE_OPTIONS
        checks/check_justfiles.py:NATIVE_FAST_COMMAND
        checks/check_justfiles.py:NATIVE_FAST_RECIPE_RE
        checks/check_python_lock_policy.py:INTERPRETER_IMPORT_ROOTS
        checks/check_shell_just_invocations.py:PROTECTED_REASON
        checks/check_shell_just_invocations.py:PROTECTED_SHEBANG
        checks/markdown_reference_policy.py:COMPONENT_RELATIVE_PREFIXES
        checks/markdown_reference_policy.py:DECLARED_BARE_CODE_FILES
        checks/markdown_reference_policy.py:ROOT_FILE_TOKENS
        checks/markdown_reference_policy.py:SYSTEM_HEADER_BASENAMES
        checks/shell_entrypoint_policy.py:PORTABLE_SHEBANG
        checks/shell_entrypoint_policy.py:PORTABLE_SH_SHEBANG
        checks/shell_entrypoint_policy.py:PRIVILEGED_REASON
        checks/shell_entrypoint_policy.py:PRIVILEGED_SHEBANG
        """
    )


def _suppression_control_plane_literal_5_p1() -> tuple[str, ...]:
    """Reviewed suppression-control-plane identities (literal), part 1 of 1."""
    return _identities(
        """
        checks/check_hil_privilege_boundary.py:EXACT_POLICY_TEMPLATE
        checks/check_hil_privilege_boundary.py:LOAD_POLICY_DEAD_NOFOLLOW
        checks/check_hil_privilege_boundary.py:LOAD_POLICY_NOFOLLOW
        checks/check_hil_privilege_boundary.py:LOAD_POLICY_PREFIX
        checks/check_shebangs.py:FAILED_CLEANUP_EXEC
        checks/check_shebangs.py:FORBIDDEN_REEXEC_TOKENS
        checks/hil_convergence_safety_roles.py:BENCH_HOLDER_DECISION
        checks/hil_convergence_safety_roles.py:BENCH_HOLDER_FILE_PROOF
        checks/suppression_checker_census.py:FIXTURE_OWNER_RELATIVE_PATHS
        checks/markdown_reference_policy.py:DECLARED_BARE_CONTEXT_SHA256
        """
    )


def _suppression_control_plane_string_leaves_6_p1() -> tuple[str, ...]:
    """Reviewed suppression-control-plane identities (string-leaves), part 1 of 1."""
    return _identities(
        """
        checks/check_shebangs.py:PINNED_INTERPRETER_BOUNDARIES
        checks/check_shebangs.py:PRIVILEGED_BODY_CLOSE
        checks/check_shebangs.py:PRIVILEGED_BODY_OPEN
        checks/check_shebangs.py:PRIVILEGED_BODY_PREFIX
        checks/check_shebangs.py:PRIVILEGED_DUAL_BODY_PREFIX
        checks/check_shell_just_invocations.py:SENSITIVE_BOUNDARY_LINES
        checks/hil_cache_repair_rules.py:PINNED_CHECKER_OCCURRENCES
        checks/shell_invocation_references.py:EXACT_REFERENCES
        """
    )


def _allowed_token_expression_digest_4_p2() -> tuple[str, ...]:
    """Reviewed allowed-token identities whose exact expression is authenticated."""
    return _identities(
        """
        checks/download_installers_macos.py:ALLOWED_INDIRECT_EXECUTION
        """
    )


def _suppression_control_plane_expression_digest_6_p2() -> tuple[str, ...]:
    """Reviewed security expressions whose dependencies are authorities too."""
    return _identities(
        """
        checks/check_shebangs.py:PRIVILEGED_RIG_BODY_PREFIX
        checks/check_shebangs.py:PRIVILEGED_RUNTIME_VARIANTS
        checks/download_installers_macos.py:_REEXEC_START
        checks/download_installers_macos.py:_REEXEC_LINES
        checks/download_installers_macos.py:_REEXEC_LITERAL
        checks/download_installers_macos.py:_REEXEC_SOURCE
        checks/download_installers_macos.py:_REEXEC_WORDS
        checks/download_installers_macos.py:_REEXEC_RECORD
        """
    )


def _final_positive_scope_literal_8_p1() -> tuple[str, ...]:
    """Reviewed final-integration path authorities."""
    return _identities(
        """
        checks/python_lock_policy_uv_execution.py:DEPLOYMENT_CLOSURE_PATHS
        checks/python_lock_policy_uv_cache_release.py:RELEASE_SELFTEST_PATH
        """
    )


def _final_positive_scope_string_leaves_8_p2() -> tuple[str, ...]:
    """Reviewed fixed runtime-root authorities used by the image selftests."""
    return _identities(
        """
        ci/devcontainer_image_selftest_supervisor.py:CANONICAL_TMP
        ci/devcontainer_image_selftest_supervisor_cases.py:CANONICAL_TMP
        """
    )


def _final_anti_vacuity_literal_9_p1() -> tuple[str, ...]:
    """Reviewed final-integration anti-vacuity floors."""
    return _identities(
        """
        checks/python_lock_policy_scan.py:MIN_UV_ARGV_SIZE
        checks/python_lock_policy_uv_cache_release.py:EXPECTED_PROVISIONER_UV_RUNS
        checks/python_lock_policy_uv_cache_release.py:EXPECTED_SELFTEST_DISPATCH_CALLS
        checks/hil_convergence_safety_raw_digest_runtime.py:_FAILURE_ATTEMPTS
        """
    )


def _final_control_plane_string_leaves_10_p1() -> tuple[str, ...]:
    """Reviewed exact argv and digest authorities from the final UV policy."""
    return _identities(
        """
        checks/python_lock_policy_scan.py:HIL_UV_AUTH_ARGV
        checks/python_lock_policy_scan.py:HIL_UV_PROBE_ARGV
        checks/python_lock_policy_scan.py:HIL_UV_SYNC_ARGV
        checks/python_lock_policy_uv_cache.py:BOOTSTRAP_MODULE_SHA256
        checks/python_lock_policy_uv_cache.py:MODE_TEST_MODULE_SHA256
        checks/python_lock_policy_uv_cache.py:PORTABLE_REGISTRY_DIGEST
        checks/python_lock_policy_uv_cache.py:MODE_EXECUTION_REGISTRY_DIGEST
        checks/python_lock_policy_uv_execution.py:EXEC_MODULE_SHA256
        checks/python_lock_policy_uv_execution.py:RUNNER_MODULE_SHA256
        checks/shell_invocation_policy.py:RELEASE_LOADER_NAME
        checks/shell_invocation_policy.py:RELEASE_LOADER_MAIN_REL
        checks/shell_invocation_policy.py:RELEASE_LOADER_MAIN_LOGICAL
        checks/shell_invocation_policy.py:RELEASE_LOADER_FIXTURE_REL
        checks/shell_invocation_policy.py:RELEASE_LOADER_FIXTURE_LOGICAL
        """
    )


def _final_control_plane_expression_digest_10_p2() -> tuple[str, ...]:
    """Reviewed exact semantic-contract expressions from the final UV policy."""
    return _identities(
        """
        checks/check_devcontainer.py:EXPECTED_UV_RUN_BLOCK
        checks/python_lock_policy_uv_execution.py:EXEC_EXACT_BODIES
        checks/python_lock_policy_uv_execution.py:EXEC_EXACT_FUNCTION_DIGESTS
        checks/python_lock_policy_uv_execution.py:EXEC_EXACT_ASSIGNMENTS
        checks/python_lock_policy_uv_execution.py:EXEC_CALL_CONTRACTS
        checks/python_lock_policy_uv_cache_release_contracts.py:PYTHON_BYTECODE_NEGATIVE_CONTROL_CONTRACT
        checks/python_lock_policy_uv_cache_release_contracts.py:PYTHON_SELFTEST_TMP_CONTRACTS
        checks/python_lock_policy_uv_cache_release_contracts.py:PYTHON_NO_BYTECODE_RESIDUE_CONTRACT
        checks/shell_invocation_policy.py:RELEASE_LOADER_GRAMMARS
        """
    )


def _final_security_raw_byte_literal_10_p3() -> tuple[str, ...]:
    """Return reviewed raw-byte and fixed-executable security authorities."""
    return _identities(
        """
        checks/hil_convergence_safety_image_lock_digest.py:DEVCONTAINER_IMAGE_PATH
        checks/hil_convergence_safety_image_lock_digest.py:DEVCONTAINER_IMAGE_RAW_SHA256
        checks/hil_convergence_safety_image_lock_digest.py:DEVCONTAINER_IMAGE_LOCK_RECEIPTS_PATH
        checks/hil_convergence_safety_image_lock_digest.py:DEVCONTAINER_IMAGE_LOCK_RECEIPTS_RAW_SHA256
        checks/hil_convergence_safety_image_lock_digest.py:DEVCONTAINER_IMAGE_LOCK_SELFTEST_PATH
        checks/hil_convergence_safety_image_lock_digest.py:DEVCONTAINER_IMAGE_LOCK_SELFTEST_RAW_SHA256
        checks/hil_convergence_safety_image_lock_digest.py:DEVCONTAINER_IMAGE_SELFTEST_PATH
        checks/hil_convergence_safety_image_lock_digest.py:DEVCONTAINER_IMAGE_SELFTEST_RAW_SHA256
        checks/hil_convergence_safety_image_lock_digest.py:DEVCONTAINER_IMAGE_BOUND_EXIT_SELFTEST_PATH
        checks/hil_convergence_safety_image_lock_digest.py:DEVCONTAINER_IMAGE_BOUND_EXIT_SELFTEST_RAW_SHA256
        checks/hil_convergence_safety_image_lock_digest.py:DEVCONTAINER_IMAGE_SELFTEST_CASES_PATH
        checks/hil_convergence_safety_image_lock_digest.py:DEVCONTAINER_IMAGE_SELFTEST_CASES_RAW_SHA256
        checks/hil_convergence_safety_image_lock_digest.py:DEVCONTAINER_IMAGE_SIGNAL_SELFTEST_PATH
        checks/hil_convergence_safety_image_lock_digest.py:DEVCONTAINER_IMAGE_SIGNAL_SELFTEST_RAW_SHA256
        checks/hil_convergence_safety_image_lock_digest.py:DEVCONTAINER_IMAGE_SELFTEST_PROCESS_PATH
        checks/hil_convergence_safety_image_lock_digest.py:DEVCONTAINER_IMAGE_SELFTEST_PROCESS_RAW_SHA256
        checks/hil_convergence_safety_image_lock_digest.py:DEVCONTAINER_IMAGE_SELFTEST_SUPERVISOR_PATH
        checks/hil_convergence_safety_image_lock_digest.py:DEVCONTAINER_IMAGE_SELFTEST_SUPERVISOR_RAW_SHA256
        checks/hil_convergence_safety_image_lock_digest.py:DEVCONTAINER_IMAGE_SELFTEST_SUPERVISOR_CASES_PATH
        checks/hil_convergence_safety_image_lock_digest.py:DEVCONTAINER_IMAGE_SELFTEST_SUPERVISOR_CASES_RAW_SHA256
        checks/hil_convergence_safety_image_lock_digest.py:_DIRECTORY_MODE
        checks/hil_convergence_safety_image_lock_digest.py:_ROOT_MODE_REQUIRED
        checks/hil_convergence_safety_image_lock_digest.py:_ROOT_MODE_ALLOWED
        checks/hil_convergence_safety_image_lock_digest.py:_EXECUTABLE_MODE
        checks/hil_convergence_safety_image_lock_digest.py:_SOURCE_MODE
        checks/hil_convergence_safety_image_lock_digest.py:_MAX_AUTHORITY_BYTES
        checks/hil_convergence_safety_image_lock_digest.py:_READ_STEP_BYTES
        checks/hil_convergence_safety_image_lock_digest.py:_FORBIDDEN_RELATIVE_PARTS
        checks/hil_convergence_safety_image_lock_digest.py:_FIXED_PATH_BY_PIN
        checks/hil_convergence_safety_image_lock_digest.py:RAW_DIGEST_CONTROLS_PATH
        checks/hil_convergence_safety_image_lock_digest.py:RAW_DIGEST_CONTROLS_RAW_SHA256
        ci/devcontainer_image_selftest_supervisor.py:CASES_RAW_SHA256
        ci/devcontainer_image_selftest_supervisor.py:PROCESS_RAW_SHA256
        checks/hil_convergence_safety_runtime_launcher.py:PYTHON_INTERPRETER
        """
    )


def _vendor_exemption_literal_7_p1() -> tuple[str, ...]:
    """Reviewed vendor-exemption identities (literal), part 1 of 1."""
    return _identities(
        """
        checks/markdown_reference_policy.py:LIBWEBP_ABSENCE_CLAUSE
        checks/markdown_reference_policy.py:SOUP_DECLARED_ABSENCES
        checks/markdown_reference_policy.py:VENDOR_PREFIXES
        """
    )


def _regex_exclusion_literal_8_p1() -> tuple[str, ...]:
    """Reviewed regex-exclusion identities (literal), part 1 of 1."""
    return _identities(
        """
        checks/markdown_reference_policy.py:TOOL_PRIVATE_CLAUSE_PATTERNS
        """
    )


def _known_gap_literal_9_p1() -> tuple[str, ...]:
    """Reviewed known-gap identities (literal), part 1 of 1."""
    return _identities(
        """
        checks/markdown_reference_policy.py:WORK_FIXTURE_PATH
        """
    )


def _base_review_schema_groups() -> tuple[tuple[str, str, tuple[str, ...]], ...]:
    """Return the base suppression-audit schema groups in reviewed order."""
    return (
        (
            "positive-scope",
            "literal",
            _positive_scope_literal_1_p1() + _positive_scope_literal_1_p2(),
        ),
        (
            "positive-scope",
            "string-leaves",
            _positive_scope_string_leaves_2_p1(),
        ),
        (
            "anti-vacuity-floor",
            "literal",
            _anti_vacuity_floor_literal_3_p1(),
        ),
        (
            "allowed-token",
            "literal",
            _allowed_token_literal_4_p1(),
        ),
        (
            "allowed-token",
            "expression-digest",
            _allowed_token_expression_digest_4_p2(),
        ),
        (
            "suppression-control-plane",
            "literal",
            _suppression_control_plane_literal_5_p1(),
        ),
        (
            "suppression-control-plane",
            "string-leaves",
            _suppression_control_plane_string_leaves_6_p1(),
        ),
        (
            "suppression-control-plane",
            "expression-digest",
            _suppression_control_plane_expression_digest_6_p2(),
        ),
    )


def _final_review_schema_groups() -> tuple[tuple[str, str, tuple[str, ...]], ...]:
    """Return the final-integration schema groups in reviewed order."""
    return (
        (
            "positive-scope",
            "literal",
            _final_positive_scope_literal_8_p1(),
        ),
        (
            "positive-scope",
            "string-leaves",
            _final_positive_scope_string_leaves_8_p2(),
        ),
        (
            "anti-vacuity-floor",
            "literal",
            _final_anti_vacuity_literal_9_p1(),
        ),
        (
            "suppression-control-plane",
            "string-leaves",
            _final_control_plane_string_leaves_10_p1(),
        ),
        (
            "suppression-control-plane",
            "expression-digest",
            _final_control_plane_expression_digest_10_p2(),
        ),
        (
            "suppression-control-plane",
            "literal",
            _final_security_raw_byte_literal_10_p3(),
        ),
        (
            "vendor-exemption",
            "literal",
            _vendor_exemption_literal_7_p1(),
        ),
        (
            "regex-exclusion",
            "literal",
            _regex_exclusion_literal_8_p1(),
        ),
        (
            "known-gap",
            "literal",
            _known_gap_literal_9_p1(),
        ),
    )


def additional_schema_groups() -> tuple[tuple[str, str, tuple[str, ...]], ...]:
    """Return newly reviewed authority identities grouped by semantics."""
    return _base_review_schema_groups() + _final_review_schema_groups()
