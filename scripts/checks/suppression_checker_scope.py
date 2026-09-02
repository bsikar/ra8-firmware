# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""AST inventory of checker scope authorities and their concrete values."""

from __future__ import annotations

import ast
import hashlib
import json
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

from suppression_catalog import ownership
from suppression_checker_census import (
    census_bindings,
    classification_digest,
    mutation_findings,
    repository_mutation_findings,
    shape_problem,
    sink_findings,
)
from suppression_model import Finding, Suppression
from suppression_nonauth_registry import NON_AUTHORITIES
from suppression_scope_registry import (
    AUTHORITY_SCHEMAS,
    NON_AUTHORITY_CATEGORIES,
    AuthoritySchema,
)

CANDIDATE_PREFIXES = ("scripts/checks/", "scripts/ci/")
EXPECTED_AUTHORITIES = 1006
EXPECTED_VALUES = 3779
MIN_CENSUS_CONSTANTS = 1700
PAIR_SIZE = 2
GAP_MIN_ARGS = 4
EXPECTED_AUTHORITY_VALUE_SHA256 = "6971b6e7aac6006052ca8e14785ac095261a0c3422616cc585a24d402d505bdc"
EXPECTED_AUTHORITY_REASON_SHA256 = (
    "a80044f930143e9b426600aeaf0420e77f928592c32e572c666fb7e5c95ad7ca"
)
EXPECTED_CLASSIFICATION_SHA256 = "f7f8294b528fa36894ca76d55dc984f48382a5761f75a85614e3ab6f1358503d"
_EMPTY_AUTHORITIES = frozenset(
    {
        "scripts/checks/stack_usage_check.py:FIRST_PARTY_EXEMPTIONS",
        "scripts/checks/suppression_governance.py:GLOBAL_EXCLUSION_AUTHORITIES",
    }
)
_SUBTYPE_REASONS = {
    "positive-scope": "Explicit positive boundary determines which repository inputs are checked.",
    "path-exclusion": (
        "Explicit path exclusion removes reviewed non-first-party or out-of-domain input."
    ),
    "self-exemption": (
        "Self-reference exemption prevents the policy checker from flagging its own grammar."
    ),
    "allowed-token": (
        "Named token is an explicitly reviewed semantic exception to the checker rule."
    ),
    "vendor-exemption": "Vendored or SOUP input is governed by its upstream validation boundary.",
    "generated-classification": (
        "Generated input is governed by a distinct reproducible generator boundary."
    ),
    "host-exemption": "Hosted-only input is outside the embedded target semantic boundary.",
    "ignored-literal": "Named semantic literal is an explicit checker exception.",
    "regex-exclusion": "Compiled expression defines an exact reviewed exclusion boundary.",
    "known-gap": "Named gap remains explicit and measurable until its checker support lands.",
    "stack-exemption": "Named frame exception binds an exact function, ceiling, and rationale.",
    "suppression-control-plane": (
        "Scanner control-plane value defines ownership, syntax, or census scope."
    ),
    "anti-vacuity-floor": (
        "Numeric floor or audited count keeps a collapsed scan from reporting clean."
    ),
    "waiver-recognizer": (
        "Pattern recognizing an explicit in-source waiver marker; it defines how policy is waived."
    ),
}


@dataclass(frozen=True)
class PolicyValue:
    """One safely resolved concrete value and an optional source rationale."""

    value: str
    reason: str = ""


@dataclass(frozen=True)
class ResolvedAuthority:
    """One final module assignment bound to its explicit registry schema."""

    path: str
    line: int
    name: str
    schema: AuthoritySchema
    values: tuple[PolicyValue, ...]


def _call_name(node: ast.Call) -> str:
    """Return a simple/attribute call name without executing it."""
    func = node.func
    if isinstance(func, ast.Name):
        return func.id
    if isinstance(func, ast.Attribute):
        return func.attr
    return ""


def _dedupe(values: list[PolicyValue]) -> list[PolicyValue]:
    """Preserve declaration order while rejecting duplicate concrete leaves."""
    result: list[PolicyValue] = []
    seen: set[str] = set()
    for item in values:
        if item.value in seen:
            continue
        seen.add(item.value)
        result.append(item)
    return result


