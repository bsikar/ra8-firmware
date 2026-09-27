// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"fmt"
	"sort"
	"strings"
)

// The mirror of a_file_argument_the_tool_reads.go, and the last argv shape the
// ra8ci: half of the dispatch seam leaves open.
//
// That door refuses a step handing a file to one of the eleven tools that read
// none. This one refuses the opposite mistake: a step handing NO scope at all
// to one of the four that cannot derive one. The two are not symmetric and
// neither implies the other, because an empty argv is a legitimate, intended
// shape for fourteen of the eighteen: driver-asm-guard, legacy-make,
// no-goto-setjmp, no-unsafe-python-install, nsc-veneer-defs,
// pointer-boilerplate, stub-crypto-guard and wave-references each derive their
// own scope from the checkout and REFUSE argv past --selftest;
// inclusive-terminology-commits reads its commit messages from stdin;
// final-newline, gnu-attribute and tz-boundary-discard treat an empty argv as
// the whole tree; runner-clock and tests-readme parse an empty argv into their
// documented defaults. For those fourteen an empty argv is the normal
// dispatch and a rule demanding a selector would refuse the manifest we
// actually ship.
//
// The four below say the opposite in their own words, and every one of them
// stops before reading anything:
//
//	ascii             parseOptions errors with "pass one target path,
//	                  optionally with --checkout, or pass --all" whenever
//	                  --all is absent and NArg() != 1. Exit 2.
//	assert-casts      len(args) == 0 prints "usage: ra8ci assert-casts <file>
//	                  [...] or ra8ci assert-casts --all". Exit 1, not 2, which
//	                  is worth knowing: an operator reads exit 1 as a gate
//	                  FINDING rather than a misuse, so this one lies about the
//	                  tree as well as failing.
//	no-null           len(args) == 0 prints the check_no_null.py usage. Exit 2.
//	since             neither the --all branch nor the path branch matches an
//	                  empty argv, so Run falls to the else and prints
//	                  "usage: ra8ci since FILE [FILE ...] | --all | --selftest".
//	                  Exit 2.
//
// So a reviewed step naming ra8ci:no-null with no arguments was admitted by
// review, digested into the catalog, dispatched to a guest, and answered with
// a usage line on every runner, forever. It is the same forever-failure the
// other argv doors close, reached by the one argv nothing else looks at: the
// empty one.
//
// WHAT COUNTS AS A SCOPE is read out of those same four parsers rather than
// invented here. Each accepts exactly three answers to "what should I look
// at": the self test (prove the detector, read no tree), the whole tree
// (--all), or one or more paths. ascii's --checkout is not a fourth: it says
// where to resolve the target, and parseOptions still requires the target
// beside it, so a step passing --checkout alone is refused here and by the
// tool for the same reason. A step passing only ascii's --check is refused for
// the same reason again: --check chooses report-instead-of-rewrite, which is
// an answer to what to DO, not to what to look at.
//
// THE SET IS RESTATED, NOT IMPORTED, for the reason the tool names, their
// flags and their file-reading all are: the executor imports the catalog, so
// the catalog cannot ask the executor what its tools require.
// TestEveryReviewedToolSaysWhetherItRequiresAScopeSelector holds the set to
// the dispatch list, so a tool cannot be added in one place and forgotten
// here.
var toolsRequiringAScopeSelector = map[string]bool{
	"ra8ci:ascii":        true, // parseOptions: no --all and NArg() != 1 is an error
	"ra8ci:assert-casts": true, // len(args) == 0 prints usage and returns 1
	"ra8ci:no-null":      true, // len(args) == 0 prints usage and returns 2
	"ra8ci:since":        true, // empty argv falls to the usage branch
}

// ToolRequiresAScopeSelector reports whether a reviewed ra8ci: tool refuses an
// empty argv because it cannot derive its own scope.
func ToolRequiresAScopeSelector(program string) bool {
	return toolsRequiringAScopeSelector[program]
}

// checkAScopeSelectorIsNamedWhereTheToolRequiresOne refuses a reviewed task
// whose step gives a scope-taking tool nothing to look at. It is an admission
// rule, applied where a manifest is read rather than against a task already
// persisted under a reviewed digest, the same line the other doors on this
// seam draw.
//
// It is the one door on this seam that must read the TASK rather than the
// step, and the embedded catalog is why. ascii-rewrite dispatches ra8ci:ascii
// with the reviewed arguments --checkout and nothing else, and its target is
// the task's declared positional, which BindArguments appends at dispatch
// time. Read as a step in isolation that argv names no scope; read as the task
// it belongs to, the scope is the argument the caller must supply. So a task
// declaring at least one positional satisfies this rule for every step in it,
// because StepArgv appends the bound values to each. A declared FLAG does not:
// it arrives as --name=value, which is neither a path nor a scope switch any
// of these four parses.
func checkAScopeSelectorIsNamedWhereTheToolRequiresOne(task Task) error {
	if len(task.ArgsSchema.Positional) > 0 {
		return nil
	}
	for _, step := range task.Steps {
		program := step.Program
		if !toolsRequiringAScopeSelector[program] {
			continue
		}
		if namesAScope(step.Args) {
			continue
		}
		return fmt.Errorf("%w: task %q step %q gives %q no scope; it cannot derive one and answers an argv naming neither --%s, --%s nor a path with a usage error, so name a path, --%s, --%s, or a declared positional argument (the %d other tools derive their own scope)",
			ErrInvalidCatalog, task.Name, step.Name, program, allFlag, selftestFlag, allFlag, selftestFlag,
			len(reviewedToolPrograms)-len(toolsRequiringAScopeSelector))
	}
	return nil
}

// namesAScope reports whether an argv answers "what should I look at" in one
// of the three ways every scope-taking tool accepts: the self test, the whole
// tree, or at least one path. A bare word is read as a path the same way
// checkFileArgumentsAreOnesTheToolReads reads one, so an option's separate
// value cannot pass for a target; none of the four tools here takes such an
// option today, and sharing the reading is what keeps that true if one gains
// a flag later.
func namesAScope(args []string) bool {
	for _, arg := range args {
		if !strings.HasPrefix(arg, "-") {
			return true
		}
		name := flagNameArgvClaims(arg)
		if name == allFlag || name == selftestFlag {
			return true
		}
	}
	return false
}

// toolsDerivingTheirOwnScope names the tools an empty argv is a real dispatch
// for, so a reader can see the refusal is about four tools rather than about
// empty argv in general.
func toolsDerivingTheirOwnScope() []string {
	named := make([]string, 0, len(reviewedToolPrograms))
	for _, program := range reviewedToolPrograms {
		if !toolsRequiringAScopeSelector[program] {
			named = append(named, program)
		}
	}
	sort.Strings(named)
	return named
}
