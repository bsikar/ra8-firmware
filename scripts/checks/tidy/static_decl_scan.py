# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Recognize header-only non-inline static function declarations.

The tidy router must distinguish ordinary standalone headers from textual
include fragments. This scanner applies line splicing, removes preprocessing
directives and comments, then tokenizes C-family source so declaration
whitespace and line wrapping cannot change that ownership decision.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

_TOKEN_RE = re.compile(
    r"""
    (?P<block_comment>/\*.*?\*/)
  | (?P<line_comment>//[^\n]*)
  | (?P<string>"(?:\\.|[^"\\])*")
  | (?P<char>'(?:\\.|[^'\\])*')
  | (?P<identifier>[A-Za-z_][A-Za-z0-9_]*)
  | (?P<punct>[{}()\[\];=,*])
  | (?P<space>\s+)
  | (?P<other>.)
    """,
    re.DOTALL | re.VERBOSE,
)
_IDENTIFIER_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")


class ScanError(ValueError):
    """The token stream is malformed, so ownership cannot be decided safely."""


_GROUP_CLOSE = {"(": ")", "[": "]", "{": "}"}
_GROUP_OPEN = set(_GROUP_CLOSE)
_GROUP_END = set(_GROUP_CLOSE.values())
_POINTER_QUALIFIERS = {"const", "restrict", "volatile", "_Atomic"}
_DECL_SPECIFIER_WORDS = {
    "_Alignas",
    "_Atomic",
    "_Bool",
    "_Complex",
    "_Noreturn",
    "auto",
    "bool",
    "char",
    "char8_t",
    "char16_t",
    "char32_t",
    "class",
    "const",
    "consteval",
    "constexpr",
    "double",
    "enum",
    "extern",
    "float",
    "inline",
    "__inline",
    "__inline__",
    "int",
    "long",
    "register",
    "restrict",
    "short",
    "signed",
    "static",
    "struct",
    "typedef",
    "union",
    "unsigned",
    "void",
    "volatile",
    "wchar_t",
}
_INLINE_SPECIFIERS = frozenset({"inline", "__inline", "__inline__", "constexpr", "consteval"})
_TYPE_BEARING_SPECIFIERS = frozenset(
    {
        "_Bool",
        "auto",
        "bool",
        "char",
        "char8_t",
        "char16_t",
        "char32_t",
        "class",
        "double",
        "enum",
        "float",
        "int",
        "long",
        "short",
        "signed",
        "struct",
        "union",
        "unsigned",
        "void",
        "wchar_t",
    }
)
_ELABORATED_SPECIFIERS = frozenset({"class", "enum", "struct", "union"})
_PARAMETER_EXPRESSION_LEADS = frozenset({"!", "+", "-", "false", "nullptr", "true", "~"})
_MIN_USER_TYPE_DECL_TOKENS = 2
_PREFIX_GROUP_WORDS = {
    "_Alignas",
    "_Atomic",
    "alignas",
    "decltype",
    "typeof",
    "typeof_unqual",
    "__typeof__",
}
_ANNOTATION_WORDS = {
    "asm",
    "__asm",
    "__asm__",
    "__attribute",
    "__attribute__",
    "__declspec",
}


