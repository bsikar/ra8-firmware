# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Reviewed non-authority classifications added by the 2026-08 audit."""

from __future__ import annotations


def _identities(value: str) -> tuple[str, ...]:
    """Split one whitespace-separated identity block into exact entries."""
    return tuple(value.split())


def _additional_non_authority_groups_part1() -> tuple[tuple[str, tuple[str, ...]], ...]:
    """Constants proven not to select or exempt checker inputs (part 1 of 4)."""
    return (
        (
            "derived-runtime",
            _identities(
                """
                    checks/check_chapter_map_freshness.py:REPO_ROOT
                    checks/check_hil_privilege_boundary.py:REPO_ROOT
                    checks/check_hil_rig_contract.py:REPO_ROOT
                    checks/hil_convergence_safety_raw_digest_runtime.py:_LABEL_BY_PATH
                    checks/hil_convergence_safety_raw_digest_runtime.py:_MAIN_LABEL
                    checks/hil_convergence_safety_raw_digest_runtime.py:_RAW_FILE_LABEL
                    checks/hil_convergence_safety_policy.py:REPO_ROOT
                    checks/hil_privileged_helper_selftest.py:HELPER
                    checks/hil_privileged_helper_selftest.py:REPO_ROOT
                    checks/markdown_reference_selftest.py:REPO_ROOT
                    checks/markdown_references.py:REPO_ROOT
                    checks/shell_entrypoint_policy.py:PRIVILEGED_PATHS
                    checks/shell_entrypoint_policy.py:SHELL_POLICIES
                    checks/shell_entrypoint_policy.py:SOURCED_ONLY_PATHS
                    """
            ),
        ),
        (
            "selftest-fixture",
            (
                "checks/hil_convergence_safety_raw_digest_fixtures.py:INPUT_BY_PATH",
                "checks/hil_convergence_safety_raw_digest_fixtures.py:PRE_CLOSE_FAILURE",
                "checks/hil_convergence_safety_raw_digest_fixtures.py:ROOT_OPEN_FAILURE",
                "checks/markdown_reference_selftest.py:_SCRIPT_ROOT",
            ),
        ),
    )


def _additional_non_authority_groups_part2() -> tuple[tuple[str, tuple[str, ...]], ...]:
    """Constants proven not to select or exempt checker inputs (part 2 of 4)."""
    return (
        (
            "detector-pattern",
            _identities(
                """
                    checks/check_ansible_collections.py:CALLBACK_RE
                    checks/check_chapter_map_freshness.py:CHAPTER_ROW_RE
                    checks/check_hil_rig_contract.py:EXPANSION
                    checks/check_hil_rig_contract.py:PI_HOST_EXPANSION
                    checks/check_hil_rig_contract.py:SOURCE_RIG
                    checks/check_shell_just_invocations.py:UNPRIVILEGED_RUN_JUST_RE
                    checks/markdown_reference_policy.py:ATX_HEADING_RE
                    checks/markdown_reference_policy.py:BARE_CODE_FILE_RE
                    checks/markdown_reference_policy.py:BARE_FILE_SUFFIX_PATTERN
                    checks/markdown_reference_policy.py:BARE_MARKDOWN_PATTERN
                    checks/markdown_reference_policy.py:EXPLICIT_ANCHOR_RE
                    checks/markdown_reference_policy.py:FENCE_RE
                    checks/markdown_reference_policy.py:HTML_TARGET_RE
                    checks/markdown_reference_policy.py:LINE_CITATION_RE
                    checks/markdown_reference_policy.py:PATH_RE
                    checks/markdown_reference_policy.py:REFERENCE_DEF_RE
                    checks/markdown_reference_policy.py:REFERENCE_USE_RE
                    checks/markdown_reference_policy.py:ROOT_FILE_PATTERN
                    checks/markdown_reference_policy.py:SETEXT_HEADING_RE
                    checks/markdown_reference_policy.py:SHORTCUT_PATH_REFERENCE_RE
                    checks/markdown_reference_policy.py:SOUP_LOCAL_PATH_RE
                    checks/markdown_reference_policy.py:SYMBOL_SUFFIX_RE
                    checks/markdown_references.py:PLACEHOLDER_RE
                    """
            ),
        ),
        ("exit-code", ("checks/check_shebangs.py:EARLY_EXIT_STATUS",)),
    )


