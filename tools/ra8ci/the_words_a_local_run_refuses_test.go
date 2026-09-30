// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"io"
	"os"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

// runLocalTask refuses four different ways before it opens a spool or runs
// anything, and the four refusals do not read alike on purpose: each names
// what the caller should type instead. They are also the last point at which
// a mistake is free, so the ordering between them is worth holding.
//
// Two of them are only reachable through the one catalog task that declares
// arguments. parseTaskArguments and BindArguments both apply the same value
// rules, so a malformed VALUE is caught by the first and never reaches the
// second. What only the second can see is an argument that is missing or was
// never declared, and that is the gap these cover.

// localRunStderr runs runLocalTask with both standard streams captured and
// hands back its status and whatever it said. The working directory is a
// temporary one, so a refusal that slipped past these checks would fail at
// the checkout lookup rather than running a task over a real tree.
func localRunStderr(t *testing.T, args []string) (int, string) {
	t.Helper()
	directory := t.TempDir()
	noCheckoutAbove(t, directory)
	t.Chdir(directory)

	reader, writer, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	savedOut, savedErr := os.Stdout, os.Stderr
	os.Stdout, os.Stderr = writer, writer
	status := runLocalTask(context.Background(), args)
	os.Stdout, os.Stderr = savedOut, savedErr
	_ = writer.Close()
	said, err := io.ReadAll(reader)
	if err != nil {
		t.Fatal(err)
	}
	_ = reader.Close()
	return status, string(said)
}

// A task nobody declares is named back rather than run.
func TestALocalRunNamesTheTaskItDoesNotKnow(t *testing.T) {
	status, said := localRunStderr(t, []string{"not-a-task"})
	if status != 2 {
		t.Fatalf("status = %d; want 2", status)
	}
	if !strings.Contains(said, "unknown task not-a-task") {
		t.Fatalf("said %q; want the unknown task named", said)
	}
}

// The declared positional is required, and leaving it off is caught by
// binding rather than by the name=value parse, which sees nothing at all to
// object to. The refusal has to name the argument, since "usage" alone would
// not tell the caller which one is missing.
func TestALocalRunNamesThePositionalItWasNotGiven(t *testing.T) {
	status, said := localRunStderr(t, []string{"ascii-rewrite"})
	if status != 2 {
		t.Fatalf("status = %d; want 2", status)
	}
	if !strings.Contains(said, "path") || !strings.Contains(said, "required") {
		t.Fatalf("said %q; want the missing positional named", said)
	}
}

// An argument the task never declared is refused rather than dropped. The
// name and the value both pass the shape rules, so this too is binding's
// refusal and not the parse's: a caller who misremembered the argument name
// would otherwise watch the task run without the value they supplied.
func TestALocalRunRefusesAnArgumentTheTaskNeverDeclared(t *testing.T) {
	status, said := localRunStderr(t, []string{"ascii-rewrite", "path=docs", "depth=2"})
	if status != 2 {
		t.Fatalf("status = %d; want 2", status)
	}
	if !strings.Contains(said, "depth") {
		t.Fatalf("said %q; want the undeclared argument named", said)
	}
}

// A value that breaks the shape rules is caught one step earlier, by the
// name=value parse, and says so in its own words. Pinning both sides is what
// keeps the two refusals from collapsing into one another.
func TestALocalRunRefusesAValueBeforeItEverBinds(t *testing.T) {
	status, said := localRunStderr(t, []string{"ascii-rewrite", "path=docs|etc"})
	if status != 2 {
		t.Fatalf("status = %d; want 2", status)
	}
	if !strings.Contains(said, "invalid value for argument") {
		t.Fatalf("said %q; want the parse refusal, not binding's", said)
	}
}

// runLocalTask also refuses a task that needs server dispatch, and today
// nothing reaches that branch: every task in the reviewed catalog is
// safe-local. That is worth stating rather than leaving as an unexplained
// gap in coverage. If this ever fails, the branch has become live and wants
// a test of its own.
func TestEveryReviewedTaskIsStillSafeToRunLocally(t *testing.T) {
	cat, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	dispatched := []string(nil)
	for _, name := range cat.Names() {
		task, ok := cat.Task(name)
		if !ok {
			t.Fatalf("catalog names %q and then does not hold it", name)
		}
		if !task.IsSafeLocal() {
			dispatched = append(dispatched, task.Name+" ("+task.Scope+")")
		}
	}
	if len(dispatched) != 0 {
		t.Fatalf("the catalog now holds server-dispatch tasks: %s; "+
			"runLocalTask's dispatch refusal is live and wants its own test",
			strings.Join(dispatched, ", "))
	}
}
