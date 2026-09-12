# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Render a review-only GitHub script with complete preflight and recovery."""

from __future__ import annotations

import hashlib
import shlex
from dataclasses import dataclass

from work_plan import Node, Plan, _require_safe_plan, plan_to_json


def _record_call(*cells: str) -> str:
    """Return one argv-preserving recovery-ledger function call."""
    return "record_recovery " + " ".join(shlex.quote(cell) for cell in cells)


@dataclass(frozen=True)
class EmittedCommand:
    """The data for one issue creation and its project fields."""

    key: str
    argv: tuple[str, ...]
    fields: tuple[tuple[str, str], ...]
    epic: str | None
    depends_on: tuple[str, ...]


def _issue_body(node: Node) -> str:
    """Build one issue body, including stable planning metadata."""
    parts = [node.body] if node.body else []
    trailer = [f"Plan-key: {node.key}"]
    if node.priority:
        trailer.append(f"Priority: {node.priority}")
    if node.estimate:
        trailer.append(f"Estimate: {node.estimate}")
    parts.append("\n".join(trailer))
    return "\n\n".join(parts)


def issue_commands(plan: Plan) -> list[EmittedCommand]:
    """Build every issue command in deterministic dependency order."""
    _require_safe_plan(plan)
    index = plan.by_key()
    built: list[EmittedCommand] = []
    for key in plan.order:
        node = index[key]
        argv: list[str] = ["--title", node.title, "--body", _issue_body(node)]
        for label in node.labels:
            argv.extend(["--label", label])
        fields = tuple(
            (name, value)
            for name, value in (
                ("Status", node.status),
                ("Track", node.track),
                ("Priority", node.priority),
            )
            if value is not None
        )
        built.append(
            EmittedCommand(
                key=node.key,
                argv=tuple(argv),
                fields=fields,
                epic=node.epic,
                depends_on=node.depends_on,
            )
        )
    return built


def _script_targeting(plan: Plan, plan_id: str) -> str:
    """Return the fixed GitHub targeting preamble for the emitted script."""
    return f"""#!/bin/sh
# shellcheck disable=SC2016  # GraphQL and hostile notes data must remain literal argv.
# REVIEW BEFORE RUNNING. This script was emitted, never executed by work.
set -eu
umask 077

GH_HOST={shlex.quote(plan.github_host)}
TARGET_REPO={shlex.quote(plan.repository)}
PROJECT_OWNER={shlex.quote(plan.project_owner)}
PROJECT_NUMBER={plan.project_number}
PLAN_ID={plan_id}
export GH_HOST

MUTATION_STARTED=0
"""


def _script_ledger_anchor() -> str:
    """Return the fail-closed HOME anchor for the mutation recovery ledger.

    The rerun guard is only as strong as this anchor, so every untrustworthy
    HOME shape refuses here rather than being resolved into a fresh ledger.
    """
    return """# The rerun guard below is only as strong as this anchor. A ledger placed
# relative to the working directory refused a second run from the SAME
# directory and silently created a duplicate set of issues from any other
# one, which is exactly what the guard claims to prevent. Anchor it to the
# invoking operator, so one plan has one ledger wherever this is run from.
# A relative HOME would put the anchor back under the working directory, so
# it is rejected rather than resolved.
ledger_refusal() {
  printf '%s\\n' "$@" >&2
  exit 2
}

[ -n "${HOME-}" ] ||
  ledger_refusal "HOME must be set; it anchors the GitHub mutation recovery ledger"
case $HOME in
  /*) ;;
  *)
    ledger_refusal \\
      "HOME must be an absolute path; a relative HOME re-anchors the" \\
      "recovery ledger to the working directory"
    ;;
esac
[ -d "$HOME" ] ||
  ledger_refusal "HOME does not name an existing directory;" \\
    "refusing to fabricate a recovery ledger root"
RECOVERY_ROOT="$HOME/.ra8-work-recovery"
RECOVERY_DIR="$RECOVERY_ROOT/$PLAN_ID"
RECOVERY_LEDGER="$RECOVERY_DIR/ledger.tsv"
"""


