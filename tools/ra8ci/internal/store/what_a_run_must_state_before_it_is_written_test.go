// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"encoding/json"
	"errors"
	"strings"
	"testing"
)

// What a run must state before a transaction opens.
//
// validateRun refuses in four places and each refusal is a single boolean OR
// of a dozen clauses, so a passing test proves almost nothing about which
// clause did the work. The DAG half is already pinned in store_test.go; this
// takes the three bound-and-shape refusals ahead of it, one clause at a time,
// each over a run that is otherwise entirely valid. A clause that has never
// been exercised alone is a clause that could be deleted without any test
// noticing.
//
// The bounds themselves are the point rather than the wording. Every one of
// these strings is interpolated into a row that Postgres will judge on its own
// terms, so a value past its bound is refused here or refused by the database
// mid-transaction, and the second is a 500 an operator cannot act on.

// TestARunStatesItsOwnMetadataBeforeAnythingIsWritten walks the metadata guard
// clause by clause. Each case changes exactly one field of an otherwise valid
// run, so the refusal can only have come from that clause.
func TestARunStatesItsOwnMetadataBeforeAnythingIsWritten(t *testing.T) {
	for _, c := range []struct {
		name  string
		spoil func(*CreateRunInput)
	}{
		{"no trigger", func(in *CreateRunInput) { in.Trigger = "" }},
		{"a trigger past its bound", func(in *CreateRunInput) { in.Trigger = strings.Repeat("t", 65) }},
		{"no actor", func(in *CreateRunInput) { in.ActorID = "" }},
		{"an actor past its bound", func(in *CreateRunInput) { in.ActorID = strings.Repeat("a", 257) }},
		{"no repository", func(in *CreateRunInput) { in.Repository = "" }},
		{"a repository past its bound", func(in *CreateRunInput) { in.Repository = strings.Repeat("r", 513) }},
		{"a branch past its bound", func(in *CreateRunInput) { in.Branch = strings.Repeat("b", 513) }},
		{"a commit that is not a full SHA", func(in *CreateRunInput) { in.CommitSHA = strings.Repeat("a", 39) }},
		{"a commit in upper case", func(in *CreateRunInput) { in.CommitSHA = strings.Repeat("A", 40) }},
		{"a snapshot digest of the wrong width", func(in *CreateRunInput) { in.SnapshotSHA256 = strings.Repeat("b", 63) }},
		{"a catalog digest that is not hex", func(in *CreateRunInput) { in.CatalogSHA256 = strings.Repeat("g", 64) }},
		{"a parent run that is not an identifier", func(in *CreateRunInput) { in.ParentRunID = "not-a-uuid" }},
	} {
		t.Run(c.name, func(t *testing.T) {
			in := testRun()
			c.spoil(&in)
			err := validateRun(in)
			if !errors.Is(err, ErrInvalid) {
				t.Fatalf("err %v, want ErrInvalid", err)
			}
			if !strings.Contains(err.Error(), "run metadata") {
				t.Fatalf("refusal %q does not name the metadata", err)
			}
		})
	}

	// An empty branch is allowed, unlike the other three: a run can be
	// triggered against a detached commit with no branch to name.
	in := testRun()
	in.Branch = ""
	if err := validateRun(in); err != nil {
		t.Fatalf("refused a run with no branch: %v", err)
	}
	// And a parent run is optional, but must be an identifier when stated.
	in = testRun()
	in.ParentRunID = ""
	if err := validateRun(in); err != nil {
		t.Fatalf("refused a run with no parent: %v", err)
	}
}

