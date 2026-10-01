// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"bytes"
	"context"
	"errors"
	"io"
	"os"
	"path/filepath"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

// verifiedCheckout answers this repository's own root, which is the only
// checkout a reviewed task will run out of: VerifyCheckout demands a .git and
// a catalog whose digest matches the one embedded in this binary, so a
// fixture tree cannot stand in for it without restating the manifest.
func verifiedCheckout(t *testing.T) string {
	t.Helper()
	root := filepath.Join("..", "..", "..", "..")
	verified, err := catalog.VerifyCheckout(root)
	if err != nil {
		t.Skipf("this build is not running out of a verified checkout: %v", err)
	}
	return verified
}

// aReviewedSafeLocalTask answers the first catalog task the executor will
// admit, so these doors are judged against a real reviewed definition rather
// than one written here. A task the catalog stopped shipping cannot silently
// turn these tests into no-ops: an empty catalog skips loudly.
func aReviewedSafeLocalTask(t *testing.T) catalog.Task {
	t.Helper()
	definitions, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	for _, name := range definitions.Names() {
		task, found := definitions.Task(name)
		if found && task.IsSafeLocal() {
			return task
		}
	}
	t.Skip("the catalog declares no safe-local task")
	return catalog.Task{}
}

// The first door is the checkout, not the arguments. A caller who supplies
// values against an unverified tree is told the tree is wrong, because a
// complaint about the values would send them to fix the wrong thing.
func TestRunWithArgumentsRefusesAnUnverifiedCheckout(t *testing.T) {
	for name, root := range map[string]string{
		"empty":       "",
		"no checkout": t.TempDir(),
	} {
		result, err := RunWithArguments(context.Background(), root, aReviewedSafeLocalTask(t),
			map[string]string{"gate": "lint-go"}, io.Discard, io.Discard)
		if !errors.Is(err, catalog.ErrInvalidCheckout) {
			t.Fatalf("%s: err = %v, want ErrInvalidCheckout", name, err)
		}
		if result.TaskName != "" || len(result.Steps) != 0 {
			t.Fatalf("%s: refused run still reported an attempt: %+v", name, result)
		}
	}
}

// A task is admitted on being the reviewed definition itself, not on carrying
// a reviewed name. Every mutation below leaves the name alone.
func TestRunWithArgumentsRefusesATaskTheCatalogNeverReviewed(t *testing.T) {
	root := verifiedCheckout(t)
	reviewed := aReviewedSafeLocalTask(t)

	unnamed := reviewed
	unnamed.Name = "no-such-task"

	reversioned := reviewed
	reversioned.Version = reviewed.Version + 1

	restretched := reviewed
	restretched.DeadlineSeconds = reviewed.DeadlineSeconds + 1

	unscoped := reviewed
	unscoped.Scope = "lab"

	for name, task := range map[string]catalog.Task{
		"absent":           unnamed,
		"other version":    reversioned,
		"other deadline":   restretched,
		"outside the safe": unscoped,
		"zero value":       {},
	} {
		result, err := RunWithArguments(context.Background(), root, task, nil, io.Discard, io.Discard)
		if !errors.Is(err, ErrUnreviewedTask) {
			t.Fatalf("%s: err = %v, want ErrUnreviewedTask", name, err)
		}
		if result.TaskName != "" || len(result.Steps) != 0 {
			t.Fatalf("%s: refused run still reported an attempt: %+v", name, result)
		}
	}
}

// Every task in the v1 catalog declares no arguments, so a caller supplying
// values is refused whole. The refusal must come from the binding rather than
// from the admission check, or a reviewed task would look unreviewed the
// moment somebody passed it a value.
func TestRunWithArgumentsRefusesValuesTheReviewedTaskNeverDeclared(t *testing.T) {
	root := verifiedCheckout(t)
	reviewed := aReviewedSafeLocalTask(t)
	if _, err := reviewed.BindArguments(map[string]string{"gate": "lint-go"}); err == nil {
		t.Skipf("task %q now declares arguments; this door moved", reviewed.Name)
	}
	result, err := RunWithArguments(context.Background(), root, reviewed,
		map[string]string{"gate": "lint-go"}, io.Discard, io.Discard)
	if err == nil {
		t.Fatal("undeclared values were accepted")
	}
	if errors.Is(err, ErrUnreviewedTask) {
		t.Fatalf("undeclared values were reported as an unreviewed task: %v", err)
	}
	if result.TaskName != "" || len(result.Steps) != 0 {
		t.Fatalf("refused run still reported an attempt: %+v", result)
	}
}

// The step-writer selector is optional, but at most one, and never nil. Both
// refusals land before a process is started, which is what these assertions
// are really for: a run refused here must not have spent an attempt.
func TestRunWithArgumentsRefusesAnUnusableStepWriterSelector(t *testing.T) {
	root := verifiedCheckout(t)
	reviewed := aReviewedSafeLocalTask(t)
	writers := func(string) (io.Writer, io.Writer) { return io.Discard, io.Discard }

	var nilSelector func(string) (io.Writer, io.Writer)
	for name, selectors := range map[string][]func(string) (io.Writer, io.Writer){
		"two selectors":   {writers, writers},
		"three selectors": {writers, writers, writers},
		"nil selector":    {nilSelector},
	} {
		var stdout, stderr bytes.Buffer
		result, err := RunWithArguments(context.Background(), root, reviewed, nil, &stdout, &stderr, selectors...)
		if err == nil {
			t.Fatalf("%s: an unusable selector was accepted", name)
		}
		if result.TaskName != "" || len(result.Steps) != 0 {
			t.Fatalf("%s: refused run still reported an attempt: %+v", name, result)
		}
		if stdout.Len() != 0 || stderr.Len() != 0 {
			t.Fatalf("%s: refused run still wrote output: %q / %q", name, stdout.String(), stderr.String())
		}
	}
}