def _additional_non_authority_groups_part3() -> tuple[tuple[str, tuple[str, ...]], ...]:
    """Constants proven not to select or exempt checker inputs (part 3 of 4)."""
    return (
        (
            "numeric-format",
            (
                "checks/markdown_reference_policy.py:PARSER_RUNTIME_LIMIT_SECONDS",
                "checks/shell_invocation_policy.py:PRIVILEGED_TARGET_INDEX",
            ),
        ),
        (
            "parser-token",
            _identities(
                """
                    checks/markdown_reference_policy.py:BARE_FILE_SUFFIXES
                    checks/markdown_reference_policy.py:REMOTE_SCHEMES
                    checks/markdown_reference_policy.py:TRAILING_PATH_JUNK
                    checks/shell_invocation_policy.py:DECLARATION_PREFIX
                    checks/shell_invocation_policy.py:JUST_RECIPE
                    checks/shell_invocation_policy.py:MARKDOWN_FENCE
                    checks/shell_invocation_policy.py:NONCOMMAND_SHELL_PREFIX
                    checks/shell_invocation_policy.py:SHELL_CONTROL
                    checks/shell_invocation_policy.py:SHELL_PREFIX
                    checks/shell_invocation_references.py:DOCKER_COMMANDS
                    checks/shell_invocation_references.py:JSON_FENCE_LANGUAGES
                    checks/shell_invocation_references.py:PROCESS_CALLS
                    checks/shell_invocation_references.py:SHELL_FENCE_LANGUAGES
                    checks/shell_invocation_references.py:YAML_COMMAND_KEYS
                    """
            ),
        ),
    )


def _part4_process_patterns() -> tuple[tuple[str, tuple[str, ...]], ...]:
    """Classify image process-policy detector tokens."""
    return (
        (
            "detector-pattern",
            _identities(
                "checks/hil_convergence_safety_image_process_policy.py:"
                "PROCESS_MODULE_SEMANTIC_TOKENS "
                "checks/hil_convergence_safety_image_process_policy.py:"
                "SUPERVISOR_SEMANTIC_PROCESS_TOKENS "
                "checks/hil_convergence_safety_image_process_policy.py:"
                "SUPERVISOR_CASES_SEMANTIC_PROCESS_TOKENS"
                " checks/hil_convergence_safety_image_process_policy.py:"
                "SUPERVISOR_CASES_SCOPED_PROCESS_TOKENS"
                " checks/hil_convergence_safety_image_process_policy.py:"
                "SUPERVISOR_SCOPED_LOADER_TOKENS"
                " checks/hil_convergence_safety_image_process_policy.py:"
                "CROSS_LANGUAGE_SCOPED_TOKENS"
                " checks/hil_convergence_safety_image_process_policy.py:"
                "TRIPWIRE_LABELS"
                " checks/hil_convergence_safety_image_process_policy.py:"
                "TRIPWIRE_PATTERN"
                " checks/hil_convergence_safety_image_harness_policy.py:"
                "_IMAGE_PROCESS_ANALYSIS_MODULE"
                " checks/hil_convergence_safety_image_harness_policy.py:"
                "_IMAGE_PROCESS_ANALYSIS_IMPORT"
                " checks/hil_convergence_safety_image_process_analysis.py:"
                "PROCESS_MODULE_SEMANTIC_TOKENS"
                " checks/hil_convergence_safety_image_process_analysis.py:"
                "SUPERVISOR_SEMANTIC_PROCESS_TOKENS"
                " checks/hil_convergence_safety_image_process_analysis.py:"
                "SUPERVISOR_CASES_SEMANTIC_PROCESS_TOKENS"
                " checks/hil_convergence_safety_image_process_analysis.py:"
                "SUPERVISOR_CASES_SCOPED_PROCESS_TOKENS"
                " checks/hil_convergence_safety_image_process_analysis.py:"
                "SUPERVISOR_SCOPED_LOADER_TOKENS"
                " checks/hil_convergence_safety_image_process_analysis.py:"
                "CROSS_LANGUAGE_SCOPED_TOKENS"
                " checks/hil_convergence_safety_image_process_analysis.py:"
                "TRIPWIRE_LABELS"
                " checks/hil_convergence_safety_image_process_analysis.py:"
                "TRIPWIRE_PATTERN"
                " checks/hil_convergence_safety_process_mutations.py:"
                "_BOUND_EXIT_MUTATION_LABELS"
                " checks/hil_convergence_safety_image_lock_receipts.py:"
                "PROCESS_SOURCE_SPECS"
                " checks/hil_convergence_safety_image_lock_receipts.py:"
                "LEGACY_PROCESS_FINDINGS"
            ),
        ),
    )