def _pair_value(node: ast.AST, env: dict[str, list[PolicyValue]], path: str) -> PolicyValue | None:
    """Resolve one explicitly schema-bound ``(value, reason)`` policy tuple."""
    if not isinstance(node, (ast.Tuple, ast.List)) or len(node.elts) != PAIR_SIZE:
        return None
    second = node.elts[1]
    if not isinstance(second, ast.Constant) or not isinstance(second.value, str):
        return None
    reason = second.value.strip()
    if not any(character.isalnum() for character in reason):
        return None
    first = _resolve(node.elts[0], env, path, reasoned_pairs=False)
    if len(first) != 1:
        return None
    return PolicyValue(first[0].value, reason)


def _path_join(left: str, right: str) -> str:
    """Join repo-relative path AST operands without host-path semantics."""
    if not left:
        return right
    return str(PurePosixPath(left) / right)


def _is_own_file(node: ast.AST) -> bool:
    """Recognize a ``Path(__file__)`` expression, optionally ``.resolve()``d."""
    if isinstance(node, ast.Call) and _call_name(node) == "resolve":
        return isinstance(node.func, ast.Attribute) and _is_own_file(node.func.value)
    if isinstance(node, ast.Call) and _call_name(node) == "Path" and node.args:
        first = node.args[0]
        return isinstance(first, ast.Name) and first.id == "__file__"
    return False


def _is_repo_root_chain(node: ast.AST) -> bool:
    """Recognize ``repo_root()`` calls and parent-chains over ``__file__``."""
    if isinstance(node, ast.Call) and _call_name(node) == "repo_root":
        return True
    if isinstance(node, ast.Attribute) and node.attr in {"parent", "parents"}:
        return _is_own_file(node.value) or _is_repo_root_chain(node.value)
    if isinstance(node, ast.Subscript):
        return _is_repo_root_chain(node.value)
    return False


def _resolve_comprehension(
    node: ast.GeneratorExp | ast.ListComp | ast.SetComp,
    env: dict[str, list[PolicyValue]],
    path: str,
) -> list[PolicyValue]:
    """Statically evaluate a single-variable, condition-free comprehension."""
    if len(node.generators) != 1:
        return []
    generator = node.generators[0]
    if generator.ifs or generator.is_async or not isinstance(generator.target, ast.Name):
        return []
    items = _resolve(generator.iter, env, path)
    if not items:
        return []
    loop_name = generator.target.id
    result: list[PolicyValue] = []
    for item in items:
        scoped = dict(env)
        scoped[loop_name] = [item]
        element = _resolve(node.elt, scoped, path)
        if len(element) != 1:
            return []
        result.append(element[0])
    return _dedupe(result)


def _resolve_joined(
    node: ast.JoinedStr, env: dict[str, list[PolicyValue]], path: str
) -> list[PolicyValue]:
    """Concatenate an f-string whose every part resolves to one value."""
    parts: list[str] = []
    for value in node.values:
        if isinstance(value, ast.Constant) and isinstance(value.value, str):
            parts.append(value.value)
        elif isinstance(value, ast.FormattedValue):
            inner = _resolve(value.value, env, path)
            if len(inner) != 1:
                return []
            parts.append(inner[0].value)
        else:
            return []
    return [PolicyValue("".join(parts))]


def _resolve_call(
    node: ast.Call,
    env: dict[str, list[PolicyValue]],
    path: str,
    *,
    reasoned_pairs: bool,
) -> list[PolicyValue]:
    """Resolve the closed call vocabulary used by scope authorities."""
    name = _call_name(node)
    result: list[PolicyValue] = []
    if _is_repo_root_chain(node):
        result = [PolicyValue("")]
    elif _is_own_file(node):
        result = [PolicyValue(path)]
    elif name in {"frozenset", "set", "tuple", "list"}:
        result = (
            _resolve(node.args[0], env, path, reasoned_pairs=reasoned_pairs) if node.args else []
        )
    elif name in {"compile", "Path", "PurePosixPath", "escape"} and node.args:
        first = node.args[0]
        if (
            isinstance(first, ast.Call)
            and _call_name(first) == "get"
            and len(first.args) == PAIR_SIZE
        ):
            result = _resolve(first.args[1], env, path)
        else:
            result = _resolve(first, env, path)
    else:
        result = _resolve_call_tail(name, node, env, path)
    return result