def _script_recovery_harness() -> str:
    """Return the recovery-ledger writer, reporter, and signal traps."""
    return """
record_recovery() {
  first=$1
  shift
  printf '%s' "$first" >&3
  for field do
    printf '\\t%s' "$field" >&3
  done
  printf '\\n' >&3
}

report_recovery() {
  rc=$?
  trap - EXIT HUP INT TERM
  if [ "$MUTATION_STARTED" -eq 1 ]; then
    printf '%s\\n' "GitHub mutation recovery ledger: $RECOVERY_LEDGER" >&2
    cat "$RECOVERY_LEDGER" >&2
    if [ "$rc" -ne 0 ]; then
      printf '%s\\n' \\
        "A mutation failed. Do not rerun blindly and do not auto-delete." \\
        "Review BEGIN rows without matching result rows, inspect the exact" \\
        "repository/project above, then resume or clean up each URL/item by hand." >&2
    fi
  fi
  exit "$rc"
}

trap report_recovery EXIT
trap 'exit 130' HUP INT TERM
"""


def _script_header(plan: Plan) -> str:
    """Return fixed GitHub targeting and the fail-closed recovery harness."""
    plan_id = hashlib.sha256(plan_to_json(plan).encode("utf-8")).hexdigest()[:20]
    return (
        _script_targeting(plan, plan_id)
        + _script_ledger_anchor()
        + _script_recovery_harness()
        + "\n"
    )


def _board_helpers() -> str:
    """Return exact project discovery and field helper functions."""
    return """load_project() {
  gh api --hostname "$GH_HOST" graphql \
    -f login="$PROJECT_OWNER" -F number="$PROJECT_NUMBER" -f query='
    query($login: String!, $number: Int!) {
      user(login: $login) {
        projectV2(number: $number) { id number title viewerCanUpdate }
      }
    }' --jq '.data.user.projectV2'
}

load_project_fields() {
  gh api --hostname "$GH_HOST" graphql --paginate -f project="$PROJECT_ID" -f query='
    query($project: ID!, $endCursor: String) {
      node(id: $project) {
        ... on ProjectV2 {
          fields(first: 100, after: $endCursor) {
            nodes {
              ... on ProjectV2SingleSelectField { id name options { id name } }
            }
            pageInfo { hasNextPage endCursor }
          }
        }
      }
    }' --jq '.data.node.fields.nodes[]'
}

require_field() {
  count="$(printf %s "$PROJECT_FIELDS" | jq -s --arg n "$1" \
    '[.[] | select(.name==$n)] | length')"
  [ "$count" -eq 1 ] || { echo "field must resolve exactly once: $1" >&2; exit 1; }
}

field_id() {
  printf %s "$PROJECT_FIELDS" | jq -r -s --arg n "$1" \
    '[.[] | select(.name==$n)][0].id'
}

require_option() {
  count="$(printf %s "$PROJECT_FIELDS" | jq -s --arg n "$1" --arg o "$2" \
    '[.[] | select(.name==$n) | .options[] | select(.name==$o)] | length')"
  [ "$count" -eq 1 ] || {
    echo "option must resolve exactly once: $1 / $2" >&2
    exit 1
  }
}

option_id() {
  printf %s "$PROJECT_FIELDS" | jq -r -s --arg n "$1" --arg o "$2" \
    '[.[] | select(.name==$n) | .options[] | select(.name==$o)][0].id'
}

"""


def _preflight_authorization(plan: Plan) -> list[str]:
    """Return the identity and permission proofs that precede any mutation."""
    return [
        'command -v gh >/dev/null 2>&1 || { echo "gh is required" >&2; exit 1; }',
        'command -v jq >/dev/null 2>&1 || { echo "jq is required" >&2; exit 1; }',
        'gh auth status --hostname "$GH_HOST" >/dev/null',
        'REPO_META="$(gh api --hostname "$GH_HOST" "repos/$TARGET_REPO")"',
        f'[ "$(printf %s "$REPO_META" | jq -r .full_name)" = {shlex.quote(plan.repository)} ] || '
        '{ echo "repository identity mismatch" >&2; exit 1; }',
        '[ "$(printf %s "$REPO_META" | jq -r .has_issues)" = true ] || '
        '{ echo "issues are disabled" >&2; exit 1; }',
        '[ "$(printf %s "$REPO_META" | jq -r .permissions.push)" = true ] || '
        '{ echo "token cannot create issues in the exact repository" >&2; exit 1; }',
        'PROJECT_META="$(load_project)"',
        '[ "$(printf %s "$PROJECT_META" | jq -r .number)" -eq "$PROJECT_NUMBER" ] || '
        '{ echo "exact project did not resolve" >&2; exit 1; }',
        '[ "$(printf %s "$PROJECT_META" | jq -r .viewerCanUpdate)" = true ] || '
        '{ echo "token cannot update the exact project" >&2; exit 1; }',
        'PROJECT_ID="$(printf %s "$PROJECT_META" | jq -r .id)"',
        '[ -n "$PROJECT_ID" ] && [ "$PROJECT_ID" != null ] || '
        '{ echo "project id is absent" >&2; exit 1; }',
        'PROJECT_FIELDS="$(load_project_fields)"',
    ]


