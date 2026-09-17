# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Exhaustive census, immutability, and inline-scope rules for checker constants.

Three fail-closed properties are enforced over every checker module:

1. CENSUS -- every module-level ALL_CAPS binding must be classified, either as
   a typed scope/control authority (``AUTHORITY_SCHEMAS``) or as an explicit
   non-authority carrying a category reason (``NON_AUTHORITIES``). A constant
   in neither registry fails; a registry row whose constant no longer exists
   fails; a non-authority whose value shape contradicts its category fails.
   Walrus bindings count: ``_ = (ROOTS := ("evil/",))`` binds ``ROOTS`` at
   module level just as an assignment statement does.
2. IMMUTABILITY -- a registered authority is bound exactly once, at module
   top level, by a plain assignment. Reassignment, augmented assignment,
   conditional or nested binding, subscript/attribute stores, ``del``,
   ``global`` rebinding, mutator method calls, and mutation reached through
   ANY alias of the object are rejected, so the authenticated digest always
   describes the runtime value. Aliasing is resolved semantically rather than
   by token: an alias of a module attribute (``d = mod.AUTHORITY``), a
   namespace read (``vars(mod)["AUTHORITY"]``, ``getattr(mod, "AUTHORITY")``,
   ``mod.__dict__["AUTHORITY"]``), a dynamically imported module
   (``__import__("mod").AUTHORITY``), an unbound mutator
   (``dict.update(mod.AUTHORITY, ...)``), ``setattr``/``delattr``, and the
   in-place ``operator`` functions all reach the same guarded object and are
   all reported.
3. INLINE SCOPE -- path-shaped scope literals may not be smuggled into filter
   sinks (``startswith``/``endswith`` prefix tuples, or ``Path.parts``
   membership and set algebra). Both sinks resolve their operand through
   local names, walrus bindings, aliases, conditional expressions, container
   constructors, and container algebra, so a literal moved out of the call
   is still found. Scope data must be a declared module authority.