_SELFTEST_CASES = {
    "single line": ("RA8_INTERNAL static int helper(void);", True),
    "spliced keyword and name": ("sta\\\ntic int hel\\\nper(void);", True),
    "spliced block comment": (
        "/\\\n* static int hidden(void); */ int public_api(void);",
        False,
    ),
    "spliced define": (
        "#def\\\nine HIDDEN static int hidden(void)\nint public_api(void);",
        False,
    ),
    "return/name split": ("RA8_INTERNAL static int\nhelper(void);", True),
    "all specifiers split": ("RA8_INTERNAL\nstatic\nint helper(void);", True),
    "trailing annotation declaration": ("static int helper(void) RA8_UNUSED;", True),
    "exact RA8_UNUSED definition": (
        "RA8_INTERNAL static int helper(void) RA8_UNUSED\n{ return 0; }",
        True,
    ),
    "trailing annotation call": (
        "static int helper(void) RA8_ANNOTATE(owner);",
        True,
    ),
    "compiler attribute": (
        "static int helper(void) __attribute__((unused));",
        True,
    ),
    "parenthesized function name": ("static int (helper)(void);", True),
    "returns function pointer": ("static int (*helper(void))(int);", True),
    "mixed data then function": ("static int value, helper(void);", True),
    "mixed function then data": ("static int helper(void), value;", True),
    "pointer return": ("static int *helper(void);", True),
    "inline then noninline": (
        "static inline int accessor(void) { return 0; }\nstatic int helper(void);",
        True,
    ),
    "static inline": ("static inline int accessor(void) { return 0; }", False),
    "inline static": ("inline static int accessor(void) { return 0; }", False),
    "static data": ("static int value = 1;", False),
    "static function pointer data": ("static int (*handler)(void);", False),
    "mixed data only": ("static int first, *second;", False),
    "annotated data": ("static int value RA8_UNUSED;", False),
    "function-local static": (
        "int owner(void) { static int value = 1; return value; }",
        False,
    ),
    "block declaration": (
        "int owner(void) { int helper(void); static int value = 1; return value; }",
        False,
    ),
    "comment and literal": (
        '/* static int helper(void); */ const char *s = "static int helper(void);";',
        False,
    ),
    "macro directive": ("#define HELPER static int helper(void)\nint public_api(void);", False),
    "array initializer": ("static int values[] = {1, 2};", False),
    "aggregate callback": (
        "static struct record { int (*callback)(void); } value;",
        False,
    ),
}

_CPP_SELFTEST_CASES = {
    "extern C declaration": (
        '#ifdef __cplusplus\nextern "C" {\n#endif\n'
        "static int helper(void);\n"
        "#ifdef __cplusplus\n}\n#endif\n",
        True,
    ),
    "extern C definition": (
        '#ifdef __cplusplus\nextern "C" {\n#endif\n'
        "static int helper(void) { return 0; }\n"
        "#ifdef __cplusplus\n}\n#endif\n",
        True,
    ),
    "extern C nested scopes": (
        '#ifdef __cplusplus\nextern "C" {\n#endif\n'
        "struct Record { static int member(void); };\n"
        "union Variant { int value; static int member(void); };\n"
        "enum Kind { kind_one };\n"
        "class Holder { static int member(void); };\n"
        "int owner(void) { static int value = 1; return value; }\n"
        "#ifdef __cplusplus\n}\n#endif\n",
        False,
    ),
    "named namespace": (
        "namespace detail { static int helper(void); }",
        True,
    ),
    "inline namespace": (
        "inline namespace abi { static int helper(void) { return 0; } }",
        True,
    ),
    "anonymous namespace": (
        "namespace { static int helper(void); }",
        True,
    ),
    "nested namespace": (
        "namespace outer::inner { static int helper(void); }",
        True,
    ),
    "extern C++ wrapping extern C": (
        'extern "C++" { extern "C" { static int helper(void); } }',
        True,
    ),
    "direct initialized static object": (
        "struct Value { explicit Value(int); }; static Value value(7);",
        False,
    ),
    "identifier initialized static object": (
        "struct Value { explicit Value(int); }; int existing = 7; static Value value(existing);",
        False,
    ),
    "unary initialized static object": (
        "struct Value { explicit Value(int); }; int existing = 7; static Value value(-existing);",
        False,
    ),
    "call initialized static object": (
        "struct Value { explicit Value(int); }; int factory(); static Value value(factory());",
        False,
    ),
    "string initialized static object": (
        'struct Value { explicit Value(const char *); }; static Value value("text");',
        False,
    ),
    "character initialized static object": (
        "struct Value { explicit Value(char); }; static Value value('x');",
        False,
    ),
    "boolean initialized static object": (
        "struct Value { explicit Value(bool); }; static Value value(true);",
        False,
    ),
    "defaulted function parameter": (
        "static int helper(int value = 7);",
        True,
    ),
    "typedef named function parameter": (
        "using Count = int; static int helper(Count value);",
        True,
    ),
    "qualified unnamed function parameter": (
        "namespace model { struct Value {}; } static int helper(model::Value);",
        True,
    ),
    "namespace attribute": (
        'namespace detail [[deprecated("fixture")]] { static int helper(void); }',
        True,
    ),
    "nested inline namespace": (
        "namespace outer::inline abi { static int helper(void); }",
        True,
    ),
    "noexcept function": (
        "static int helper(void) noexcept;",
        True,
    ),
    "conditional noexcept function": (
        "static int helper(void) noexcept(true);",
        True,
    ),
    "trailing return function": (
        "static auto helper(void) noexcept -> int;",
        True,
    ),
    "templated trailing return": (
        "#include <array>\nstatic auto helper(void) -> std::array<int, 2>;",
        True,
    ),
    "nested templated trailing return": (
        "#include <vector>\n"
        "static auto helper(void) noexcept -> const std::vector<std::vector<int>>&;",
        True,
    ),
    "global qualified templated trailing return": (
        "#include <array>\nstatic auto helper(void) -> ::std::array<int, 2>;",
        True,
    ),
    "templated direct initialized object": (
        "#include <vector>\nint existing = 7; static std::vector<int> value(existing);",
        False,
    ),
    "gnu inline spelling": (
        "static __inline int helper(void) { return 0; }",
        False,
    ),
    "gnu inline suffix spelling": (
        "static __inline__ int helper(void) { return 0; }",
        False,
    ),
    "constexpr implicit inline": (
        "static constexpr int helper(void) { return 0; }",
        False,
    ),
    "consteval implicit inline": (
        "static consteval int helper(void) { return 0; }",
        False,
    ),
    "namespace nested scopes stay opaque": (
        "namespace detail {\n"
        "struct Record { static int member(void); };\n"
        "union Variant { int value; static int member(void); };\n"
        "enum Kind { kind_one };\n"
        "class Holder { static int member(void); };\n"
        "int owner(void) { static int value = 1; return value; }\n"
        "}",
        False,
    ),
}