// Run is RunWithArguments with no values, so the two entry points must refuse
// the same thing in the same words. If they ever drift, one of them has grown
// a door the other does not have.
func TestRunAndRunWithArgumentsRefuseAlike(t *testing.T) {
	root := t.TempDir()
	task := aReviewedSafeLocalTask(t)

	plainResult, plainErr := Run(context.Background(), root, task, io.Discard, io.Discard)
	boundResult, boundErr := RunWithArguments(context.Background(), root, task, nil, io.Discard, io.Discard)

	if (plainErr == nil) != (boundErr == nil) {
		t.Fatalf("Run err = %v, RunWithArguments err = %v", plainErr, boundErr)
	}
	if plainErr != nil && plainErr.Error() != boundErr.Error() {
		t.Fatalf("Run said %q, RunWithArguments said %q", plainErr, boundErr)
	}
	if plainResult.TaskName != boundResult.TaskName || plainResult.ExitCode != boundResult.ExitCode ||
		len(plainResult.Steps) != len(boundResult.Steps) {
		t.Fatalf("Run = %+v, RunWithArguments = %+v", plainResult, boundResult)
	}
}

// resolveTaskProgram resolves a relative program out of the verified
// checkout, never out of the caller's working directory, and a symlink may
// not carry it back out again.
func TestATaskProgramIsResolvedInsideTheCheckout(t *testing.T) {
	root := t.TempDir()
	if err := os.MkdirAll(filepath.Join(root, "scripts"), 0o700); err != nil {
		t.Fatal(err)
	}
	inside := filepath.Join(root, "scripts", "gate.sh")
	if err := os.WriteFile(inside, []byte("#!/bin/sh\n"), 0o700); err != nil {
		t.Fatal(err)
	}
	resolved, err := resolveTaskProgram(root, "scripts/gate.sh")
	if err != nil {
		t.Fatal(err)
	}
	realRoot, err := filepath.EvalSymlinks(root)
	if err != nil {
		t.Fatal(err)
	}
	if resolved != filepath.Join(realRoot, "scripts", "gate.sh") {
		t.Fatalf("resolved = %q, want the program inside the checkout", resolved)
	}

	for name, program := range map[string]string{
		"empty":        "",
		"parent":       "..",
		"up and out":   filepath.Join("..", "gate.sh"),
		"absent":       "scripts/absent.sh",
		"dotted climb": "scripts/../../gate.sh",
	} {
		if _, err := resolveTaskProgram(root, program); err == nil {
			t.Fatalf("%s: %q was resolved", name, program)
		}
	}
}

// A symlink is followed, so the target decides. One pointing out of the
// checkout is refused even though its own path sits inside.
func TestATaskProgramSymlinkMayNotLeaveTheCheckout(t *testing.T) {
	outside := t.TempDir()
	escape := filepath.Join(outside, "escape.sh")
	if err := os.WriteFile(escape, []byte("#!/bin/sh\n"), 0o700); err != nil {
		t.Fatal(err)
	}
	root := t.TempDir()
	if err := os.MkdirAll(filepath.Join(root, "scripts"), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(escape, filepath.Join(root, "scripts", "gate.sh")); err != nil {
		t.Skipf("this filesystem will not hold a symlink: %v", err)
	}
	if _, err := resolveTaskProgram(root, "scripts/gate.sh"); !errors.Is(err, ErrUnsafeEnvironment) {
		t.Fatalf("err = %v, want ErrUnsafeEnvironment", err)
	}
}

// isWithin answers on the RESOLVED paths, so a root that cannot be resolved
// is an error rather than a false "outside", and a path that does not exist
// yet is still judged by where it would land.
func TestIsWithinJudgesResolvedPaths(t *testing.T) {
	root := t.TempDir()
	if err := os.MkdirAll(filepath.Join(root, "build", "artifacts"), 0o700); err != nil {
		t.Fatal(err)
	}
	for name, candidate := range map[string]string{
		"the root itself": root,
		"a child":         filepath.Join(root, "build"),
		"a grandchild":    filepath.Join(root, "build", "artifacts"),
		"not yet written": filepath.Join(root, "build", "artifacts", "report.json"),
		"deep and absent": filepath.Join(root, "a", "b", "c"),
	} {
		within, err := isWithin(root, candidate)
		if err != nil || !within {
			t.Fatalf("%s: within = %v, err = %v", name, within, err)
		}
	}

	parent := filepath.Dir(root)
	for name, candidate := range map[string]string{
		"the parent":  parent,
		"a sibling":   filepath.Join(parent, filepath.Base(root)+"-other"),
		"the climb":   filepath.Join(root, "..", ".."),
		"a neighbour": t.TempDir(),
	} {
		within, err := isWithin(root, candidate)
		if err != nil {
			t.Fatalf("%s: err = %v", name, err)
		}
		if within {
			t.Fatalf("%s: %q was judged inside %q", name, candidate, root)
		}
	}

	if _, err := isWithin(filepath.Join(root, "absent"), root); !errors.Is(err, ErrUnsafeEnvironment) {
		t.Fatalf("an unresolvable root: err = %v, want ErrUnsafeEnvironment", err)
	}
}