def _part4_subreaper_patterns() -> tuple[tuple[str, tuple[str, ...]], ...]:
    """Classify image subreaper detector tokens."""
    return (
        (
            "detector-pattern",
            _identities(
                """
                checks/hil_convergence_safety_image_subreaper_policy.py:SUBREAPER_TOKENS
                checks/hil_convergence_safety_image_subreaper_policy.py:MOVED_PROCESS_TOKENS
                checks/hil_convergence_safety_image_subreaper_policy.py:PROCESS_STATIC_TOKENS
                checks/hil_convergence_safety_image_subreaper_policy.py:PROCESS_LOADER_TOKENS
                checks/hil_convergence_safety_image_subreaper_policy.py:PROCESS_LOADER_ORDER
                checks/hil_convergence_safety_image_subreaper_policy.py:PROCESS_VALIDATION_ORDER
                checks/hil_convergence_safety_image_subreaper_policy.py:PROCESS_LOADER_ORDER_DIAGNOSTIC
                checks/hil_convergence_safety_image_subreaper_policy.py:SUPERVISOR_SUBREAPER_LABELS
                checks/hil_convergence_safety_image_subreaper_policy.py:SUPERVISOR_SUBREAPER_OWNERS
                checks/hil_convergence_safety_image_subreaper_policy.py:SUPERVISOR_SUBREAPER_ORDER
                checks/hil_convergence_safety_image_subreaper_policy.py:PROCESS_SUBREAPER_ORDER
                """
            ),
        ),
    )


def _part4_selftest_fixtures() -> tuple[tuple[str, tuple[str, ...]], ...]:
    """Classify unrelated checker selftest fixtures."""
    return (
        (
            "selftest-fixture",
            _identities(
                """
                    checks/check_bench_lock.py:_FIRE_PRIVILEGED_HELPER
                    checks/check_bench_lock.py:_QUIET_PRIVILEGED_HELPER
                    checks/check_hil_rig_contract.py:FAKE_HARNESS
                    checks/check_hil_rig_contract.py:FAKE_INVALID
                    checks/check_hil_rig_contract.py:FAKE_VALID
                    checks/check_shell_just_invocations.py:FUTURE_SCRIPT_FIXTURE
                    checks/markdown_reference_selftest.py:_FIXTURE_CASES
                    checks/shell_invocation_selftest_cases.py:DUAL
                    checks/shell_invocation_selftest_cases.py:EXTENSIONLESS
                    checks/shell_invocation_selftest_cases.py:INFRA
                    checks/shell_invocation_selftest_cases.py:INSTALL
                    checks/shell_invocation_selftest_cases.py:JUST_CASES
                    checks/shell_invocation_selftest_cases.py:POLICIES
                    checks/shell_invocation_selftest_cases.py:PRIVILEGED_CALLER
                    checks/shell_invocation_selftest_cases.py:PYTHON_CASES
                    checks/shell_invocation_selftest_cases.py:SHELL_CASES
                    checks/shell_invocation_selftest_cases.py:SOURCED
                    checks/shell_invocation_selftest_cases.py:SURFACE_CASES
                    """
            ),
        ),
    )


def _additional_non_authority_groups_part4() -> tuple[tuple[str, tuple[str, ...]], ...]:
    """Combine image policy and fixture non-authorities (part 4 of 6)."""
    return (*_part4_process_patterns(), *_part4_subreaper_patterns(), *_part4_selftest_fixtures())