_MALFORMED_CASES = {
    "unterminated declarator": ("static int broken(", "c"),
    "unterminated extern C wrapper": ('extern "C" { static int helper(void);', "c++"),
    "malformed extern C declaration": ('extern "C" { static int broken( }', "c++"),
    "unterminated namespace": ("namespace detail { static int helper(void);", "c++"),
    "unterminated trailing template": (
        "#include <vector>\nstatic auto helper(void) -> std::vector<int;",
        "c++",
    ),
}


def _strip_preprocessor_directives(text: str) -> str:
    """Replace directives with newlines while respecting continuations."""
    kept: list[str] = []
    continued = False
    for line in text.splitlines(keepends=True):
        is_directive = continued or line.lstrip().startswith("#")
        continued = is_directive and line.rstrip().endswith("\\")
        if is_directive:
            kept.append("\n" if line.endswith("\n") else "")
        else:
            kept.append(line)
    return "".join(kept)


def _splice_lines(text: str) -> str:
    """Apply C translation-phase backslash-newline deletion."""
    output: list[str] = []
    index = 0
    while index < len(text):
        if text.startswith("\\\r\n", index):
            index += 3
        elif text.startswith("\\\n", index):
            index += 2
        else:
            output.append(text[index])
            index += 1
    return "".join(output)


def _tokens(text: str) -> list[str]:
    """Return semantic identifier and punctuation tokens."""
    # Translation phase 2 precedes comment recognition and preprocessing.
    # Splicing later would misread a split comment opener or directive and
    # could invent declarations that the compiler never sees.
    source = _strip_preprocessor_directives(_splice_lines(text))
    # Preserve literals so the declarator parser can distinguish a C++ direct
    # initializer from an empty function parameter list.
    ignored = {"block_comment", "line_comment", "space"}
    return [
        match.group(0) for match in _TOKEN_RE.finditer(source) if match.lastgroup not in ignored
    ]