"""

from __future__ import annotations

import ast
import hashlib
import json
import re
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path

from suppression_model import Finding

MODULE_CONSTANT_RE = re.compile(r"^_{0,2}[A-Z][A-Z0-9_]*$")
PATHLIKE_FRAGMENT_RE = re.compile(r"[A-Za-z0-9_.-]/")
MUTATOR_METHODS = frozenset(
    {
        "add",
        "append",
        "clear",
        "discard",
        "extend",
        "insert",
        "pop",
        "popitem",
        "remove",
        "reverse",
        "setdefault",
        "sort",
        "update",
    }
)
OPERATOR_MUTATORS = frozenset(
    {
        "delitem",
        "iadd",
        "iand",
        "iconcat",
        "ifloordiv",
        "ilshift",
        "imatmul",
        "imod",
        "imul",
        "ior",
        "ipow",
        "irshift",
        "isub",
        "itruediv",
        "ixor",
        "setitem",
    }
)
ATTRIBUTE_MUTATORS = frozenset({"delattr", "setattr"})
NAMESPACE_CALLS = frozenset({"globals", "vars"})
MODULE_IMPORT_CALLS = frozenset({"__import__", "import_module"})
UNBOUND_MUTATOR_TYPES = frozenset({"bytearray", "dict", "list", "set"})
CONTAINER_CALLS = frozenset({"frozenset", "list", "set", "tuple"})
PREFIX_SINK_METHODS = frozenset({"endswith", "startswith"})
LITERAL_CONTAINERS = (ast.Tuple, ast.List, ast.Set, ast.Dict)
SCOPE_NODES = (ast.AsyncFunctionDef, ast.ClassDef, ast.FunctionDef, ast.Lambda)
SET_ALGEBRA_OPS = (ast.BitAnd, ast.BitOr, ast.BitXor, ast.Sub)


def _scope_nodes(scope: ast.AST) -> list[ast.AST]:
    """Return every node inside one scope, never descending into a nested scope."""
    nodes: list[ast.AST] = []
    for child in ast.iter_child_nodes(scope):
        nodes.append(child)
        if not isinstance(child, SCOPE_NODES):
            nodes.extend(_scope_nodes(child))
    return nodes


def _call_name(node: ast.Call) -> str:
    """Return the simple or attribute name a call invokes, without evaluating it."""
    func = node.func
    if isinstance(func, ast.Name):
        return func.id
    if isinstance(func, ast.Attribute):
        return func.attr
    return ""


def _constant_text(node: ast.AST) -> str | None:
    """Return the exact string a constant expression names, if it names one."""
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    return None


def _name_bindings(nodes: Iterable[ast.AST]) -> list[tuple[str, ast.AST]]:
    """Return every simple ``name = expression`` binding among some nodes."""
    pairs: list[tuple[str, ast.AST]] = []
    for node in nodes:
        if isinstance(node, ast.Assign):
            pairs.extend(
                (target.id, node.value) for target in node.targets if isinstance(target, ast.Name)
            )
        elif (
            isinstance(node, (ast.NamedExpr, ast.AnnAssign))
            and isinstance(node.target, ast.Name)
            and node.value
        ):
            pairs.append((node.target.id, node.value))
    return pairs


def _statement_bindings(node: ast.stmt) -> list[tuple[str, int, ast.AST | None]]:
    """Return the ALL_CAPS names one module-level statement binds directly."""
    bindings: list[tuple[str, int, ast.AST | None]] = []
    if isinstance(node, ast.Assign):
        for target in node.targets:
            names: list[ast.Name] = []
            if isinstance(target, ast.Name):
                names = [target]
            elif isinstance(target, (ast.Tuple, ast.List)):
                names = [item for item in target.elts if isinstance(item, ast.Name)]
            bindings.extend(
                (name.id, node.lineno, node.value)
                for name in names
                if MODULE_CONSTANT_RE.match(name.id)
            )
        return bindings
    target = getattr(node, "target", None)
    if (
        isinstance(node, (ast.AnnAssign, ast.AugAssign))
        and isinstance(target, ast.Name)
        and MODULE_CONSTANT_RE.match(target.id)
    ):
        bindings.append((target.id, node.lineno, node.value))
    return bindings


def _walrus_bindings(node: ast.stmt) -> list[tuple[str, int, ast.AST | None]]:
    """Return the ALL_CAPS names one module-level statement binds by walrus.

    A ``def``/``class``/``lambda`` opens its own namespace, so a walrus inside
    one binds there, not at module level, and is not a module constant.
    """
    if isinstance(node, SCOPE_NODES):
        return []
    return [
        (child.target.id, child.lineno, child.value)
        for child in _scope_nodes(node)
        if isinstance(child, ast.NamedExpr)
        and isinstance(child.target, ast.Name)
        and MODULE_CONSTANT_RE.match(child.target.id)
    ]


def census_bindings(tree: ast.Module) -> list[tuple[str, int, ast.AST | None]]:
    """Return every module-level ALL_CAPS binding as ``(name, line, value)``."""
    bindings: list[tuple[str, int, ast.AST | None]] = []
    for node in tree.body:
        bindings.extend(_statement_bindings(node))
        bindings.extend(_walrus_bindings(node))
    return bindings


def _store_root(node: ast.AST) -> ast.AST:
    """Return the base expression a subscript/attribute store mutates."""
    while isinstance(node, (ast.Subscript, ast.Attribute)):
        node = node.value
    return node


def _store_chain(node: ast.AST) -> list[ast.AST]:
    """Return a store target followed by every expression it is an element of."""
    chain = [node]
    while isinstance(node, (ast.Subscript, ast.Attribute)):
        node = node.value
        chain.append(node)
    return chain


def _import_bindings(
    tree: ast.Module, module_authorities: dict[str, frozenset[str]]
) -> tuple[dict[str, str], dict[str, str]]:
    """Return authority-import aliases and checker-module aliases in one file."""
    imported: dict[str, str] = {}
    module_aliases: dict[str, str] = {}
    for node in ast.walk(tree):
        if isinstance(node, ast.ImportFrom) and node.module is not None and node.level == 0:
            module = node.module.rpartition(".")[2]
            names = module_authorities.get(module)
            if names is None:
                continue
            for alias in node.names:
                if alias.name in names:
                    imported[alias.asname or alias.name] = f"{module}:{alias.name}"
        elif isinstance(node, ast.Import):
            for alias in node.names:
                module = alias.name.rpartition(".")[2]
                if module in module_authorities:
                    module_aliases[alias.asname or alias.name] = module
    return imported, module_aliases


def _mutation(rel: str, line: int, name: str, how: str) -> Finding:
    """Build one authority-mutation finding."""
    return Finding(
        "checker-authority-mutation",
        f"{name} is mutated after authentication ({how})",
        rel,
        line,
    )


@dataclass(frozen=True)
class _MutationScan:
    """One module's guarded names and cross-module authority bindings."""

    rel: str
    guarded: frozenset[str]
    module_lookup: dict[str, str]
    module_authorities: dict[str, frozenset[str]]

    def module_of(self, node: ast.AST) -> str | None:
        """Return the checker module an expression names, without importing it."""
        if isinstance(node, ast.Name):
            return self.module_lookup.get(node.id)
        if isinstance(node, ast.Call) and _call_name(node) in MODULE_IMPORT_CALLS and node.args:
            text = _constant_text(node.args[0])
            module = text.rpartition(".")[2] if text is not None else ""
            return module if module in self.module_authorities else None
        return None

    def namespace_of(self, node: ast.AST) -> str | None:
        """Return the module whose writable namespace mapping an expression is."""
        if isinstance(node, ast.Call) and _call_name(node) in NAMESPACE_CALLS:
            return "" if not node.args else self.module_of(node.args[0])
        if isinstance(node, ast.Attribute) and node.attr == "__dict__":
            return self.module_of(node.value)
        return None

    def holds(self, module: str, name: str) -> bool:
        """Return whether one module binds a registered authority of that name."""
        if not module:
            return name in self.guarded
        return name in self.module_authorities.get(module, frozenset())

    def authority_label(self, node: ast.AST) -> str | None:
        """Return the label of the registered authority an expression resolves to."""
        if isinstance(node, ast.Name) and node.id in self.guarded:
            return node.id
        if isinstance(node, ast.Attribute):
            module = self.module_of(node.value)
            if module is not None and self.holds(module, node.attr):
                return f"{module}.{node.attr}"
        if isinstance(node, ast.Subscript):
            return self._namespace_item(self.namespace_of(node.value), node.slice)
        if isinstance(node, ast.Call) and _call_name(node) == "getattr" and len(node.args) > 1:
            return self._namespace_item(self.module_of(node.args[0]), node.args[1])
        return None

    def _namespace_item(self, module: str | None, key: ast.AST) -> str | None:
        """Return the label of one authority reached through a module namespace."""
        name = _constant_text(key)
        if module is None or name is None or not self.holds(module, name):
            return None
        return f"{module}.{name}" if module else name

    def store_findings(
        self, node: ast.Assign | ast.AugAssign | ast.Delete, targets: list[ast.AST], how: str
    ) -> list[Finding]:
        """Reject stores and deletions that reach a guarded object."""
        findings: list[Finding] = []
        for target in targets:
            if isinstance(target, (ast.Subscript, ast.Attribute)):
                findings.extend(self._element_findings(node.lineno, target, how))
            elif isinstance(target, ast.Name) and isinstance(node, (ast.AugAssign, ast.Delete)):
                if target.id in self.guarded:
                    findings.append(_mutation(self.rel, node.lineno, target.id, how))
            elif isinstance(target, (ast.Tuple, ast.List)):
                findings.extend(self.store_findings(node, list(target.elts), how))
        return findings

    def _element_findings(self, line: int, target: ast.AST, how: str) -> list[Finding]:
        """Reject one subscript or attribute store reaching a registered authority."""
        root = _store_root(target)
        if isinstance(root, ast.Name) and root.id in self.guarded:
            return [_mutation(self.rel, line, root.id, f"{how} via element store")]
        for index, element in enumerate(_store_chain(target)):
            label = self.authority_label(element)
            if label is not None:
                detail = how if index == 0 else f"{how} via element store"
                return [_mutation(self.rel, line, label, detail)]
        return []

    def call_findings(self, node: ast.Call) -> list[Finding]:
        """Reject mutator method calls and mutating functions on guarded objects."""
        return self._method_findings(node) + self._function_findings(node)

    def _method_findings(self, node: ast.Call) -> list[Finding]:
        """Reject a mutator method invoked on an authority or a module namespace."""
        func = node.func
        if not isinstance(func, ast.Attribute) or func.attr not in MUTATOR_METHODS:
            return []
        how = f".{func.attr}() call"
        label = self.authority_label(func.value)
        if label is not None:
            return [_mutation(self.rel, node.lineno, label, how)]
        if self.namespace_of(func.value) is not None:
            return [_mutation(self.rel, node.lineno, "module namespace", f"{how} on a namespace")]
        unbound = isinstance(func.value, ast.Name) and func.value.id in UNBOUND_MUTATOR_TYPES
        label = self.authority_label(node.args[0]) if unbound and node.args else None
        return [_mutation(self.rel, node.lineno, label, f"unbound {how}")] if label else []

    def _function_findings(self, node: ast.Call) -> list[Finding]:
        """Reject setattr/delattr and in-place operator functions on an authority."""
        name = _call_name(node)
        if not node.args or name not in ATTRIBUTE_MUTATORS | OPERATOR_MUTATORS:
            return []
        first = node.args[0]
        module = self.module_of(first) if name in ATTRIBUTE_MUTATORS else None
        if module is not None:
            return [_mutation(self.rel, node.lineno, module, f"{name} on checker module")]
        label = self.authority_label(first)
        if label is not None:
            return [_mutation(self.rel, node.lineno, label, f"{name}() call")]
        if self.namespace_of(first) is not None:
            return [
                _mutation(self.rel, node.lineno, "module namespace", f"{name}() on a namespace")
            ]
        return []


