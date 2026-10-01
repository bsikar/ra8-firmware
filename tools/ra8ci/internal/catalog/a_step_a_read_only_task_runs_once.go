// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"fmt"
	"strings"
)

// checkNoStepRepeatsAnotherStepsCommand refuses a reviewed READ-ONLY task
// whose steps dispatch the same command twice.
//
// ValidateTask already refuses two steps sharing a NAME, and every dispatch
// door in this package judges one step's argv against the contract of the
// program it names. None of them compares one step to another, so two steps
// differing only in their names and dispatching a byte-identical command are
// admitted by all of them: each is, on its own terms, exactly right.
//
// Nothing fails when they run. v1 fixes Retry.MaxAttempts at 1, so the repeat
// is not a retry; it is the same command asked the same question twice inside
// the one deadline the whole task shares. The task pays for the work twice,
// history files two verdicts for one question, and a reader of the evidence
// cannot tell which of the two names answered it. That is the whole defect: it
// is invisible from either step and green when it runs.
//
// SCOPE IS THE WHOLE RULE. A repeat is only provably redundant where nothing
// between the two runs can change what the command reads, and the catalog
// states that directly: a safe-local-read-only task may not write the working
// tree, so the second run sees the tree the first one saw. Every other scope
// may write, and there the repeat is a real shape rather than a mistake: check,
// rewrite, check again is how a task proves its own rewrite settled, and the
// second check is the point of it. So this door judges read-only tasks and
// leaves the writing scopes to say what they mean.
//
// Steps sharing a PROGRAM are admitted everywhere; that is how one gate runs
// over several targets or modes. Only an identical argv is refused, compared
// exactly and in order, the way the executor hands it on: two steps naming the
// same options in a different order dispatch different command lines, and the
// parser doors already judge order where it changes what is read.
//
// This is an admission rule, applied where a manifest is read, not a re-check
// of a task already persisted against a reviewed digest: a definition admitted
// under an older rule keeps running, the same line ValidateTask draws against
// the dispatch seam.
func checkNoStepRepeatsAnotherStepsCommand(task Task) error {
	if task.Scope != "safe-local-read-only" {
		return nil
	}
	type command struct {
		program string
		argv    string
	}
	first := make(map[command]string, len(task.Steps))
	for _, step := range task.Steps {
		// A NUL cannot appear in an admitted program or argument
		// (ValidateTask refuses it in both), so joining on it cannot
		// make two different argv slices compare equal.
		key := command{program: step.Program, argv: strings.Join(step.Args, "\x00")}
		earlier, repeated := first[key]
		if !repeated {
			first[key] = step.Name
			continue
		}
		return fmt.Errorf("%w: read-only task %q runs the same command in steps %q and %q (%s); the task may not write the working tree, so the second reads what the first read and files a second verdict for one question",
			ErrInvalidCatalog, task.Name, earlier, step.Name, commandAsWritten(step))
	}
	return nil
}

// commandAsWritten spells a step's dispatch the way its definition states it,
// so the refusal names the command and not only the two steps.
func commandAsWritten(step Step) string {
	if len(step.Args) == 0 {
		return step.Program
	}
	return step.Program + " " + strings.Join(step.Args, " ")
}