def _part5_supervisor_numbers() -> tuple[tuple[str, tuple[str, ...]], ...]:
    """Classify supervisor numeric runtime constants."""
    return (
        (
            "numeric-format",
            _identities(
                """
                    ci/devcontainer_image_selftest_supervisor.py:MANAGED_SIGNALS
                    ci/devcontainer_image_selftest_supervisor.py:DEADLINE_SECONDS
                    ci/devcontainer_image_selftest_supervisor.py:POLL_SECONDS
                    ci/devcontainer_image_selftest_supervisor.py:CONTROLLER_ARG_COUNT
                    ci/devcontainer_image_selftest_supervisor.py:SUPERVISOR_ARG_COUNT
                    ci/devcontainer_image_selftest_supervisor.py:HIDDEN_SELFTEST_ARG_COUNT
                    ci/devcontainer_image_selftest_supervisor.py:ROOT_SELFTEST_ARG_COUNT
                    ci/devcontainer_image_selftest_supervisor.py:WATCHDOG_TIMEOUT_SECONDS
                    ci/devcontainer_image_selftest_supervisor.py:SELFTEST_WATCHDOG_TIMEOUT_SECONDS
                    ci/devcontainer_image_selftest_supervisor.py:RECEIPT_MODE
                    ci/devcontainer_image_selftest_supervisor.py:RECEIPT_MAX_BYTES
                    ci/devcontainer_image_selftest_supervisor.py:WRITE_ATTEMPT_MULTIPLIER
                    ci/devcontainer_image_selftest_supervisor.py:STATUS_SIZE
                    ci/devcontainer_image_selftest_supervisor.py:INJECTED_FAILURE_STATUS
                    ci/devcontainer_image_selftest_supervisor.py:PUBLIC_REFUSAL_STATUS
                    ci/devcontainer_image_selftest_supervisor.py:INTEGRITY_REFUSAL_STATUS
                    ci/devcontainer_image_selftest_supervisor.py:HARDLINK_COUNT
                    ci/devcontainer_image_selftest_supervisor.py:STALL_STATUS
                    ci/devcontainer_image_selftest_supervisor.py:MIN_POPULATED_GROUP_MEMBERS
                    ci/devcontainer_image_selftest_supervisor.py:ENTRY_MAX_BYTES
                    ci/devcontainer_image_selftest_supervisor.py:ENTRY_READ_STEPS
                    ci/devcontainer_image_selftest_supervisor.py:ENTRY_MODES
                    ci/devcontainer_image_selftest_supervisor.py:CASES_MODE
                    ci/devcontainer_image_selftest_supervisor.py:CASES_MAX_BYTES
                    ci/devcontainer_image_selftest_supervisor.py:CASES_READ_STEPS
                    ci/devcontainer_image_selftest_supervisor.py:PROCESS_MODE
                    ci/devcontainer_image_selftest_supervisor.py:PROCESS_MAX_BYTES
                    ci/devcontainer_image_selftest_supervisor.py:PROCESS_READ_STEPS
                    ci/devcontainer_image_selftest_supervisor.py:PRIVATE_MODE
                    ci/devcontainer_image_selftest_supervisor.py:SUITE_ROOT_SUFFIX_LENGTH
                    ci/devcontainer_image_selftest_process.py:PROCESS_LOAD_VERSION
                    ci/devcontainer_image_selftest_process.py:PROCESS_GROUP_FIELD
                    ci/devcontainer_image_selftest_process.py:SESSION_FIELD
                    ci/devcontainer_image_selftest_process.py:START_TIME_FIELD
                    ci/devcontainer_image_selftest_process.py:PR_SET_CHILD_SUBREAPER
                    ci/devcontainer_image_selftest_process.py:PR_GET_CHILD_SUBREAPER
                    ci/devcontainer_image_selftest_process.py:CHILD_LIST_MAX_BYTES
                    ci/devcontainer_image_selftest_process.py:ENTRY_EXEC_DESCRIPTOR_MINIMUM
                    """
            ),
        ),
    )