def _protected_aliases(
    tree: ast.Module,
    protected: set[str],
    module_lookup: dict[str, str],
    module_authorities: dict[str, frozenset[str]],
) -> set[str]:
    """Return same-module names bound to a protected authority object."""
    pairs = _name_bindings(ast.walk(tree))
    aliases: set[str] = set()
    changed = True
    while changed:
        changed = False
        scan = _MutationScan("", frozenset(protected | aliases), module_lookup, module_authorities)
        for name, value in pairs:
            if name in protected or name in aliases:
                continue
            if scan.authority_label(value) is not None:
                aliases.add(name)
                changed = True
    return aliases


def _binding_findings(
    rel: str,
    node: ast.Assign | ast.AnnAssign,
    local_authorities: set[str],
    top_level: set[ast.stmt],
    bound: set[str],
) -> list[Finding]:
    """Reject nested, conditional, and repeated authority bindings."""
    findings: list[Finding] = []
    targets = node.targets if isinstance(node, ast.Assign) else [node.target]
    for target in targets:
        if not isinstance(target, ast.Name) or target.id not in local_authorities:
            continue
        if node not in top_level:
            problem = f"{target.id} is bound inside control flow or a function"
        elif target.id in bound:
            problem = f"{target.id} is bound more than once at module level"
        else:
            bound.add(target.id)
            continue
        findings.append(Finding("checker-authority-rebinding", problem, rel, node.lineno))
    return findings