def _resolve_call_tail(
    name: str,
    node: ast.Call,
    env: dict[str, list[PolicyValue]],
    path: str,
) -> list[PolicyValue]:
    """Resolve the join, fromkeys, own-path, and Gap call forms."""
    result: list[PolicyValue] = []
    if name == "resolve" and isinstance(node.func, ast.Attribute):
        result = _resolve(node.func.value, env, path)
    elif name == "fromkeys" and len(node.args) == PAIR_SIZE:
        keys = _resolve(node.args[0], env, path)
        label = _resolve(node.args[1], env, path)
        if keys and len(label) == 1:
            result = [PolicyValue(f"{key.value}:{label[0].value}") for key in keys]
    elif name == "join" and isinstance(node.func, ast.Attribute) and node.args:
        separator = _resolve(node.func.value, env, path)
        joined = _resolve(node.args[0], env, path)
        if len(separator) == 1 and joined:
            result = [PolicyValue(separator[0].value.join(item.value for item in joined))]
    elif name == "Gap" and len(node.args) >= GAP_MIN_ARGS:
        resolved = _resolve(node.args[0], env, path)
        if len(resolved) == 1:
            reason_bits = [
                item.value
                for argument in node.args[1:GAP_MIN_ARGS]
                for item in _resolve(argument, env, path)
            ]
            result = [PolicyValue(resolved[0].value, " ".join(reason_bits))]
    return result


def _resolve_collection(
    node: ast.Tuple | ast.List | ast.Set,
    env: dict[str, list[PolicyValue]],
    path: str,
    *,
    reasoned_pairs: bool,
) -> list[PolicyValue]:
    """Resolve a literal container under one explicit authority schema."""
    if reasoned_pairs:
        pairs = [_pair_value(child, env, path) for child in node.elts]
        return _dedupe([item for item in pairs if item is not None]) if all(pairs) else []
    return _dedupe(
        [item for child in node.elts for item in _resolve(child, env, path, reasoned_pairs=False)]
    )


def _resolve_dict(
    node: ast.Dict, env: dict[str, list[PolicyValue]], path: str
) -> list[PolicyValue]:
    """Flatten a mapping authority as key:value concrete leaves."""
    values: list[PolicyValue] = []
    for key_node, value_node in zip(node.keys, node.values, strict=True):
        if key_node is None:
            values.extend(_resolve(value_node, env, path))
            continue
        keys = _resolve(key_node, env, path)
        children = _resolve(value_node, env, path)
        for key in keys:
            values.extend(
                PolicyValue(f"{key.value}:{child.value}", child.reason) for child in children
            )
    return _dedupe(values)


def _resolve_binary(
    node: ast.BinOp, env: dict[str, list[PolicyValue]], path: str
) -> list[PolicyValue]:
    """Resolve union, string concatenation, and repo-path joins."""
    left = _resolve(node.left, env, path)
    right = _resolve(node.right, env, path)
    if isinstance(node.op, ast.BitOr):
        return _dedupe(left + right)
    if len(left) != 1 or len(right) != 1:
        return []
    if isinstance(node.op, ast.Add):
        return [PolicyValue(left[0].value + right[0].value)]
    if isinstance(node.op, ast.Div):
        return [PolicyValue(_path_join(left[0].value, right[0].value))]
    return []


def _resolve_atom(node: ast.AST, env: dict[str, list[PolicyValue]]) -> list[PolicyValue]:
    """Resolve constants, names, and repository-root chains."""
    if isinstance(node, ast.Constant):
        if isinstance(node.value, (str, int, float, bool)):
            return [PolicyValue(str(node.value))]
        return []
    if isinstance(node, (ast.Subscript, ast.Attribute)):
        return [PolicyValue("")] if _is_repo_root_chain(node) else []
    if isinstance(node, ast.Name):
        return [PolicyValue("")] if node.id == "REPO_ROOT" else list(env.get(node.id, []))
    return []