// TestTheIdempotencyFieldsAreStatedTogetherOrNotAtAll pins the pairing. A key
// with no digest cannot be replayed safely, and a digest with no key is never
// read, so the two are refused apart in both directions. The key-without-digest
// direction is already pinned in store_test.go; the rest is here.
func TestTheIdempotencyFieldsAreStatedTogetherOrNotAtAll(t *testing.T) {
	for _, c := range []struct {
		name  string
		spoil func(*CreateRunInput)
	}{
		{"a digest with no key", func(in *CreateRunInput) { in.RequestSHA256 = strings.Repeat("d", 64) }},
		{"a key past its bound", func(in *CreateRunInput) {
			in.IdempotencyKey, in.RequestSHA256 = strings.Repeat("k", 257), strings.Repeat("d", 64)
		}},
		{"a key whose digest is not a digest", func(in *CreateRunInput) {
			in.IdempotencyKey, in.RequestSHA256 = "once", "not-a-digest"
		}},
	} {
		t.Run(c.name, func(t *testing.T) {
			in := testRun()
			c.spoil(&in)
			err := validateRun(in)
			if !errors.Is(err, ErrInvalid) {
				t.Fatalf("err %v, want ErrInvalid", err)
			}
			if !strings.Contains(err.Error(), "idempotency") {
				t.Fatalf("refusal %q does not name the idempotency fields", err)
			}
		})
	}

	// Both stated, both well formed, and the run is taken.
	in := testRun()
	in.IdempotencyKey, in.RequestSHA256 = "once", strings.Repeat("d", 64)
	if err := validateRun(in); err != nil {
		t.Fatalf("refused a correctly keyed run: %v", err)
	}
}

// TestARunCarriesBetweenOneAndAHundredTasks pins the count bound on both sides
// and at both edges. A run with no tasks would sit queued forever with nothing
// to dispatch, and the ceiling is what keeps one request from filling the
// dispatch table.
func TestARunCarriesBetweenOneAndAHundredTasks(t *testing.T) {
	in := testRun()
	in.Tasks = nil
	err := validateRun(in)
	if !errors.Is(err, ErrInvalid) || !strings.Contains(err.Error(), "1..100") {
		t.Fatalf("err %v, want a refusal naming the bound", err)
	}

	// Exactly at the ceiling is taken; one past it is refused. The keys are
	// distinct and nothing depends on anything, so only the count can decide.
	atCeiling := testRun()
	atCeiling.Tasks = manyTasks(100)
	if err := validateRun(atCeiling); err != nil {
		t.Fatalf("refused a run of exactly 100 tasks: %v", err)
	}
	over := testRun()
	over.Tasks = manyTasks(101)
	err = validateRun(over)
	if !errors.Is(err, ErrInvalid) || !strings.Contains(err.Error(), "1..100") {
		t.Fatalf("err %v, want a refusal naming the bound", err)
	}
}