def _part5_supervisor_tokens() -> tuple[tuple[str, tuple[str, ...]], ...]:
    """Classify supervisor parser tokens."""
    return (
        (
            "parser-token",
            (
                "ci/devcontainer_image_selftest_supervisor.py:CASES_ARG",
                "ci/devcontainer_image_selftest_supervisor.py:PROCESS_ARG",
                "ci/devcontainer_image_selftest_supervisor.py:SUITE_ROOT_PREFIX",
            ),
        ),
    )


def _part5_process_runtime() -> tuple[tuple[str, tuple[str, ...]], ...]:
    """Classify process-loader derived runtime values."""
    return (
        (
            "derived-runtime",
            _identities(
                """
                ci/devcontainer_image_selftest_process.py:MAIN_API
                ci/devcontainer_image_selftest_process.py:DEADLINE_SECONDS
                ci/devcontainer_image_selftest_process.py:MANAGED_SIGNALS
                ci/devcontainer_image_selftest_process.py:POLL_SECONDS
                ci/devcontainer_image_selftest_process.py:SUPERVISOR_PROGRAM
                """
            ),
        ),
    )


def _additional_non_authority_groups_part5() -> tuple[tuple[str, tuple[str, ...]], ...]:
    """Combine image-supervisor runtime non-authorities (part 5 of 6)."""
    return (*_part5_supervisor_numbers(), *_part5_supervisor_tokens(), *_part5_process_runtime())


def _additional_non_authority_groups_part6() -> tuple[tuple[str, tuple[str, ...]], ...]:
    """Supervisor-case and invocation fixture constants (part 6 of 8)."""
    return (
        (
            "numeric-format",
            _identities(
                """
                    ci/devcontainer_image_selftest_supervisor_cases.py:HIDDEN_ARG_COUNT
                    ci/devcontainer_image_selftest_supervisor_cases.py:ROOT_ARG_COUNT
                    ci/devcontainer_image_selftest_supervisor_cases.py:CASES_LOAD_VERSION
                    ci/devcontainer_image_selftest_supervisor_cases.py:CLOSED_DESCRIPTOR
                    ci/devcontainer_image_selftest_supervisor_cases.py:PRIVATE_MODE
                    ci/devcontainer_image_selftest_supervisor_cases.py:SUITE_ROOT_SUFFIX_LENGTH
                    ci/devcontainer_image_selftest_supervisor_cases.py:STAT_SELFTEST_ARG_COUNT
                    ci/devcontainer_image_selftest_supervisor_cases.py:TEST_PROCESS_GROUP
                    ci/devcontainer_image_selftest_supervisor_cases.py:USAGE_STATUS
                    ci/devcontainer_image_selftest_supervisor_cases.py:DEADLINE_SECONDS
                    ci/devcontainer_image_selftest_supervisor_cases.py:ENTRY_MAX_BYTES
                    ci/devcontainer_image_selftest_supervisor_cases.py:HARDLINK_COUNT
                    ci/devcontainer_image_selftest_supervisor_cases.py:INTEGRITY_REFUSAL_STATUS
                    ci/devcontainer_image_selftest_supervisor_cases.py:POLL_SECONDS
                    ci/devcontainer_image_selftest_supervisor_cases.py:PUBLIC_REFUSAL_STATUS
                    ci/devcontainer_image_selftest_supervisor_cases.py:RECEIPT_MODE
                    ci/devcontainer_image_selftest_supervisor_cases.py:RECEIPT_MAX_BYTES
                    ci/devcontainer_image_selftest_supervisor_cases.py:SELFTEST_WATCHDOG_TIMEOUT_SECONDS
                    ci/devcontainer_image_selftest_supervisor_cases.py:STALL_STATUS
                    """
            ),
        ),
        (
            "derived-runtime",
            (
                "ci/devcontainer_image_selftest_supervisor_cases.py:MAIN_API",
                "ci/devcontainer_image_selftest_supervisor_cases.py:SUPERVISOR_PROGRAM",
            ),
        ),
        (
            "selftest-fixture",
            _identities(
                """
                    checks/shell_invocation_selftest_cases.py:RELEASE_LOADER_REL
                    checks/shell_invocation_selftest_cases.py:RELEASE_LOADER_CALL
                    checks/shell_invocation_selftest_cases.py:RELEASE_FIXTURE_REL
                    checks/shell_invocation_selftest_cases.py:RELEASE_FIXTURE_CALL
                    checks/shell_invocation_selftest_cases.py:PORTABLE_CALLER
                    checks/shell_invocation_selftest_cases.py:SOURCED_CALLER
                    checks/shell_invocation_selftest_cases.py:UNBOUND_SOURCED_CALLER
                    checks/shell_invocation_selftest_cases.py:NONEXECUTABLE_CALLER
                    checks/shell_invocation_selftest_cases.py:SH_CALLER
                    checks/shell_invocation_selftest_cases.py:RELEASE_LOADER_CASES
                    """
            ),
        ),
    )