def _resolve(
    node: ast.AST,
    env: dict[str, list[PolicyValue]],
    path: str,
    *,
    reasoned_pairs: bool = False,
) -> list[PolicyValue]:
    """Safely resolve the literal/computed expression subset used by checkers."""
    result: list[PolicyValue] = []
    if isinstance(node, (ast.Constant, ast.Subscript, ast.Attribute, ast.Name)):
        result = _resolve_atom(node, env)
    elif isinstance(node, (ast.Tuple, ast.List, ast.Set)):
        result = _resolve_collection(node, env, path, reasoned_pairs=reasoned_pairs)
    elif isinstance(node, ast.Dict):
        result = _resolve_dict(node, env, path)
    elif isinstance(node, ast.Call):
        result = _dedupe(_resolve_call(node, env, path, reasoned_pairs=reasoned_pairs))
    elif isinstance(node, ast.BinOp):
        result = _resolve_binary(node, env, path)
    elif isinstance(node, (ast.Starred, ast.GeneratorExp, ast.ListComp, ast.SetComp)):
        result = _resolve_spread(node, env, path, reasoned_pairs=reasoned_pairs)
    elif isinstance(node, ast.JoinedStr):
        result = _resolve_joined(node, env, path)
    elif isinstance(node, ast.IfExp):
        result = _dedupe(
            _resolve(node.body, env, path, reasoned_pairs=reasoned_pairs)
            + _resolve(node.orelse, env, path, reasoned_pairs=reasoned_pairs)
        )
    return result


def _resolve_spread(
    node: ast.AST,
    env: dict[str, list[PolicyValue]],
    path: str,
    *,
    reasoned_pairs: bool,
) -> list[PolicyValue]:
    """Resolve starred spreads and statically-evaluable comprehensions."""
    if isinstance(node, ast.Starred):
        return _resolve(node.value, env, path, reasoned_pairs=reasoned_pairs)
    return _resolve_comprehension(node, env, path)


def _string_leaves(node: ast.AST) -> list[PolicyValue]:
    """Bind every string constant inside a structured table, sorted exactly."""
    leaves = sorted(
        {
            child.value
            for child in ast.walk(node)
            if isinstance(child, ast.Constant) and isinstance(child.value, str)
        }
    )
    return [PolicyValue(value) for value in leaves]


def _callable_table(node: ast.AST) -> list[PolicyValue]:
    """Bind a dispatch table by the exact names of the callables it holds."""
    names = sorted(
        {child.id for child in ast.walk(node) if isinstance(child, ast.Name)}
        | {child.attr for child in ast.walk(node) if isinstance(child, ast.Attribute)}
    )
    return [PolicyValue(value) for value in names]


def _is_empty_literal(node: ast.AST) -> bool:
    """Return whether a value is an explicitly empty literal container."""
    if isinstance(node, (ast.Tuple, ast.List, ast.Set)) and not node.elts:
        return True
    if isinstance(node, ast.Dict) and not node.keys:
        return True
    if isinstance(node, ast.Call) and _call_name(node) in {"frozenset", "set", "tuple", "dict"}:
        return not node.args or _is_empty_literal(node.args[0])
    return False


def _assignment(node: ast.AST) -> tuple[str, ast.AST] | None:
    """Return a single-name module assignment."""
    if (
        isinstance(node, ast.Assign)
        and len(node.targets) == 1
        and isinstance(node.targets[0], ast.Name)
    ):
        return node.targets[0].id, node.value
    if (
        isinstance(node, ast.AnnAssign)
        and isinstance(node.target, ast.Name)
        and node.value is not None
    ):
        return node.target.id, node.value
    return None


