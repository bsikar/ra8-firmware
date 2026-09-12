# RA8 Firmware: Developer Guide

Welcome to the RA8 Firmware repository. Because this is a safety-critical project targeting DO-178C / MISRA compliance, you will notice that our CI gates are exceptionally strict. 

We use a "fail-closed" cryptographic ratchet architecture. This means **you cannot quietly ignore a warning, leave dead code, or bypass a linter.** However, you also shouldn't have to fight the tools. We have built automation to handle the paperwork for you.

---

## 1. The Golden Rule: `just quality::local::auto-sync`

If you modify a Python checking script, add a suppression comment to C code (e.g., `/* alloc-allow: ... */`), or CI fails with a `ledger-missing-site` error: **do not edit the `.github/` ledgers by hand.**

Instead, run:
```bash
just quality::local::auto-sync
```
This single command automatically updates all cryptographic hashes, runs the tree formatters, and appends any missing suppressions to the `.github/` ledger as drafts. 

## 2. Approving Suppressions

If CI fails with `ledger-unreviewed`, it means you have draft suppressions in the ledger that need a formal justification. 

Run:
```bash
just quality::local::review-suppressions
```
This interactive script will ask you for a rationale (e.g., `tool-false-positive`), generate a signed cryptographic review batch, and mark your suppressions as approved so CI can pass.

## 3. Unused Includes

Unlike standard AST linters that throw thousands of false positives on embedded umbrella headers, we use **speculative compilation** to enforce zero dead includes in C code. 

If you want to check if your headers are clean before pushing, run:
```bash
just quality::local::unused-includes
```
If this tool flags a header, it is mathematically proven to be dead weight. Delete it.

## 4. Key Coding Standards

To save you from CI failures, keep these hard limits in mind:
* **NASA Power of 10 (Rule 4):** Functions must not exceed **60 lines**. Files must not exceed **1000 lines**. 
* **No Magic Numbers:** Magic numbers are banned in C code. Use `typedef enum` for integers. If a magic number is mathematically required, append `/* MAGIC-OK: <reason> */`.
* **Doxygen Contracts:** Every function, struct, enum, and macro must be fully documented (`@brief`, `@details`, `@param`, `@return`, `@retval`, `@pre`, `@post`).
* **Compiler Attributes:** Never use raw `__attribute__((...))`. Use the cross-platform `RA8_*` macros defined in `ra8_attributes.h`.

For the complete source of truth on styles, see [`docs/STYLE_GUIDE.md`](docs/STYLE_GUIDE.md).

## 5. Running CI Locally

You can run the exact same checks that GitHub Actions runs, directly on your machine.
```bash
# Run the entire CI suite
just ci

# Run just the pre-commit fast gates
just quality::local::gate pre-commit-checks
```