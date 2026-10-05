// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"io"
	"os"
	"runtime"
	"strings"
	"testing"
)

// A local task run answers with the shell's own vocabulary for how it ended:
// 124 for a deadline, 130 for a cancellation, and the task's own exit code
// otherwise. 130 is what tells an operator who pressed Ctrl-C that the run
// stopped because they said so rather than because the task failed, and
// getting it wrong turns a cancellation into a red build.

// ranLocally runs one local task with both streams captured and the context
// the caller chose, which is the whole point here: the context is what
// carries a cancellation in.
func ranLocally(t *testing.T, ctx context.Context, args []string) (string, string, int) {
	t.Helper()
	outReader, outWriter, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	errReader, errWriter, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	savedOut, savedErr := os.Stdout, os.Stderr
	os.Stdout, os.Stderr = outWriter, errWriter

	spoke := make(chan string, 1)
	complained := make(chan string, 1)
	go func() { body, _ := io.ReadAll(outReader); spoke <- string(body) }()
	go func() { body, _ := io.ReadAll(errReader); complained <- string(body) }()

	status := runLocalTask(ctx, args)

	os.Stdout, os.Stderr = savedOut, savedErr
	if err := outWriter.Close(); err != nil {
		t.Fatal(err)
	}
	if err := errWriter.Close(); err != nil {
		t.Fatal(err)
	}
	return <-spoke, <-complained, status
}

func TestACancelledLocalTaskAnswersOneHundredAndThirty(t *testing.T) {
	if runtime.GOOS == "darwin" {
		t.Skip("the cancellation fixture uses assert-casts, whose catalog support is Linux and Windows")
	}
	offline(t)
	submittableCheckout(t)

	cancelled, stop := context.WithCancel(context.Background())
	stop()

	// assert-casts is reviewed for both Linux and Windows. Using ascii here
	// would stop at the catalog OS gate before the cancellation could be seen.
	_, complained, status := ranLocally(t, cancelled, []string{"assert-casts"})
	if status != 130 {
		t.Fatalf("exit=%d stderr=%q; want 130 for a cancelled run", status, complained)
	}
	if !strings.Contains(complained, "task cancelled") {
		t.Fatalf("stderr=%q; want the cancellation named", complained)
	}
	// The run is still recorded. A cancelled run that left no trace is a run
	// nobody can account for afterwards.
	if !strings.Contains(complained, "unsynced") {
		t.Fatalf("stderr=%q; want the cancelled run spooled anyway", complained)
	}
}

func TestAWorkingDirectoryOutsideAnyCheckoutIsRefusedByName(t *testing.T) {
	// findCheckout walks up from the working directory looking for .git and
	// gives up at the filesystem root. The refusal has to say what was not
	// found, since the usual cause is running ra8ci from somewhere unexpected.
	t.Chdir(t.TempDir())
	if _, err := findCheckout(); err == nil {
		t.Fatal("a directory outside any checkout was accepted")
	} else if !strings.Contains(err.Error(), "no repository checkout found") {
		t.Fatalf("refusal=%v; want it to name what was missing", err)
	}
}