def _authority_records(
    rel: str,
    line: int,
    name: str,
    resolved: list[PolicyValue],
    schema: AuthoritySchema,
) -> tuple[list[Suppression], list[Finding]]:
    """Build all concrete rows for one classified authority."""
    identity = f"{rel}:{name}"
    if not resolved and identity not in _EMPTY_AUTHORITIES:
        finding = Finding(
            "unresolved-checker-scope-authority",
            f"{name} uses an unsupported expression",
            rel,
            line,
        )
        return [], [finding]
    shared_reason = _SUBTYPE_REASONS[schema.subtype]
    rows = [
        Suppression(
            rel,
            line,
            1,
            "checker-scope-control",
            "repository-checker",
            schema.subtype,
            name,
            f"value:{value.value}",
            value.reason or shared_reason,
            "module-ast-authority",
            ownership(rel),
            (),
            evidence=(f"authority:{rel}:{name}:{value.value}",),
        )
        for value in resolved
    ]
    return rows, []


def _stack_exemptions(
    node: ast.AST, env: dict[str, list[PolicyValue]], path: str
) -> list[PolicyValue]:
    """Resolve exact ``(translation-unit, function, ceiling, reason)`` rows."""
    if not isinstance(node, (ast.Tuple, ast.List)):
        return []
    values: list[PolicyValue] = []
    for child in node.elts:
        if not isinstance(child, (ast.Tuple, ast.List)) or len(child.elts) != GAP_MIN_ARGS:
            return []
        fields = [_resolve(field, env, path) for field in child.elts[:3]]
        reason = _resolve(child.elts[3], env, path)
        if any(len(field) != 1 for field in fields) or len(reason) != 1:
            return []
        if not any(character.isalnum() for character in reason[0].value):
            return []
        identity = ":".join(field[0].value for field in fields)
        values.append(PolicyValue(identity, reason[0].value))
    return values


def _resolve_registered(
    node: ast.AST,
    env: dict[str, list[PolicyValue]],
    path: str,
    schema: AuthoritySchema,
) -> list[PolicyValue]:
    """Resolve one closed AST shape selected by its explicit registry entry."""
    if schema.mode == "expression-digest":
        expression = ast.dump(node, annotate_fields=True, include_attributes=False)
        digest = hashlib.sha256(expression.encode("utf-8")).hexdigest()
        resolved = [PolicyValue(f"sha256:{digest}")]
    elif schema.mode == "reasoned-pairs":
        resolved = _resolve(node, env, path, reasoned_pairs=True)
    elif schema.mode == "stack-exemptions":
        resolved = _stack_exemptions(node, env, path)
    elif schema.mode == "string-leaves":
        resolved = _string_leaves(node)
    elif schema.mode == "callable-table":
        resolved = _callable_table(node)
    elif schema.mode.startswith("derived-"):
        resolved = []
    else:
        resolved = _resolve(node, env, path)
    return resolved


def _census_findings(
    rel: str,
    tree: ast.Module,
    census: list[tuple[str, int, ast.AST | None]],
    module_authorities: dict[str, frozenset[str]],
) -> list[Finding]:
    """Enforce classification, shape, immutability, and sink rules for one file."""
    findings: list[Finding] = []
    has_selftest = any(
        isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and "selftest" in node.name
        for node in ast.walk(tree)
    )
    for name, line, value_node in census:
        identity = f"{rel}:{name}"
        if identity in AUTHORITY_SCHEMAS:
            continue
        category = NON_AUTHORITIES.get(identity)
        if category is None:
            findings.append(
                Finding(
                    "unclassified-checker-constant",
                    f"{name} is neither a registered authority nor a classified non-authority",
                    rel,
                    line,
                )
            )
        elif category not in NON_AUTHORITY_CATEGORIES:
            findings.append(
                Finding(
                    "non-authority-shape-mismatch",
                    f"{name}: unknown non-authority category {category}",
                    rel,
                    line,
                )
            )
        elif value_node is not None:
            problem = shape_problem(rel, category, value_node, has_selftest=has_selftest)
            if problem is not None:
                findings.append(
                    Finding("non-authority-shape-mismatch", f"{name}: {problem}", rel, line)
                )
    local_authorities = {
        name for name, _line, _value in census if f"{rel}:{name}" in AUTHORITY_SCHEMAS
    }
    findings.extend(mutation_findings(rel, tree, local_authorities, module_authorities))
    findings.extend(sink_findings(rel, tree))
    return findings


