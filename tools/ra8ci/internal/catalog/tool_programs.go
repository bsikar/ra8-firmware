// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import "fmt"

// The ra8ci: half of the dispatch seam, stated as a list rather than a shape.
//
// programs.go admits an ra8ci: program on its SHAPE: the tool half is a valid
// name and short enough. That is the right rule for the bash half, where the
// reviewed script is a path in the verified checkout and the checkout decides
// whether it exists. It is the wrong rule here, because an ra8ci: tool is not
// a file anyone can add: the executor dispatches a fixed set in process
// (executor.runStep) and nothing else in the tree can grow one.
//
// So a reviewed step naming a tool the executor does not implement was
// admitted by review and discovered only when the step ran. What the runner
// reports then is the worst available answer: the name has no slash, so
// resolveTaskProgram hands it to exec.LookPath, which fails, and the attempt
// comes back as ErrToolMissing, the error that means "this runner is missing a
// tool it was supposed to have". An operator reads that as a broken runner and
// goes looking at the image, when the truth is a typo in a reviewed manifest
// that every runner in the fleet would fail the same way.
//
// The list below is the executor's dispatch set, and holding the two together
// is what makes the refusal honest, so
// executor.TestEveryReviewedToolProgramIsDispatchedInProcess pins that every
// name here reaches an in-process gate rather than a program lookup. The
// catalog cannot import the executor (the executor imports the catalog), which
// is why the list lives at this end and the pin lives at that one.
//
// Adding a tool is two edits and the test says so: implement it in
// executor.runStep and name it here, in the same change that reviews the task
// using it.
var reviewedToolPrograms = []string{
	"ra8ci:ascii",
	"ra8ci:assert-casts",
	"ra8ci:driver-asm-guard",
	"ra8ci:final-newline",
	"ra8ci:gnu-attribute",
	"ra8ci:inclusive-terminology-commits",
	"ra8ci:legacy-make",
	"ra8ci:no-goto-setjmp",
	"ra8ci:no-null",
	"ra8ci:no-unsafe-python-install",
	"ra8ci:nsc-veneer-defs",
	"ra8ci:pointer-boilerplate",
	"ra8ci:runner-clock",
	"ra8ci:since",
	"ra8ci:stub-crypto-guard",
	"ra8ci:tests-readme",
	"ra8ci:tz-boundary-discard",
	"ra8ci:wave-references",
}

// ReviewedToolPrograms returns every ra8ci: program the executor implements.
func ReviewedToolPrograms() []string {
	return append([]string(nil), reviewedToolPrograms...)
}

// IsReviewedToolProgram reports whether a full ra8ci: program name is one the
// executor dispatches in process.
func IsReviewedToolProgram(program string) bool {
	for _, candidate := range reviewedToolPrograms {
		if program == candidate {
			return true
		}
	}
	return false
}

// checkToolProgramExists refuses a reviewed step naming an ra8ci: tool nothing
// implements. It is an admission rule, applied where a manifest is read rather
// than against a task already persisted under a reviewed digest, the same line
// ValidateTask draws against the dispatch seam: a task admitted while a tool
// existed keeps running even if the tool is later withdrawn, and the withdrawal
// is what has to re-review it.
func checkToolProgramExists(step Step, tool string) error {
	if IsReviewedToolProgram(step.Program) {
		return nil
	}
	return fmt.Errorf("%w: step %q names the %s tool %q, which no runner implements; the executor dispatches %d reviewed tools and a name outside them fails on every runner as a missing tool",
		ErrInvalidCatalog, step.Name, trimmedToolPrefix(), tool, len(reviewedToolPrograms))
}

func trimmedToolPrefix() string {
	return ToolProgramPrefix[:len(ToolProgramPrefix)-1]
}