def _consume_group(tokens: list[str], start: int, end: int) -> int:
    """Return the token after one balanced (), [], or {} group."""
    opening = tokens[start]
    if opening not in _GROUP_OPEN:
        message = f"expected group opener, got {opening!r}"
        raise ScanError(message)
    stack = [_GROUP_CLOSE[opening]]
    for index in range(start + 1, end):
        lexeme = tokens[index]
        if lexeme in _GROUP_OPEN:
            stack.append(_GROUP_CLOSE[lexeme])
        elif lexeme in _GROUP_END and (not stack or lexeme != stack.pop()):
            message = f"mismatched group terminator {lexeme!r}"
            raise ScanError(message)
        if lexeme in _GROUP_END and not stack:
            return index + 1
    message = f"unterminated {opening!r} group"
    raise ScanError(message)


def _consume_double_bracket(tokens: list[str], start: int, end: int) -> int:
    """Return the token after a balanced C23/C++ [[attribute]]."""
    depth = 1
    index = start + 2
    while index < end:
        pair = tokens[index : index + 2]
        if pair == ["[", "["]:
            depth += 1
            index += 2
        elif pair == ["]", "]"]:
            depth -= 1
            index += 2
            if depth == 0:
                return index
        else:
            index += 1
    message = "unterminated '[[ attribute ]]' group"
    raise ScanError(message)


def _is_annotation_word(token: str) -> bool:
    """Recognize project/compiler annotation macro spellings."""
    return (
        token in _ANNOTATION_WORDS
        or token.startswith(("RA8_", "__"))
        or (token.isupper() and "_" in token)
    )


def _consume_annotation(tokens: list[str], start: int, end: int) -> int | None:
    """Consume one leading/trailing annotation, if present."""
    if tokens[start : start + 2] == ["[", "["]:
        return _consume_double_bracket(tokens, start, end)
    token = tokens[start]
    if not _IDENTIFIER_RE.fullmatch(token) or not _is_annotation_word(token):
        return None
    after = start + 1
    if after < end and tokens[after] == "(":
        return _consume_group(tokens, after, end)
    return after


def _prefix_is_specifiers(prefix: list[str], *, require_static: bool) -> bool:
    """Accept a declaration-specifier/attribute prefix, not declarator syntax."""
    if require_static and "static" not in prefix:
        return False
    index = 0
    user_type_names = 0
    while index < len(prefix):
        annotation_end = _consume_annotation(prefix, index, len(prefix))
        if annotation_end is not None:
            index = annotation_end
            continue
        lexeme = prefix[index]
        if not _IDENTIFIER_RE.fullmatch(lexeme):
            if lexeme == "{":
                index = _consume_group(prefix, index, len(prefix))
                continue
            return False
        if lexeme not in _DECL_SPECIFIER_WORDS and lexeme not in _PREFIX_GROUP_WORDS:
            user_type_names += 1
            if user_type_names > 1:
                return False
        if index + 1 < len(prefix) and prefix[index + 1] == "(":
            if lexeme not in _PREFIX_GROUP_WORDS:
                return False
            index = _consume_group(prefix, index + 1, len(prefix))
        else:
            index += 1
    return bool(prefix) or not require_static


def _parse_declarator(tokens: list[str], start: int, end: int) -> tuple[int, list[str]] | None:
    """Parse one C declarator and return its derived-type operators."""
    index = start
    pointers: list[str] = []
    while index < end and tokens[index] == "*":
        pointers.append("pointer")
        index += 1
        while index < end:
            annotation_end = _consume_annotation(tokens, index, end)
            if annotation_end is not None:
                index = annotation_end
            elif tokens[index] in _POINTER_QUALIFIERS:
                index += 1
            else:
                break
    if (
        index < end
        and _IDENTIFIER_RE.fullmatch(tokens[index])
        and tokens[index] not in _DECL_SPECIFIER_WORDS
    ):
        index += 1
        operators: list[str] = []
    elif index < end and tokens[index] == "(":
        after_group = _consume_group(tokens, index, end)
        inner = _parse_declarator(tokens, index + 1, after_group - 1)
        if inner is None or inner[0] != after_group - 1:
            return None
        index, operators = after_group, inner[1]
    else:
        return None
    while index < end and tokens[index] in {"(", "["}:
        opening = tokens[index]
        group_end = _consume_group(tokens, index, end)
        if opening == "(" and not _is_parameter_declaration_clause(
            tokens[index + 1 : group_end - 1]
        ):
            return None
        index = group_end
        operators.append("function" if opening == "(" else "array")
    operators.extend(pointers)
    return index, operators


