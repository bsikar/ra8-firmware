// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/asciigate"
)

// A gate named on the command line has two quite different jobs behind one
// word. With no arguments it is a catalog task, spooled and executed like any
// other local run. With arguments it is the gate itself, handed the checkout
// to read. Nothing had driven either side, because both need a real checkout
// underneath them; submittableCheckout supplies one.

// privateOutbox points the local spool at a directory of its own, so a local
// run records where the test can see it and never writes into a real home.
func privateOutbox(t *testing.T) string {
	t.Helper()
	outbox := filepath.Join(t.TempDir(), "outbox")
	t.Setenv("RA8CI_STATE_DIR", outbox)
	return outbox
}

// spooled reports how many results the local outbox holds.
func spooled(t *testing.T, outbox string) int {
	t.Helper()
	entries, err := os.ReadDir(outbox)
	if err != nil {
		if os.IsNotExist(err) {
			return 0
		}
		t.Fatal(err)
	}
	count := 0
	for _, entry := range entries {
		if !entry.IsDir() && strings.HasSuffix(entry.Name(), ".json") {
			count++
		}
	}
	return count
}

func TestAGateWithArgumentsIsHandedTheCheckoutToRead(t *testing.T) {
	root := submittableCheckout(t)
	outbox := privateOutbox(t)

	status := runCheckoutGate(context.Background(), "ascii", []string{"--selftest"}, asciigate.Run)

	// The gate derives its scope from the checkout it was handed, and this
	// one holds two files against a floor of 2500, so it refuses. That
	// refusal is the assertion: exit 2 is the gate's own answer about this
	// tree, which it could only reach by being given this root. A missing
	// checkout is refused at 1, and the catalog path would have spooled.
	if status != 2 {
		t.Fatalf("exit=%d; want the gate's own refusal over this tree", status)
	}
	// A gate is a read. Nothing about it belongs in the local outbox, which is
	// the record of work that still has to reach the plane.
	if held := spooled(t, outbox); held != 0 {
		t.Fatalf("outbox holds %d results; want a gate to spool nothing", held)
	}
	if root == "" {
		t.Fatal("no checkout was planted")
	}
}

func TestAGateWithArgumentsRefusesWithoutACheckout(t *testing.T) {
	// A directory with no .git anywhere above it: findCheckout walks to the
	// filesystem root and gives up, and the gate is never reached.
	t.Chdir(t.TempDir())
	privateOutbox(t)

	status := runCheckoutGate(context.Background(), "ascii", []string{"--selftest"}, asciigate.Run)
	if status != 1 {
		t.Fatalf("exit=%d; want the missing checkout refused", status)
	}
}

func TestAGateWithNoArgumentsIsRunAsACatalogTask(t *testing.T) {
	submittableCheckout(t)
	outbox := privateOutbox(t)

	// The fixture holds the catalog and nothing else, so the task's steps have
	// no scripts to run and the execution fails. That failure is the point:
	// reaching it means the word went to the catalog, was bound, was found
	// safe to run locally, and was spooled before anything was executed.
	status := runCheckoutGate(context.Background(), "ascii", nil, asciigate.Run)
	if status == 0 {
		t.Fatal("a task with no scripts under it reported success")
	}
	if held := spooled(t, outbox); held == 0 {
		t.Fatal("the local run left nothing in the outbox to sync")
	}
}

func TestALocalTaskRecordsItsResultBeforeItIsJudged(t *testing.T) {
	submittableCheckout(t)
	outbox := privateOutbox(t)

	status := runLocalTask(context.Background(), []string{"ascii"})
	if status == 0 {
		t.Fatal("a task with no scripts under it reported success")
	}
	// The result is persisted whether the task passed or failed: an unsynced
	// failure is exactly the thing the outbox exists to carry.
	if held := spooled(t, outbox); held == 0 {
		t.Fatal("the failure left nothing in the outbox to sync")
	}
}

func TestALocalTaskRefusesWhatItWillNotRun(t *testing.T) {
	cases := map[string]struct {
		args []string
		want int
	}{
		"a task no catalog has":           {[]string{"not-a-task"}, 2},
		"a value the task never declared": {[]string{"ascii", "depth=2"}, 2},
	}
	for name, testCase := range cases {
		t.Run(name, func(t *testing.T) {
			submittableCheckout(t)
			outbox := privateOutbox(t)

			if status := runLocalTask(context.Background(), testCase.args); status != testCase.want {
				t.Fatalf("exit=%d; want %d", status, testCase.want)
			}
			// Every one of these is refused before the spool is opened, so a
			// refused invocation leaves nothing behind to sync.
			if held := spooled(t, outbox); held != 0 {
				t.Fatalf("outbox holds %d results; want nothing spooled for a refusal", held)
			}
		})
	}
}

func TestLocalSourceIdentityReadsTheCheckoutItIsGiven(t *testing.T) {
	root := submittableCheckout(t)

	identity, err := localSourceIdentity(context.Background(), root)
	if err != nil {
		t.Fatalf("a committed checkout had no source identity: %v", err)
	}
	if identity.Repository != "bsikar/ra8-firmware" {
		t.Fatalf("repository=%q; want the default", identity.Repository)
	}
	if identity.CommitSHA != gitSpoke(t, root, "rev-parse", "HEAD") {
		t.Fatalf("commit=%q; want the checkout's HEAD", identity.CommitSHA)
	}
	// A clean committed tree can be snapshotted, and the snapshot's root
	// commit agrees with HEAD, so the identity carries a digest and says so.
	if identity.Verification != "verified" {
		t.Fatalf("verification=%q; want a snapshotted checkout to read as verified", identity.Verification)
	}
	if len(identity.SnapshotSHA256) != 64 {
		t.Fatalf("snapshot digest=%q; want the pinned tree's sha256", identity.SnapshotSHA256)
	}
	if identity.Branch != "ra8ci/dev" {
		t.Fatalf("branch=%q; want the branch the checkout is on", identity.Branch)
	}
}

func TestLocalSourceIdentityWillNotCallADirtyTreeVerified(t *testing.T) {
	root := submittableCheckout(t)
	if err := os.WriteFile(filepath.Join(root, "scratch.txt"), []byte("uncommitted\n"), 0o644); err != nil {
		t.Fatal(err)
	}

	identity, err := localSourceIdentity(context.Background(), root)
	if err != nil {
		t.Fatalf("a dirty checkout had no source identity at all: %v", err)
	}
	// The commit is still readable, so the identity is still issued. What it
	// must not do is carry a digest: the tree on disk is not the tree that
	// commit names, and a verified identity would say it was.
	if identity.CommitSHA != gitSpoke(t, root, "rev-parse", "HEAD") {
		t.Fatalf("commit=%q; want HEAD even with work uncommitted", identity.CommitSHA)
	}
	if identity.Verification != "unverified" {
		t.Fatalf("verification=%q; want a dirty tree to read as unverified", identity.Verification)
	}
	if identity.SnapshotSHA256 != "" {
		t.Fatalf("snapshot digest=%q; want none for a tree that was never pinned", identity.SnapshotSHA256)
	}
}

func TestLocalSourceIdentityCarriesTheRepositoryTheEnvironmentNames(t *testing.T) {
	root := submittableCheckout(t)
	t.Setenv("RA8CI_REPOSITORY", "bsikar/ra8-emulator")

	identity, err := localSourceIdentity(context.Background(), root)
	if err != nil {
		t.Fatalf("a committed checkout had no source identity: %v", err)
	}
	if identity.Repository != "bsikar/ra8-emulator" {
		t.Fatalf("repository=%q; want the one the environment named", identity.Repository)
	}
}