def _module_authorities() -> dict[str, frozenset[str]]:
    """Map checker module basenames to their registered authority names."""
    modules: dict[str, set[str]] = {}
    for identity in AUTHORITY_SCHEMAS:
        path, _colon, name = identity.rpartition(":")
        modules.setdefault(Path(path).stem, set()).add(name)
    return {module: frozenset(names) for module, names in modules.items()}


def _condition_dependent(value_node: ast.AST) -> bool:
    """Return whether an authority value depends on a runtime condition."""
    return any(isinstance(node, ast.IfExp) for node in ast.walk(value_node))


def _scan_scope_file(
    root: Path, rel: str, module_authorities: dict[str, frozenset[str]]
) -> tuple[dict[str, ResolvedAuthority], list[Finding], set[str], set[str]]:
    """Parse one checker, resolve authorities, and enforce the constant census."""
    try:
        tree = ast.parse((root / rel).read_text(encoding="utf-8"), filename=rel)
    except (OSError, SyntaxError) as exc:
        return {}, [Finding("checker-scope-ast", str(exc), rel)], set(), set()
    findings: list[Finding] = []
    authorities: dict[str, ResolvedAuthority] = {}
    diagnosed: set[str] = set()
    env: dict[str, list[PolicyValue]] = {}
    census = census_bindings(tree)
    findings.extend(_census_findings(rel, tree, census, module_authorities))
    for node in tree.body:
        item = _assignment(node)
        if item is None:
            continue
        name, value_node = item
        identity = f"{rel}:{name}"
        schema = AUTHORITY_SCHEMAS.get(identity)
        if (
            schema is not None
            and schema.mode != "expression-digest"
            and _condition_dependent(value_node)
        ):
            findings.append(
                Finding(
                    "condition-dependent-authority",
                    f"{name} is built from a runtime condition and cannot be authenticated",
                    rel,
                    node.lineno,
                )
            )
            diagnosed.add(identity)
            authorities[identity] = ResolvedAuthority(rel, node.lineno, name, schema, ())
            continue
        resolved = (
            _resolve_registered(value_node, env, rel, schema)
            if schema is not None
            else _resolve(value_node, env, rel)
        )
        if resolved or isinstance(value_node, (ast.Tuple, ast.List, ast.Set, ast.Dict)):
            env[name] = resolved
        if schema is not None:
            if not resolved and _is_empty_literal(value_node):
                diagnosed.add(identity)
            authorities[identity] = ResolvedAuthority(
                rel, node.lineno, name, schema, tuple(resolved)
            )
    census_identities = {f"{rel}:{name}" for name, _line, _value in census}
    return authorities, findings, diagnosed, census_identities


def _filtered_values(
    values: tuple[PolicyValue, ...], *, reason_terms: tuple[str, ...]
) -> tuple[PolicyValue, ...]:
    """Select reasoned prefixes through one explicit imported-authority contract."""
    return tuple(
        PolicyValue(item.value)
        for item in values
        if any(term in item.reason.lower() for term in reason_terms)
    )


def _classified_values(
    values: tuple[PolicyValue, ...], classification: str
) -> tuple[PolicyValue, ...]:
    """Select mapping keys carrying one exact semantic classification."""
    suffix = f":{classification}"
    return tuple(
        PolicyValue(item.value.removesuffix(suffix))
        for item in values
        if item.value.endswith(suffix)
    )