def _additional_non_authority_groups_part7() -> tuple[tuple[str, tuple[str, ...]], ...]:
    """Runtime detector constants outside checker authority (part 7 of 8)."""
    return (
        (
            "selftest-fixture",
            ("checks/hil_convergence_safety_runtime_loader_harness.py:_HARNESS",),
        ),
        (
            "numeric-format",
            _identities(
                """
                    checks/hil_convergence_safety_runtime_mutations.py:RUNTIME_TIMEOUT_SECONDS
                    checks/hil_convergence_safety_runtime_mutations.py:RESIDUE_TIMEOUT_SECONDS
                    checks/hil_convergence_safety_runtime_mutations.py:POLL_SECONDS
                    checks/hil_convergence_safety_runtime_mutations.py:PRIVATE_MODE
                    checks/hil_convergence_safety_runtime_cleanup.py:SOURCE_DESCRIPTOR_COUNT
                    checks/hil_convergence_safety_runtime_escape.py:IDENTITY_FIELD_COUNT
                    checks/hil_convergence_safety_runtime_escape.py:PROCESS_GROUP_FIELD
                    checks/hil_convergence_safety_runtime_escape.py:SESSION_FIELD
                    checks/hil_convergence_safety_runtime_escape.py:START_TIME_FIELD
                    checks/hil_convergence_safety_runtime_mutations.py:ROOT_SUFFIX_LENGTH
                    checks/hil_convergence_safety_runtime_mutations.py:PROCESS_GROUP_FIELD
                    checks/hil_convergence_safety_runtime_mutations.py:PROCESS_UID_FIELD_COUNT
                    checks/hil_convergence_safety_runtime_cleanup.py:GATE_DESCRIPTOR_COUNT
                    """
            ),
        ),
        (
            "parser-token",
            ("checks/hil_convergence_safety_runtime_mutations.py:ROOT_PREFIX",),
        ),
        (
            "exit-code",
            _identities(
                """
                    checks/hil_convergence_safety_runtime_mutations.py:INTEGRITY_REFUSAL_STATUS
                    checks/hil_convergence_safety_runtime_mutations.py:PUBLIC_REFUSAL_STATUS
                    checks/hil_convergence_safety_runtime_mutations.py:USAGE_STATUS
                    checks/hil_convergence_safety_runtime_mutations.py:DESCENDANT_STATUS
                    checks/hil_convergence_safety_runtime_mutations.py:ONE_SHOT_MUTATION_STATUS
                    """
            ),
        ),
        (
            "detector-pattern",
            (
                "checks/hil_convergence_safety_runtime_sources.py:CASES_PIN_PATTERN",
                "checks/hil_convergence_safety_runtime_sources.py:PROCESS_PIN_PATTERN",
            ),
        ),
        (
            "derived-runtime",
            ("checks/hil_convergence_safety_runtime_mutations.py:CANONICAL_TMP",),
        ),
    )