def _function_suffix_only(tokens: list[str], start: int, end: int) -> bool:
    """Recognize conservative C++ function qualifiers and trailing returns."""
    index = start
    while index < end:
        annotation_end = _consume_annotation(tokens, index, end)
        if annotation_end is not None:
            index = annotation_end
            continue
        if tokens[index] in {"&", "const", "volatile"}:
            index += 1
            continue
        if tokens[index] == "noexcept":
            index += 1
            if index < end and tokens[index] == "(":
                index = _consume_group(tokens, index, end)
            continue
        if tokens[index : index + 2] == ["-", ">"]:
            trailing = tokens[index + 2 : end]
            return bool(trailing) and _is_parameter_declaration(trailing)
        return False
    return True


def _top_level_parts(tokens: list[str], separator: str) -> list[list[str]]:
    """Split at one separator while preserving balanced nested groups."""
    parts: list[list[str]] = [[]]
    stack: list[str] = []
    for index, lexeme in enumerate(tokens):
        template_prefix = index > 0 and (
            _IDENTIFIER_RE.fullmatch(tokens[index - 1]) is not None
            or tokens[index - 1] in {">", "]"}
        )
        if lexeme == "<" and template_prefix and _template_argument_end(tokens, index) is not None:
            stack.append(">")
        elif lexeme in _GROUP_OPEN:
            stack.append(_GROUP_CLOSE[lexeme])
        elif lexeme == ">" and stack[-1:] == [">"]:
            stack.pop()
        elif lexeme in _GROUP_END and (not stack or lexeme != stack.pop()):
            message = f"mismatched group terminator {lexeme!r}"
            raise ScanError(message)
        if lexeme == separator and not stack:
            parts.append([])
        else:
            parts[-1].append(lexeme)
    if stack:
        message = f"unterminated group, expected {stack[-1]!r}"
        raise ScanError(message)
    return parts


def _without_annotations(tokens: list[str]) -> list[str]:
    """Remove leading attributes while preserving the declaration body."""
    return tokens[_annotations_end(tokens, 0) :]


def _annotations_end(tokens: list[str], start: int) -> int:
    """Return the first token after a consecutive attribute sequence."""
    index = start
    while index < len(tokens):
        annotation_end = _consume_annotation(tokens, index, len(tokens))
        if annotation_end is None:
            break
        index = annotation_end
    return index


def _template_argument_end(tokens: list[str], start: int) -> int | None:
    """Return the end of a balanced C++ template-argument list, if any."""
    if tokens[start : start + 1] != ["<"]:
        return None
    depth = 1
    index = start + 1
    while index < len(tokens):
        lexeme = tokens[index]
        if lexeme in _GROUP_OPEN:
            index = _consume_group(tokens, index, len(tokens))
            continue
        if lexeme == "<":
            depth += 1
        elif lexeme == ">":
            depth -= 1
            if depth == 0:
                return index + 1
        index += 1
    return None


def _consume_template_arguments(tokens: list[str], start: int) -> int:
    """Return the token after one balanced C++ template-argument list."""
    end = _template_argument_end(tokens, start)
    if end is not None:
        return end
    message = "unterminated '<' template-argument group"
    raise ScanError(message)