def _dynamic_store_finding(rel: str, node: ast.Subscript) -> Finding | None:
    """Reject writes through globals()/vars() namespaces."""
    base = node.value
    if (
        isinstance(base, ast.Call)
        and isinstance(base.func, ast.Name)
        and base.func.id in NAMESPACE_CALLS
    ):
        return Finding(
            "checker-authority-mutation",
            "dynamic namespace store cannot be statically authenticated",
            rel,
            node.lineno,
        )
    return None


def mutation_findings(
    rel: str,
    tree: ast.Module,
    local_authorities: set[str],
    module_authorities: dict[str, frozenset[str]],
) -> list[Finding]:
    """Reject every write that changes a registered authority after binding."""
    imported, module_lookup = _import_bindings(tree, module_authorities)
    protected = set(local_authorities) | set(imported)
    aliases = _protected_aliases(tree, protected, module_lookup, module_authorities)
    scan = _MutationScan(rel, frozenset(protected | aliases), module_lookup, module_authorities)
    findings: list[Finding] = []
    top_level = set(tree.body)
    bound: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, (ast.Assign, ast.AnnAssign)):
            findings.extend(_binding_findings(rel, node, local_authorities, top_level, bound))
            targets = node.targets if isinstance(node, ast.Assign) else [node.target]
            findings.extend(scan.store_findings(node, list(targets), "assignment"))
        elif isinstance(node, ast.AugAssign):
            findings.extend(scan.store_findings(node, [node.target], "augmented assignment"))
        elif isinstance(node, ast.Delete):
            findings.extend(scan.store_findings(node, list(node.targets), "del"))
        elif isinstance(node, ast.Global):
            findings.extend(
                _mutation(rel, node.lineno, name, "global rebinding")
                for name in node.names
                if name in local_authorities
            )
        elif isinstance(node, ast.Call):
            findings.extend(scan.call_findings(node))
        elif isinstance(node, ast.Subscript) and isinstance(node.ctx, ast.Store):
            problem = _dynamic_store_finding(rel, node)
            if problem is not None:
                findings.append(problem)
    return findings


