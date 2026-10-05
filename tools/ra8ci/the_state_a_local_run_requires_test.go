// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"os"
	"path/filepath"
	"runtime"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/testprivatefile"
)

// A local task run spends the box: it shells out to real scripts and it writes
// a result into the outbox that a later sync uploads as evidence. Everything
// below is a state failure rather than a bad invocation, so each one answers 1
// rather than the 2 a usage refusal answers, and that difference is the whole
// point: an operator reading 2 goes and fixes their command line, an operator
// reading 1 goes and fixes their machine.

// refusedLocalRun runs the smallest real task under whatever state the caller
// arranged and hands back the exit code.
func refusedLocalRun(t *testing.T) int {
	t.Helper()
	return runLocalTask(context.Background(), []string{"assert-casts"})
}

func TestALocalTaskRefusesAStateDirectoryItCannotTrust(t *testing.T) {
	cases := map[string]func(t *testing.T) string{
		"a state directory named by a relative path": func(t *testing.T) string {
			// DefaultDirectory refuses this before the spool is ever opened:
			// a relative state directory would follow the caller's working
			// directory, and a task changing directory mid-run would then
			// spool its result somewhere nobody looks.
			t.Setenv("RA8CI_STATE_DIR", filepath.Join("relative", "outbox"))
			return ""
		},
		"a state directory that cannot be created": func(t *testing.T) string {
			blocked := filepath.Join(t.TempDir(), "occupied")
			if err := os.WriteFile(blocked, []byte("not a directory"), 0o600); err != nil {
				t.Fatal(err)
			}
			outbox := filepath.Join(blocked, "outbox")
			t.Setenv("RA8CI_STATE_DIR", outbox)
			return ""
		},
		"a state directory other users can read": func(t *testing.T) string {
			outbox := filepath.Join(t.TempDir(), "outbox")
			if err := os.MkdirAll(outbox, 0o700); err != nil {
				t.Fatal(err)
			}
			if err := testprivatefile.OtherUsersReadable(outbox); err != nil {
				t.Fatalf("grant broad access to state directory: %v", err)
			}
			return outbox
		},
	}
	for name, arrange := range cases {
		t.Run(name, func(t *testing.T) {
			submittableCheckout(t)
			outbox := arrange(t)
			if outbox != "" {
				t.Setenv("RA8CI_STATE_DIR", outbox)
			}

			if status := refusedLocalRun(t); status != 1 {
				t.Fatalf("exit=%d; want 1 for a state directory the run cannot trust", status)
			}
			// The refusal lands before the task runs, so nothing was spooled
			// and no half-written attempt is left for a sync to pick up.
			if outbox != "" {
				if held := spooled(t, outbox); held != 0 {
					t.Fatalf("outbox holds %d results; want nothing spooled for a refusal", held)
				}
			}
		})
	}
}

// A loose state directory is refused for what it is, not for where it is: the
// same path is taken once it is private, which is what keeps the refusal from
// reading as this code sometimes declining a directory it made itself.
func TestALocalTaskTakesAStateDirectoryOnceItIsPrivate(t *testing.T) {
	if runtime.GOOS == "darwin" {
		t.Skip("assert-casts is catalog-supported on Linux and Windows, not macOS")
	}
	submittableCheckout(t)
	outbox := filepath.Join(t.TempDir(), "outbox")
	if err := os.MkdirAll(outbox, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := testprivatefile.OtherUsersReadable(outbox); err != nil {
		t.Fatalf("grant broad access to state directory: %v", err)
	}
	t.Setenv("RA8CI_STATE_DIR", outbox)

	if status := refusedLocalRun(t); status != 1 {
		t.Fatalf("exit=%d; want 1 while the directory is readable by others", status)
	}
	if err := testprivatefile.OwnerOnly(outbox); err != nil {
		t.Fatalf("restrict state directory to its owner: %v", err)
	}

	// Now the run reaches a Windows-supported task. The task result matters
	// less than proving the attempt was recorded after the directory passed
	// its privacy check.
	if status := refusedLocalRun(t); status == 1 {
		t.Fatal("a private state directory was still refused as untrusted")
	}
	if held := spooled(t, outbox); held == 0 {
		t.Fatal("the run left nothing in the outbox once the directory was private")
	}
}

// findCheckout runs ahead of every state decision, so a caller standing outside
// a repository is told that and not something about their outbox.
func TestALocalTaskRefusesToRunOutsideACheckout(t *testing.T) {
	outbox := privateOutbox(t)
	t.Chdir(t.TempDir())

	if status := refusedLocalRun(t); status != 1 {
		t.Fatalf("exit=%d; want 1 outside a repository checkout", status)
	}
	if held := spooled(t, outbox); held != 0 {
		t.Fatalf("outbox holds %d results; want nothing spooled outside a checkout", held)
	}
}