def _preflight_schema(commands: list[EmittedCommand]) -> list[str]:
    """Return the field, option, and label existence proofs for this plan."""
    fields = sorted({field for command in commands for field, _value in command.fields})
    options = sorted({item for command in commands for item in command.fields})
    labels = sorted(
        {
            value
            for command in commands
            for flag, value in zip(command.argv[0::2], command.argv[1::2], strict=True)
            if flag == "--label"
        }
    )
    lines: list[str] = []
    lines.extend(f"require_field {shlex.quote(name)}" for name in fields)
    lines.extend(
        f"require_option {shlex.quote(name)} {shlex.quote(value)}" for name, value in options
    )
    lines.append(
        'REPO_LABELS="$(gh api --hostname "$GH_HOST" --paginate '
        '"repos/$TARGET_REPO/labels?per_page=100" --jq \'.[] | {name: .name}\')"'
    )
    for label in labels:
        quoted = shlex.quote(label)
        lines.append(
            f'[ "$(printf %s "$REPO_LABELS" | jq -s --arg n {quoted} '
            "'[.[] | select(.name==$n)] | length')\" -eq 1 ] || "
            '{ echo "a required label did not resolve exactly once" >&2; exit 1; }'
        )
    lines.extend(
        [
            '[ ! -L "$RECOVERY_ROOT" ] || ledger_refusal '
            '"the recovery ledger root is a symlink; refusing to follow it"',
            '[ ! -e "$RECOVERY_ROOT" ] || [ -d "$RECOVERY_ROOT" ] || ledger_refusal '
            '"the recovery ledger root exists and is not a directory"',
            '[ -d "$RECOVERY_ROOT" ] || mkdir -m 700 "$RECOVERY_ROOT"',
            # Re-check after the create: this is the only thing standing between
            # a symlink swapped in during the window above and a ledger written
            # through it. Deleting it leaves every other check passing.
            '[ ! -L "$RECOVERY_ROOT" ] && [ -d "$RECOVERY_ROOT" ] || ledger_refusal '
            '"the recovery ledger root is not a plain directory; refusing to use it"',
            '[ ! -e "$RECOVERY_DIR" ] && [ ! -L "$RECOVERY_DIR" ] || '
            "{ printf '%s\\n' \"this plan already has a recovery ledger at "
            '$RECOVERY_DIR; review it instead of duplicating issues" >&2; exit 1; }',
            'mkdir -m 700 "$RECOVERY_DIR"',
            'exec 3>"$RECOVERY_LEDGER"',
            'record_recovery plan "$PLAN_ID" "$GH_HOST" "$TARGET_REPO" '
            '"$PROJECT_OWNER/$PROJECT_NUMBER"',
            'echo "preflight complete; all following commands mutate GitHub" >&2',
        ]
    )
    return lines


def _render_preflight(plan: Plan, commands: list[EmittedCommand]) -> str:
    """Render every authorization and schema proof before mutation."""
    lines = _preflight_authorization(plan) + _preflight_schema(commands)
    return "\n".join(lines) + "\n\n"


