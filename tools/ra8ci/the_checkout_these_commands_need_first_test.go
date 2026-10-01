// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"io"
	"strings"
	"testing"
)

// Five commands read the checkout, and each one calls findCheckout before it
// reaches anything expensive: a database, a source snapshot, a gate that
// walks the tree. The ORDER is the property. A command that opened its
// database first and only then discovered it was not standing in a
// repository would spend a connection, and in the HIL case a fifteen second
// timeout, to arrive at a refusal it could have made immediately.
//
// runLocalTask and hilVerifyCaptureCommand already carry this assertion
// (how_a_cancelled_local_run_ends_test.go and
// the_capture_a_verification_can_still_reach_test.go). These are the other
// three.

// hil budget names a database in its environment and still never looks at
// it. The DSN below is deliberately one that would fail loudly if it were
// ever dialled, so the refusal naming the checkout is evidence the command
// stopped in front of it.
func TestHILBudgetRefusesOutsideACheckoutBeforeItReadsItsDatabase(t *testing.T) {
	directory := t.TempDir()
	noCheckoutAbove(t, directory)
	t.Chdir(directory)
	t.Setenv("RA8CI_DATABASE_URL", "postgres://nobody@127.0.0.1:1/nothing")

	err := hilBudgetCommand(context.Background(), []string{
		"--board-id", "board-1",
		"--manifest", "examples/hil/hil.conf",
		"--board-model", "ra8m1",
		"--program-family", "blinky",
		"--flash-restore-bound", "30s",
	})
	if err == nil {
		t.Fatal("hil budget ran outside a checkout")
	}
	if !strings.Contains(err.Error(), "no repository checkout found") {
		t.Fatalf("error = %q; want the absent checkout named, not a database failure", err)
	}
}

// run submit refuses before it tries to snapshot a source tree that is not
// there. Its arguments parse and its tasks split, so the refusal can only
// come from the checkout lookup.
func TestRunSubmitRefusesOutsideACheckoutBeforeItSnapshotsSource(t *testing.T) {
	directory := t.TempDir()
	noCheckoutAbove(t, directory)
	t.Chdir(directory)

	err := submitRun(context.Background(), []string{"-idempotency-key", "k-1", "build"})
	if err == nil {
		t.Fatal("run submit ran outside a checkout")
	}
	if !strings.Contains(err.Error(), "no repository checkout found") {
		t.Fatalf("error = %q; want the absent checkout named, not a snapshot failure", err)
	}
}

// A checkout gate never reaches its gate function outside a checkout. The
// recorder is the assertion: a gate that ran would have walked a tree that is
// not a repository and reported findings about it.
func TestACheckoutGateNeverReachesItsGateOutsideACheckout(t *testing.T) {
	directory := t.TempDir()
	noCheckoutAbove(t, directory)
	t.Chdir(directory)

	reached := false
	status := runCheckoutGate(context.Background(), "waverefs", []string{"--all"},
		func(context.Context, string, []string, io.Writer, io.Writer) int {
			reached = true
			return 0
		})
	if reached {
		t.Fatal("the gate ran over a directory that is not a checkout")
	}
	if status == 0 {
		t.Fatal("a gate that never ran answered success")
	}
}

// And with no arguments of its own the same entry point is the catalog task
// of that name instead, which is a different path and still refuses here.
// This pins the fork, so a future argument change cannot silently send an
// empty invocation down the gate branch.
func TestACheckoutGateWithNoArgumentsTakesTheTaskPathInstead(t *testing.T) {
	directory := t.TempDir()
	noCheckoutAbove(t, directory)
	t.Chdir(directory)

	reached := false
	status := runCheckoutGate(context.Background(), "waverefs", nil,
		func(context.Context, string, []string, io.Writer, io.Writer) int {
			reached = true
			return 0
		})
	if reached {
		t.Fatal("an invocation with no gate arguments reached the gate")
	}
	if status == 0 {
		t.Fatal("the task path answered success outside a checkout")
	}
}