def _has_qualified_type_prefix(tokens: list[str]) -> tuple[bool, int]:
    """Return a positive qualified type-name prefix and its end offset."""
    if not tokens:
        return False, 0
    index = 0
    qualified = tokens[:2] == [":", ":"]
    if qualified:
        index = 2
    if index >= len(tokens) or not _IDENTIFIER_RE.fullmatch(tokens[index]):
        return False, 0
    index += 1
    if tokens[index : index + 1] == ["<"]:
        index = _consume_template_arguments(tokens, index)
    while tokens[index : index + 2] == [":", ":"]:
        qualified = True
        index += 2
        if index < len(tokens) and tokens[index] == "template":
            index += 1
        if index >= len(tokens) or not _IDENTIFIER_RE.fullmatch(tokens[index]):
            return False, 0
        index += 1
        if tokens[index : index + 1] == ["<"]:
            index = _consume_template_arguments(tokens, index)
    return qualified, index


def _parameter_type_end(item: list[str]) -> int | None:
    """Return the end of a positive parameter type, or no type."""
    index = 0
    type_is_positive = False
    while index < len(item):
        annotation_end = _consume_annotation(item, index, len(item))
        if annotation_end is not None:
            index = annotation_end
            continue
        token = item[index]
        if token in _DECL_SPECIFIER_WORDS:
            type_is_positive = type_is_positive or token in _TYPE_BEARING_SPECIFIERS
            index += 1
            if token in _ELABORATED_SPECIFIERS:
                if index >= len(item) or not _IDENTIFIER_RE.fullmatch(item[index]):
                    return None
                index += 1
                type_is_positive = True
            continue
        if token in _PREFIX_GROUP_WORDS and item[index + 1 : index + 2] == ["("]:
            index = _consume_group(item, index + 1, len(item))
            type_is_positive = True
            continue
        break
    if type_is_positive:
        return index

    remainder = item[index:]
    qualified, qualified_end = _has_qualified_type_prefix(remainder)
    if qualified:
        return index + qualified_end
    user_type_has_declarator = len(remainder) >= _MIN_USER_TYPE_DECL_TOKENS and (
        remainder[1] in {"&", "*"} or _IDENTIFIER_RE.fullmatch(remainder[1])
    )
    qualified_by_specifier = index > 0 and len(remainder) == 1
    if (
        remainder
        and _IDENTIFIER_RE.fullmatch(remainder[0])
        and (user_type_has_declarator or qualified_by_specifier)
    ):
        return index + 1
    return None


def _parameter_declarator_is_valid(declarator: list[str]) -> bool:
    """Return whether tokens form a conservative parameter declarator."""
    if not declarator:
        return True
    if declarator[:1] in (["&"], ["*"]):
        return all(token in {"&", "*"} or _IDENTIFIER_RE.fullmatch(token) for token in declarator)
    if _IDENTIFIER_RE.fullmatch(declarator[0]):
        return all(
            _IDENTIFIER_RE.fullmatch(token) or token in {"&", "*", "[", "]", "(", ")"}
            for token in declarator
        )
    if declarator[0] == "(":
        try:
            return _consume_group(declarator, 0, len(declarator)) <= len(declarator)
        except ScanError:
            return False
    return False


def _is_parameter_declaration(tokens: list[str]) -> bool:
    """Recognize positive parameter-declaration grammar, never expressions."""
    item = _without_annotations(_before_initializer(tokens))
    if not item:
        return False
    if item == [".", ".", "."]:
        return True
    lead = item[0]
    if lead.startswith(('"', "'")) or lead[:1].isdigit() or lead in _PARAMETER_EXPRESSION_LEADS:
        return False
    type_end = _parameter_type_end(item)
    return type_end is not None and _parameter_declarator_is_valid(item[type_end:])


def _is_parameter_declaration_clause(tokens: list[str]) -> bool:
    """Require every top-level clause item to be a parameter declaration."""
    if not tokens:
        return True
    parts = _top_level_parts(tokens, ",")
    return bool(parts) and all(_is_parameter_declaration(part) for part in parts)


def _before_initializer(tokens: list[str]) -> list[str]:
    """Discard a top-level initializer from one init-declarator."""
    return _top_level_parts(tokens, "=")[0]