def _container_elements(node: ast.AST) -> list[ast.AST] | None:
    """Return a literal container's element nodes, or None if it is not one."""
    if isinstance(node, (ast.Tuple, ast.List, ast.Set)):
        return list(node.elts)
    if isinstance(node, ast.Dict):
        return [key for key in node.keys if key is not None]
    return None


def _pathlike_container(node: ast.AST) -> bool:
    """Return whether a literal container carries path-fragment scope strings."""
    elements = _container_elements(node)
    if elements is None:
        return False
    return any(
        isinstance(item, ast.Constant)
        and isinstance(item.value, str)
        and PATHLIKE_FRAGMENT_RE.search(item.value)
        for item in elements
    )


def _resolve_containers(
    node: ast.AST, bindings: dict[str, list[ast.AST]], seen: frozenset[str]
) -> list[ast.AST]:
    """Return every literal container one filter-sink operand can evaluate to."""
    if _container_elements(node) is not None:
        return [node]
    if isinstance(node, ast.Name):
        if node.id not in bindings or node.id in seen:
            return []
        nested = seen | {node.id}
        values = bindings[node.id]
        return [item for value in values for item in _resolve_containers(value, bindings, nested)]
    return _resolve_composite(node, bindings, seen)


def _resolve_composite(
    node: ast.AST, bindings: dict[str, list[ast.AST]], seen: frozenset[str]
) -> list[ast.AST]:
    """Resolve constructors, walrus bindings, conditionals, and container algebra."""
    if isinstance(node, ast.NamedExpr):
        return _resolve_containers(node.value, bindings, seen)
    if isinstance(node, ast.Call) and _call_name(node) in CONTAINER_CALLS and node.args:
        return _resolve_containers(node.args[0], bindings, seen)
    branches: tuple[ast.AST, ...] = ()
    if isinstance(node, ast.IfExp):
        branches = (node.body, node.orelse)
    elif isinstance(node, ast.BinOp) and isinstance(node.op, (ast.Add, *SET_ALGEBRA_OPS)):
        branches = (node.left, node.right)
    return [item for branch in branches for item in _resolve_containers(branch, bindings, seen)]


def _scope_bindings(scope: ast.AST, *, module: bool) -> dict[str, list[ast.AST]]:
    """Return the names one scope binds, minus the censused module constants."""
    bindings: dict[str, list[ast.AST]] = {}
    for name, value in _name_bindings(_scope_nodes(scope)):
        if module and MODULE_CONSTANT_RE.match(name):
            continue
        bindings.setdefault(name, []).append(value)
    return bindings