def _render_create(command: EmittedCommand, number_vars: dict[str, str]) -> str:
    """Render issue creation plus immediate recovery evidence."""
    body = command.argv[3]
    lines = [f"ISSUE_BODY={shlex.quote(body)}"]
    if command.epic is not None:
        lines.extend(['ISSUE_BODY="${ISSUE_BODY}', f'Epic: #${{{number_vars[command.epic]}}}"'])
    for dependency in command.depends_on:
        lines.extend(['ISSUE_BODY="${ISSUE_BODY}', f'Depends-on: #${{{number_vars[dependency]}}}"'])
    lines.extend(
        [
            "MUTATION_STARTED=1",
            _record_call("BEGIN", "issue", command.key),
            'ISSUE_URL="$(gh issue create --repo "$TARGET_REPO" \\',
        ]
    )
    pairs = list(zip(command.argv[0::2], command.argv[1::2], strict=True))
    for position, (flag, value) in enumerate(pairs):
        value_text = '"$ISSUE_BODY"' if flag == "--body" else shlex.quote(value)
        tail = "\\" if position + 1 < len(pairs) else ')"'
        lines.append(f"  {flag} {value_text} {tail}")
    lines.extend(
        [
            'case "$ISSUE_URL" in',
            '  "https://github.com/$TARGET_REPO/issues/"*) ;;',
            '  *) echo "created issue URL did not match exact host/repository" >&2; exit 1 ;;',
            "esac",
            f'{number_vars[command.key]}="${{ISSUE_URL##*/}}"',
            f'case "${{{number_vars[command.key]}}}" in ""|*[!0-9]*) exit 1 ;; esac',
            f'record_recovery issue {shlex.quote(command.key)} "$ISSUE_URL"',
        ]
    )
    return "\n".join(lines) + "\n"


def _render_board(command: EmittedCommand, number_var: str) -> str:
    """Render board placement and field updates with per-step evidence."""
    key = command.key
    lines = [
        _record_call("BEGIN", "item", key),
        f'CONTENT_ID="$(gh api --hostname "$GH_HOST" '
        f'"repos/$TARGET_REPO/issues/${{{number_var}}}" --jq .node_id)"',
        'ITEM_ID="$(gh api --hostname "$GH_HOST" graphql -f project="$PROJECT_ID" '
        '-f content="$CONTENT_ID" -f query=\'mutation($project: ID!, $content: ID!) {'
        " addProjectV2ItemById(input: {projectId: $project, contentId: $content}) {"
        " item { id } } }' --jq .data.addProjectV2ItemById.item.id)\"",
        'case "$ITEM_ID" in ""|*[!A-Za-z0-9_-]*) '
        'echo "invalid project item id" >&2; exit 1 ;; esac',
        f'record_recovery item {shlex.quote(key)} "$ITEM_ID"',
    ]
    for field, value in command.fields:
        field_q = shlex.quote(field)
        value_q = shlex.quote(value)
        lines.extend(
            [
                _record_call("BEGIN", "field", key, field, value),
                'gh api --hostname "$GH_HOST" graphql -f project="$PROJECT_ID" '
                f'-f item="$ITEM_ID" -f field="$(field_id {field_q})" '
                f'-f option="$(option_id {field_q} {value_q})" -f query=\''
                "mutation($project: ID!, $item: ID!, $field: ID!, $option: String!) {"
                " updateProjectV2ItemFieldValue(input: {projectId: $project, itemId: $item,"
                " fieldId: $field, value: {singleSelectOptionId: $option}}) {"
                " projectV2Item { id } } }' >/dev/null",
                _record_call("field", key, field, value),
            ]
        )
    return "\n".join(lines) + "\n"


def render_commands(plan: Plan) -> str:
    """Render the complete human-reviewed GitHub mutation script."""
    commands = issue_commands(plan)
    number_vars = {
        command.key: f"ISSUE_NUMBER_{position}" for position, command in enumerate(commands, 1)
    }
    chunks = [_script_header(plan), _board_helpers(), _render_preflight(plan, commands)]
    for command in commands:
        chunks.append(f"# --- {command.key} " + "-" * max(1, 60 - len(command.key)) + "\n")
        chunks.append(_render_create(command, number_vars))
        chunks.append(_render_board(command, number_vars[command.key]))
        chunks.append("\n")
    chunks.append('record_recovery complete "$PLAN_ID"\n')
    chunks.append(
        'echo "all mutations completed; retain $RECOVERY_LEDGER as the operator record" >&2\n'
    )
    return "".join(chunks)