def _item_declares_function(tokens: list[str], *, require_static: bool) -> bool:
    """Recognize a function declarator in one comma-separated item."""
    item = _before_initializer(tokens)
    stack: list[str] = []
    for start in range(len(item)):
        at_top_level = not stack
        lexeme = item[start]
        if lexeme in _GROUP_OPEN:
            stack.append(_GROUP_CLOSE[lexeme])
        elif lexeme in _GROUP_END and (not stack or lexeme != stack.pop()):
            message = f"mismatched group terminator {lexeme!r}"
            raise ScanError(message)
        if not at_top_level or not _prefix_is_specifiers(
            item[:start], require_static=require_static
        ):
            continue
        parsed = _parse_declarator(item, start, len(item))
        if parsed is None:
            continue
        after, operators = parsed
        if (
            operators
            and operators[0] == "function"
            and _function_suffix_only(item, after, len(item))
        ):
            return True
    return False


def _statement_declares_function(
    tokens: list[str], *, require_static: bool, exclude_inline: bool
) -> bool:
    """Recognize a function in a complete file-scope declaration header."""
    if not tokens or (require_static and "static" not in tokens):
        return False
    if exclude_inline and _INLINE_SPECIFIERS.intersection(tokens):
        return False
    for index, item in enumerate(_top_level_parts(tokens, ",")):
        if _item_declares_function(item, require_static=(require_static and index == 0)):
            return True
    return False


def _validate_groups(tokens: list[str]) -> None:
    """Reject malformed input rather than silently routing it as direct."""
    stack: list[str] = []
    for lexeme in tokens:
        if lexeme in _GROUP_OPEN:
            stack.append(_GROUP_CLOSE[lexeme])
        elif lexeme in _GROUP_END and (not stack or lexeme != stack.pop()):
            message = f"mismatched group terminator {lexeme!r}"
            raise ScanError(message)
    if stack:
        message = f"unterminated group, expected {stack[-1]!r}"
        raise ScanError(message)


def _is_language_linkage_wrapper(tokens: list[str]) -> bool:
    """Recognize canonical C and C++ language-linkage block prefixes."""
    return tuple(tokens) in {("extern", '"C"'), ("extern", '"C++"')}


def _namespace_name_end(tokens: list[str], start: int) -> int | None:
    """Consume one optionally-inline namespace name and its attributes."""
    index = start
    if tokens[index : index + 1] == ["inline"]:
        index += 1
    if index >= len(tokens) or not _IDENTIFIER_RE.fullmatch(tokens[index]):
        return None
    return _annotations_end(tokens, index + 1)


def _is_namespace_wrapper(tokens: list[str]) -> bool:
    """Recognize named, anonymous, inline, and nested namespace prefixes."""
    index = 0
    if tokens[:1] == ["inline"]:
        index = 1
    if tokens[index : index + 1] != ["namespace"]:
        return False
    index = _annotations_end(tokens, index + 1)
    if index == len(tokens):
        return True
    while index < len(tokens):
        name_end = _namespace_name_end(tokens, index)
        if name_end is None:
            return False
        index = name_end
        if index == len(tokens):
            return True
        if tokens[index : index + 2] != [":", ":"]:
            return False
        index += 2
    return False


def _is_transparent_cpp_wrapper(tokens: list[str]) -> bool:
    """Return whether a C++ wrapper preserves namespace/file-scope linkage."""
    return _is_language_linkage_wrapper(tokens) or _is_namespace_wrapper(tokens)