def _schema_digest() -> str:
    """Authenticate every registered identity with its subtype and value mode."""
    payload = {
        identity: f"{schema.subtype}/{schema.mode}"
        for identity, schema in sorted(AUTHORITY_SCHEMAS.items())
    }
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def _live_registry_values() -> dict[str, tuple[PolicyValue, ...]]:
    """Bind the classification registries' exact live content as authority values."""
    schema_binding = (
        PolicyValue(f"entries:{len(AUTHORITY_SCHEMAS)}"),
        PolicyValue(f"sha256:{_schema_digest()}"),
    )
    nonauth_binding = (
        PolicyValue(f"entries:{len(NON_AUTHORITIES)}"),
        PolicyValue(f"sha256:{classification_digest(AUTHORITY_SCHEMAS, NON_AUTHORITIES)}"),
    )
    return {
        "scripts/checks/suppression_scope_registry.py:AUTHORITY_SCHEMAS": schema_binding,
        "scripts/checks/suppression_scope_registry.py:_SCHEMA_GROUPS": schema_binding,
        "scripts/checks/suppression_nonauth_registry.py:_GROUPS": nonauth_binding,
        "scripts/checks/suppression_scope_registry.py:NON_AUTHORITY_CATEGORIES": tuple(
            PolicyValue(category, reason)
            for category, reason in sorted(NON_AUTHORITY_CATEGORIES.items())
        ),
        "scripts/checks/suppression_nonauth_registry.py:NON_AUTHORITIES": nonauth_binding,
    }


def _apply_derived(authorities: dict[str, ResolvedAuthority]) -> None:
    """Resolve registered cross-module comprehensions from their authenticated source."""
    exemptions = authorities["scripts/checks/lint_coverage_rules.py:EXEMPT_PREFIXES"].values
    path_classes = authorities["scripts/checks/lint_coverage_rules.py:PATH_CLASS"].values
    extension_classes = authorities["scripts/checks/lint_coverage_rules.py:EXT_CLASS"].values
    replacements = {
        **_live_registry_values(),
        "scripts/checks/suppression_catalog.py:VENDOR_PREFIXES": _filtered_values(
            exemptions, reason_terms=("vendored", "soup")
        ),
        "scripts/checks/suppression_catalog.py:GENERATED_PREFIXES": _filtered_values(
            exemptions, reason_terms=("generated", "emitted")
        ),
        "scripts/checks/suppression_catalog.py:BINARY_SUFFIXES": _classified_values(
            extension_classes, "binary"
        ),
        "scripts/checks/check_no_null.py:GENERATED_SOURCE_PATHS": _classified_values(
            path_classes, "generated-source"
        ),
        "scripts/checks/check_no_stdio_streams.py:GENERATED_SOURCE_PATHS": _classified_values(
            path_classes, "generated-source"
        ),
    }
    for identity, values in replacements.items():
        authority = authorities[identity]
        authorities[identity] = ResolvedAuthority(
            authority.path,
            authority.line,
            authority.name,
            authority.schema,
            values,
        )


SELF_PIN_IDENTITIES = frozenset(
    {
        "scripts/checks/suppression_checker_scope.py:EXPECTED_AUTHORITY_VALUE_SHA256",
        "scripts/checks/suppression_checker_scope.py:EXPECTED_AUTHORITY_REASON_SHA256",
        "scripts/checks/suppression_checker_scope.py:EXPECTED_CLASSIFICATION_SHA256",
    }
)


def _authority_value_digest(authorities: dict[str, ResolvedAuthority]) -> str:
    """Authenticate each authority name and its exact concrete values.

    The digest pins themselves are excluded from the domain: a digest that
    hashed its own committed value could never reach a fixed point.
    """
    payload = {
        key: sorted(item.value for item in authority.values)
        for key, authority in sorted(authorities.items())
        if key not in SELF_PIN_IDENTITIES
    }
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def _authority_reason_digest(records: list[Suppression]) -> str:
    """Authenticate rationales separately from authority/value parsing."""
    payload: dict[str, list[tuple[str, str]]] = {}
    for item in records:
        if f"{item.path}:{item.directive}" in SELF_PIN_IDENTITIES:
            continue
        key = f"{item.path}:{item.directive}"
        payload.setdefault(key, []).append((item.scope.removeprefix("value:"), item.reason))
    encoded = json.dumps(
        {key: sorted(values) for key, values in sorted(payload.items())},
        sort_keys=True,
        separators=(",", ":"),
    ).encode()
    return hashlib.sha256(encoded).hexdigest()