def _scope_table(tree: ast.Module) -> list[tuple[ast.AST, dict[str, list[ast.AST]]]]:
    """Return every scope paired with the container bindings visible inside it."""
    table: list[tuple[ast.AST, dict[str, list[ast.AST]]]] = [
        (tree, _scope_bindings(tree, module=True))
    ]
    index = 0
    while index < len(table):
        scope, visible = table[index]
        index += 1
        table.extend(
            (node, {**visible, **_scope_bindings(node, module=False)})
            for node in _scope_nodes(scope)
            if isinstance(node, SCOPE_NODES)
        )
    return table


def _reads_parts(node: ast.AST) -> bool:
    """Return whether an expression reads a path's ``parts`` tuple."""
    return any(
        isinstance(child, ast.Attribute) and child.attr == "parts" for child in ast.walk(node)
    )


def _comprehension_comparators(
    node: ast.GeneratorExp | ast.ListComp | ast.SetComp,
) -> list[tuple[int, ast.AST]]:
    """Return the membership operands of a comprehension over ``Path.parts``."""
    iterates_parts = any(
        isinstance(generator.iter, ast.Attribute) and generator.iter.attr == "parts"
        for generator in node.generators
    )
    element = getattr(node, "elt", None)
    if not iterates_parts or element is None:
        return []
    return [
        (node.lineno, compare.comparators[0])
        for compare in ast.walk(element)
        if isinstance(compare, ast.Compare)
        and len(compare.ops) == 1
        and isinstance(compare.ops[0], (ast.In, ast.NotIn))
    ]


def _membership_comparators(node: ast.AST) -> list[tuple[int, ast.AST]]:
    """Return the container operands of one ``Path.parts`` scope filter."""
    if isinstance(node, (ast.GeneratorExp, ast.ListComp, ast.SetComp)):
        return _comprehension_comparators(node)
    if isinstance(node, ast.Compare) and len(node.ops) == 1:
        over_parts = (
            isinstance(node.ops[0], (ast.In, ast.NotIn))
            and isinstance(node.left, ast.Attribute)
            and node.left.attr == "parts"
        )
        return [(node.lineno, node.comparators[0])] if over_parts else []
    if isinstance(node, ast.BinOp) and isinstance(node.op, SET_ALGEBRA_OPS):
        left = _reads_parts(node.left)
        if left != _reads_parts(node.right):
            return [(node.lineno, node.right if left else node.left)]
    return []


def _prefix_sink_findings(
    rel: str, nodes: list[ast.AST], bindings: dict[str, list[ast.AST]]
) -> list[Finding]:
    """Reject path-shaped scope containers reaching a startswith/endswith filter."""
    findings: list[Finding] = []
    for node in nodes:
        is_sink = (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and node.func.attr in PREFIX_SINK_METHODS
            and node.args
        )
        if not is_sink:
            continue
        containers = _resolve_containers(node.args[0], bindings, frozenset())
        if any(_pathlike_container(item) for item in containers):
            findings.append(
                Finding(
                    "inline-scope-literal",
                    f".{node.func.attr}() path tuple must be a declared authority",
                    rel,
                    node.lineno,
                )
            )
    return findings


def _membership_findings(
    rel: str, nodes: list[ast.AST], bindings: dict[str, list[ast.AST]]
) -> list[Finding]:
    """Reject ``Path.parts`` filters resolved against an undeclared literal."""
    findings: list[Finding] = []
    for node in nodes:
        findings.extend(
            Finding(
                "inline-scope-literal",
                "Path.parts scope filter must use a declared module authority",
                rel,
                line,
            )
            for line, comparator in _membership_comparators(node)
            if _resolve_containers(comparator, bindings, frozenset())
        )
    return findings


def _assertion_nodes(tree: ast.Module) -> set[int]:
    """Collect nodes inside test expectations, which state facts, not scope."""
    inside: set[int] = set()
    for node in ast.walk(tree):
        is_expectation = isinstance(node, ast.Assert) or (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Name)
            and node.func.id == "expect"
        )
        if is_expectation:
            inside.update(id(child) for child in ast.walk(node))
    return inside