def _additional_non_authority_groups_part8() -> tuple[tuple[str, tuple[str, ...]], ...]:
    """Semantic mutation fixtures and their expected findings (part 8 of 8)."""
    return (
        (
            "detector-pattern",
            _identities(
                """
                    checks/hil_convergence_safety_runtime_fixtures.py:SUPERVISOR_BOUND_FINDING
                    checks/hil_convergence_safety_runtime_fixtures.py:SUPERVISOR_ORDER_FINDING
                    checks/hil_convergence_safety_runtime_fixtures.py:SUPERVISOR_ORDER_ONLY_LABELS
                    checks/hil_convergence_safety_runtime_fixtures.py:SUPERVISOR_DOUBLE_FINDING_LABELS
                    checks/hil_convergence_safety_runtime_fixtures.py:SUPERVISOR_TOKEN_FINDINGS
                    checks/hil_convergence_safety_runtime_fixtures.py:LIFECYCLE_SEMANTIC_FINDINGS
                    checks/hil_convergence_safety_runtime_fixtures.py:SIGNAL_SEMANTIC_FINDINGS
                    checks/hil_convergence_safety_runtime_fixtures.py:CASES_SEMANTIC_FINDINGS
                    checks/hil_convergence_safety_runtime_fixtures.py:SUPERVISOR_CASES_SEMANTIC_FINDINGS
                    """
            ),
        ),
        (
            "selftest-fixture",
            _identities(
                """
                    checks/hil_convergence_safety_runtime_sources.py:SOURCE_MODE
                    checks/hil_convergence_safety_process_source_fixtures.py:PROCESS
                    checks/hil_convergence_safety_process_source_fixtures.py:SUPERVISOR
                    checks/hil_convergence_safety_process_source_fixtures.py:SUPERVISOR_CASES
                    checks/hil_convergence_safety_source_fixtures.py:CLOSED_COMMAND_BLOCK
                    checks/hil_convergence_safety_source_fixtures.py:CLOSED_POPEN_BLOCK
                    checks/hil_convergence_safety_source_fixtures.py:REFUSED_POPEN_BLOCK
                    checks/hil_convergence_safety_source_fixtures.py:MAIN_POPEN_BLOCK
                    checks/hil_convergence_safety_source_fixtures.py:SUPERVISOR_LAUNCH_BLOCK
                    checks/hil_convergence_safety_source_fixtures.py:PAYLOAD_ENTRY_BLOCK
                    checks/hil_convergence_safety_source_fixtures.py:ENTRY_INTEGRITY_BLOCK
                    checks/hil_convergence_safety_source_fixtures.py:MAIN_ROOT_PATH_BLOCK
                    checks/hil_convergence_safety_source_fixtures.py:MAIN_ROOT_METADATA_BLOCK
                    checks/hil_convergence_safety_source_fixtures.py:CASES_ROOT_BLOCK
                    checks/hil_convergence_safety_source_fixtures.py:ROOT_CANONICAL_VALUE
                    checks/hil_convergence_safety_source_fixtures.py:MAIN_ROOT_IDENTITY_BLOCK
                    checks/hil_convergence_safety_source_fixtures.py:HANDLER_VALIDATION_BLOCK
                    checks/hil_convergence_safety_source_fixtures.py:CASES_EMBEDDED_EXEC_BLOCK
                    checks/hil_convergence_safety_source_fixtures.py:CASES_PYTHON_LAUNCH_BLOCK
                    checks/hil_convergence_safety_source_fixtures.py:SIGNAL_EMBEDDED_EXEC_BLOCK
                    checks/hil_convergence_safety_source_fixtures.py:SIGNAL_PYTHON_LAUNCH_BLOCK
                    checks/hil_convergence_safety_source_fixtures.py:REPLACED_LOCK_TRIPWIRE_BLOCK
                    checks/hil_convergence_safety_source_fixtures.py:MISSING_LOCK_TRIPWIRE_BLOCK
                    checks/hil_convergence_safety_source_fixtures.py:CASES_SOURCE_READ_BLOCK
                    checks/hil_convergence_safety_source_fixtures.py:CASES_LOADER_BLOCK
                    """
            ),
        ),
    )


def additional_non_authority_groups() -> tuple[tuple[str, tuple[str, ...]], ...]:
    """Return constants proven not to select or exempt checker inputs."""
    return (
        *_additional_non_authority_groups_part1(),
        *_additional_non_authority_groups_part2(),
        *_additional_non_authority_groups_part3(),
        *_additional_non_authority_groups_part4(),
        *_additional_non_authority_groups_part5(),
        *_additional_non_authority_groups_part6(),
        *_additional_non_authority_groups_part7(),
        *_additional_non_authority_groups_part8(),
    )