// TestEachTaskIsJudgedOnItsOwnTerms walks the per-task guard clause by clause,
// the same way as the metadata guard. The refusal names the offending key,
// which is the only thing that makes a hundred-task run diagnosable, so every
// case asserts that too.
func TestEachTaskIsJudgedOnItsOwnTerms(t *testing.T) {
	for _, c := range []struct {
		name  string
		spoil func(*TaskInput)
	}{
		{"no key", func(task *TaskInput) { task.Key = "" }},
		{"a key past its bound", func(task *TaskInput) { task.Key = strings.Repeat("k", 129) }},
		{"a key with room around it", func(task *TaskInput) { task.Key = " format " }},
		{"no name", func(task *TaskInput) { task.Name = "" }},
		{"a name past its bound", func(task *TaskInput) { task.Name = strings.Repeat("n", 129) }},
		{"a name with room around it", func(task *TaskInput) { task.Name = "format-check\n" }},
		{"no tier", func(task *TaskInput) { task.Tier = "" }},
		{"a tier nobody declared", func(task *TaskInput) { task.Tier = "blocking" }},
		{"a scope nobody declared", func(task *TaskInput) { task.Scope = "root-shell" }},
		{"no deadline", func(task *TaskInput) { task.DeadlineSeconds = 0 }},
		{"a negative deadline", func(task *TaskInput) { task.DeadlineSeconds = -1 }},
		{"a deadline past a day", func(task *TaskInput) { task.DeadlineSeconds = 86401 }},
		{"a host class past its bound", func(task *TaskInput) { task.HostClass = strings.Repeat("h", 129) }},
		{"more dependencies than a run may hold tasks", func(task *TaskInput) {
			task.DependsOnKeys = make([]string, 101)
		}},
	} {
		t.Run(c.name, func(t *testing.T) {
			in := testRun()
			// The second task is spoiled, so the first has already been
			// accepted by the time this one is judged.
			c.spoil(&in.Tasks[1])
			err := validateRun(in)
			if !errors.Is(err, ErrInvalid) {
				t.Fatalf("err %v, want ErrInvalid", err)
			}
			if !strings.Contains(err.Error(), "invalid task") {
				t.Fatalf("refusal %q does not say a task was at fault", err)
			}
		})
	}

	// The three tiers and the six scopes are all taken, or the guard would be
	// refusing work the catalog is allowed to declare.
	for _, tier := range []string{"required", "optional", "nightly"} {
		in := testRun()
		in.Tasks[1].Tier = tier
		if err := validateRun(in); err != nil {
			t.Fatalf("refused tier %q: %v", tier, err)
		}
	}

	// A deadline of exactly a day is the ceiling, not past it.
	in := testRun()
	in.Tasks[1].DeadlineSeconds = 86400
	if err := validateRun(in); err != nil {
		t.Fatalf("refused a deadline of exactly a day: %v", err)
	}
}

// TestTwoTasksCannotShareAKey pins the duplicate refusal, which is separate
// from the per-task guard because both tasks are individually valid. The key is
// what dependencies are stated against and what the dispatch row is addressed
// by, so a run carrying it twice would have an unanswerable dependency graph.
func TestTwoTasksCannotShareAKey(t *testing.T) {
	in := testRun()
	in.Tasks[1].Key = in.Tasks[0].Key
	in.Tasks[1].DependsOnKeys = nil
	err := validateRun(in)
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("err %v, want ErrInvalid", err)
	}
	if !strings.Contains(err.Error(), "duplicate task key") || !strings.Contains(err.Error(), in.Tasks[0].Key) {
		t.Fatalf("refusal %q does not name the repeated key", err)
	}
}

// TestOnlyTheDeclaredScopesAreAccepted pins the scope table in both
// directions. A scope decides which executor a task is handed to and what it
// is allowed to touch, so a name that falls through to a default would be
// deciding that by accident.
func TestOnlyTheDeclaredScopesAreAccepted(t *testing.T) {
	for _, scope := range []string{
		"safe-local-read-only", "safe-local-write-working-tree",
		"runner", "linux-vm", "windows-vm", "hil",
	} {
		if !validScope(scope) {
			t.Fatalf("refused the declared scope %q", scope)
		}
	}
	for _, scope := range []string{
		"", "hil ", "HIL", "safe-local", "safe-local-read-write",
		"macos-vm", "root-shell", "safe-local-read-only\n",
	} {
		if validScope(scope) {
			t.Fatalf("accepted the undeclared scope %q", scope)
		}
	}
}

// manyTasks builds n independently valid tasks with distinct keys and no
// dependencies, so a refusal over them can only be the count.
func manyTasks(n int) []TaskInput {
	tasks := make([]TaskInput, 0, n)
	for i := 0; i < n; i++ {
		key := "task-" + itoa(i)
		tasks = append(tasks, TaskInput{
			Key: key, Name: key, Arguments: json.RawMessage(`{}`),
			Tier: "required", Scope: "safe-local-read-only", DeadlineSeconds: 60,
		})
	}
	return tasks
}

func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	var digits []byte
	for n > 0 {
		digits = append([]byte{byte('0' + n%10)}, digits...)
		n /= 10
	}
	return string(digits)
}