def _collect_authorities(
    root: Path, paths: list[str]
) -> tuple[dict[str, ResolvedAuthority], list[Finding], set[str]]:
    """Collect every registered assignment and enforce the exhaustive census."""
    findings: list[Finding] = []
    authorities: dict[str, ResolvedAuthority] = {}
    diagnosed: set[str] = set()
    module_authorities = _module_authorities()
    candidates = [
        rel for rel in paths if rel.startswith(CANDIDATE_PREFIXES) and rel.endswith(".py")
    ]
    census_total = 0
    seen_identities: set[str] = set()
    for rel in candidates:
        file_authorities, problems, file_diagnosed, census_identities = _scan_scope_file(
            root, rel, module_authorities
        )
        findings.extend(problems)
        authorities.update(file_authorities)
        diagnosed.update(file_diagnosed)
        census_total += len(census_identities)
        seen_identities.update(census_identities)
    if census_total < MIN_CENSUS_CONSTANTS:
        findings.append(
            Finding(
                "checker-census-floor",
                f"only {census_total} module constants seen; floor is {MIN_CENSUS_CONSTANTS}",
            )
        )
    stale = sorted(identity for identity in NON_AUTHORITIES if identity not in seen_identities)
    findings.extend(
        Finding("stale-checker-classification", f"{identity} no longer exists")
        for identity in stale
    )
    missing = sorted(set(AUTHORITY_SCHEMAS) - set(authorities))
    findings.extend(Finding("missing-checker-scope-authority", identity) for identity in missing)
    if not missing:
        _apply_derived(authorities)
    findings.extend(
        repository_mutation_findings(root, paths, module_authorities, frozenset(candidates))
    )
    return authorities, findings, diagnosed


def _records_for_authorities(
    authorities: dict[str, ResolvedAuthority],
    diagnosed: set[str],
) -> tuple[list[Suppression], list[Finding]]:
    """Render source-located inventory rows after cross-module derivation."""
    records: list[Suppression] = []
    findings: list[Finding] = []
    for identity, authority in sorted(authorities.items()):
        rows, problems = _authority_records(
            authority.path,
            authority.line,
            authority.name,
            list(authority.values),
            authority.schema,
        )
        records.extend(rows)
        if identity not in diagnosed:
            findings.extend(problems)
    return records, findings


def scan_checker_scope_controls(
    root: Path, paths: list[str]
) -> tuple[list[Suppression], list[Finding]]:
    """Inventory every reviewed checker authority and fail unknown/unresolved shapes."""
    authorities, findings, diagnosed = _collect_authorities(root, paths)
    records, record_findings = _records_for_authorities(authorities, diagnosed)
    findings.extend(record_findings)
    live_classification = classification_digest(AUTHORITY_SCHEMAS, NON_AUTHORITIES)
    if live_classification != EXPECTED_CLASSIFICATION_SHA256:
        findings.append(
            Finding(
                "checker-classification-digest",
                f"found {live_classification}; audited contract is "
                f"{EXPECTED_CLASSIFICATION_SHA256}",
            )
        )
    if len(authorities) != EXPECTED_AUTHORITIES:
        findings.append(
            Finding(
                "checker-scope-authority-count",
                f"found {len(authorities)}; audited contract is {EXPECTED_AUTHORITIES}",
            )
        )
    if EXPECTED_VALUES and len(records) != EXPECTED_VALUES:
        findings.append(
            Finding(
                "checker-scope-value-count",
                f"found {len(records)}; audited contract is {EXPECTED_VALUES}",
            )
        )
    value_digest = _authority_value_digest(authorities)
    if value_digest != EXPECTED_AUTHORITY_VALUE_SHA256:
        findings.append(
            Finding(
                "checker-scope-value-digest",
                f"found {value_digest}; audited contract is {EXPECTED_AUTHORITY_VALUE_SHA256}",
            )
        )
    reason_digest = _authority_reason_digest(records)
    if reason_digest != EXPECTED_AUTHORITY_REASON_SHA256:
        findings.append(
            Finding(
                "checker-scope-reason-digest",
                f"found {reason_digest}; audited contract is {EXPECTED_AUTHORITY_REASON_SHA256}",
            )
        )
    return records, findings