def _scope_has_noninline_static(tokens: list[str], start: int, end: int) -> bool:
    """Scan one real or transparent file scope for a matching declaration."""
    statement: list[str] = []
    index = start
    while index < end:
        lexeme = tokens[index]
        if lexeme == "{":
            after_group = _consume_group(tokens, index, end)
            if _is_transparent_cpp_wrapper(statement):
                if _scope_has_noninline_static(tokens, index + 1, after_group - 1):
                    return True
                statement = []
                index = after_group
                continue
            if _statement_declares_function(statement, require_static=True, exclude_inline=True):
                return True
            if _statement_declares_function(statement, require_static=False, exclude_inline=False):
                statement = []
                index = after_group
                continue
            statement.extend(tokens[index:after_group])
            index = after_group
            continue
        if lexeme == ";":
            if _statement_declares_function(statement, require_static=True, exclude_inline=True):
                return True
            statement = []
        else:
            statement.append(lexeme)
        index += 1
    return False


def has_noninline_static_decl(text: str) -> bool:
    """Return whether text has a file-scope non-inline static function."""
    tokens = _tokens(text)
    _validate_groups(tokens)
    return _scope_has_noninline_static(tokens, 0, len(tokens))


def _clang_accepts(name: str, source: str, language: str) -> bool:
    """Prove the fixture is accepted C/C++, not parser-only invented syntax."""
    compiler_name = "clang++-18" if language == "c++" else "clang-18"
    clang = shutil.which(compiler_name)
    if clang is None:
        print(f"static_decl_scan.py selftest: {compiler_name} not found", file=sys.stderr)
        return False
    preamble = (
        "#define RA8_INTERNAL\n"
        "#define RA8_UNUSED __attribute__((unused))\n"
        "#define RA8_ANNOTATE(x) __attribute__((annotate(#x)))\n"
    )
    # clang is the resolved, fixed-name compiler; no source token reaches argv.
    result = subprocess.run(  # noqa: S603 -- executable is the fixed clang-18 probe
        [
            clang,
            "-std=c++20" if language == "c++" else "-std=c17",
            "-Wno-gcc-compat",
            "-fsyntax-only",
            "-x",
            language,
            "-",
        ],
        input=preamble + source,
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        print(
            f"static_decl_scan.py selftest: clang rejected {name}:\n{result.stderr}",
            file=sys.stderr,
        )
        return False
    return True


def _run_selftest_case(name: str, source: str, expected: bool, language: str) -> int:
    """Run one detection direction and its real-compiler syntax proof."""
    try:
        actual = has_noninline_static_decl(source)
    except ScanError as exc:
        print(f"static_decl_scan.py selftest: {name}: {exc}", file=sys.stderr)
        return 1
    mismatch = actual != expected
    if mismatch:
        print(
            f"static_decl_scan.py selftest: {name}: got {actual}, expected {expected}",
            file=sys.stderr,
        )
    return int(mismatch) + int(not _clang_accepts(name, source, language))


def _run_selftest() -> int:
    failures = sum(
        _run_selftest_case(name, source, expected, language)
        for language, cases in (("c", _SELFTEST_CASES), ("c++", _CPP_SELFTEST_CASES))
        for name, (source, expected) in cases.items()
    )
    for name, (source, _language) in _MALFORMED_CASES.items():
        try:
            has_noninline_static_decl(source)
        except ScanError:
            continue
        print(f"static_decl_scan.py selftest: {name} did not fail closed", file=sys.stderr)
        failures += 1
    if failures == 0:
        print(
            "static_decl_scan.py --selftest: "
            f"PASS ({len(_SELFTEST_CASES)} clang-18 + "
            f"{len(_CPP_SELFTEST_CASES)} clang++-18 cases)"
        )
    return failures


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true", help="run the token scanner selftest")
    parser.add_argument("path", nargs="?", type=Path, help="header to inspect")
    return parser.parse_args()


def main() -> int:
    """Run the file scanner or its selftest."""
    args = _parse_args()
    if args.selftest:
        return _run_selftest()
    if args.path is None:
        print("static_decl_scan.py: a path is required", file=sys.stderr)
        return 2
    try:
        text = args.path.read_text(encoding="utf-8")
        matched = has_noninline_static_decl(text)
    except (OSError, UnicodeError, ScanError) as exc:
        print(f"static_decl_scan.py: {exc}", file=sys.stderr)
        return 2
    return 0 if matched else 1


if __name__ == "__main__":
    raise SystemExit(main())