def sink_findings(rel: str, tree: ast.Module) -> list[Finding]:
    """Reject scope literals reaching filter sinks through any binding form."""
    expectations = _assertion_nodes(tree)
    findings: list[Finding] = []
    for scope, bindings in _scope_table(tree):
        nodes = [node for node in _scope_nodes(scope) if id(node) not in expectations]
        findings.extend(_prefix_sink_findings(rel, nodes, bindings))
        findings.extend(_membership_findings(rel, nodes, bindings))
    return findings


CATEGORY_NODE_KINDS = {
    "exit-code": frozenset({"Constant"}),
    "numeric-format": frozenset(
        {"Attribute", "BinOp", "Call", "Constant", "Subscript", "Tuple", "UnaryOp"}
    ),
    "derived-runtime": frozenset(
        {
            "Attribute",
            "BinOp",
            "Call",
            "Compare",
            "Dict",
            "DictComp",
            "GeneratorExp",
            "IfExp",
            "JoinedStr",
            "List",
            "ListComp",
            "Name",
            "SetComp",
            "Subscript",
        }
    ),
}
EMPTY_CONTAINER_CATEGORIES = frozenset({"derived-runtime"})
FIXTURE_BASENAME_MARKERS = ("selftest", "fixture")
FIXTURE_OWNER_RELATIVE_PATHS = frozenset(
    {
        "scripts/checks/hil_convergence_safety_runtime_loader_harness.py",
        "scripts/checks/hil_convergence_safety_runtime_sources.py",
    }
)


def _empty_container(value: ast.AST) -> bool:
    """Return whether a value is an empty literal container (a runtime cache)."""
    if isinstance(value, (ast.Tuple, ast.List, ast.Set)):
        return not value.elts
    if isinstance(value, ast.Dict):
        return not value.keys
    return False


def _scope_shaped_literal(value: ast.AST) -> str | None:
    """Return a path-prefix string literal hiding inside a non-authority value."""
    for node in ast.walk(value):
        is_prefix = (
            isinstance(node, ast.Constant)
            and isinstance(node.value, str)
            and node.value.endswith("/")
            and PATHLIKE_FRAGMENT_RE.search(node.value)
        )
        if is_prefix:
            return node.value
    return None


def shape_problem(rel: str, category: str, value: ast.AST, *, has_selftest: bool) -> str | None:
    """Return why a non-authority value contradicts its category, if it does."""
    if category == "selftest-fixture":
        basename = Path(rel).name
        marked = rel in FIXTURE_OWNER_RELATIVE_PATHS or any(
            marker in basename for marker in FIXTURE_BASENAME_MARKERS
        )
        if not marked and not has_selftest:
            return "selftest-fixture classification outside a module with a selftest"
        return None
    if category in EMPTY_CONTAINER_CATEGORIES and _empty_container(value):
        return None
    allowed = CATEGORY_NODE_KINDS.get(category)
    if allowed is not None and type(value).__name__ not in allowed:
        return f"{type(value).__name__} value contradicts category {category}"
    prefix = _scope_shaped_literal(value)
    if prefix is not None:
        return f"path-prefix literal {prefix!r} is scope-shaped data"
    return None


def classification_digest(
    authority_schemas: dict[str, object], non_authorities: dict[str, str]
) -> str:
    """Authenticate the complete constant classification map."""
    payload = {
        **dict.fromkeys(authority_schemas, "authority"),
        **{key: f"non-authority:{category}" for key, category in non_authorities.items()},
    }
    encoded = json.dumps(dict(sorted(payload.items())), sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(encoded.encode()).hexdigest()


def repository_mutation_findings(
    root: Path,
    paths: list[str],
    module_authorities: dict[str, frozenset[str]],
    candidate_paths: frozenset[str],
) -> list[Finding]:
    """Reject imported-authority mutation from every tracked Python file."""
    findings: list[Finding] = []
    for rel in paths:
        if not rel.endswith(".py") or rel in candidate_paths:
            continue
        try:
            tree = ast.parse((root / rel).read_text(encoding="utf-8"), filename=rel)
        except (OSError, SyntaxError) as exc:
            findings.append(Finding("checker-scope-ast", str(exc), rel))
            continue
        findings.extend(mutation_findings(rel, tree, set(), module_authorities))
    return findings
