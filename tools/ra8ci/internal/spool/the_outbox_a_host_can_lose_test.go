// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
)

// The spool is the one place a host's evidence lives between a run and the
// sweep that uploads it. Every door below is the spool refusing to pretend:
// a directory it could not make, a directory that is no longer there, and a
// record it would have to overwrite are each an error the caller is given,
// never a silent success that loses a run.

// A spool directory the host cannot make is refused at Open, with the reason
// the filesystem gave, rather than returning a spool that cannot hold a run.
func TestASpoolDirectoryTheHostCannotMakeIsRefused(t *testing.T) {
	root := t.TempDir()
	occupied := filepath.Join(root, "outbox")
	if err := os.WriteFile(occupied, []byte("not a directory"), 0600); err != nil {
		t.Fatal(err)
	}

	t.Run("under a regular file", func(t *testing.T) {
		s, err := Open(filepath.Join(occupied, "ra8ci"))
		if err == nil {
			t.Fatal("a spool was opened beneath a regular file")
		}
		if s != nil {
			t.Fatal("a refused Open returned a spool")
		}
	})

	t.Run("on a regular file", func(t *testing.T) {
		if _, err := Open(occupied); err == nil {
			t.Fatal("a regular file was opened as a spool")
		}
	})

	t.Run("a relative path", func(t *testing.T) {
		s, err := Open("outbox")
		if err == nil || !strings.Contains(err.Error(), "absolute") {
			t.Fatalf("error = %v, want the path refused as relative", err)
		}
		if s != nil {
			t.Fatal("a refused Open returned a spool")
		}
	})
}

// A spool whose directory has gone is the host losing its own outbox under
// it. Neither half of the spool answers as though nothing had happened: a run
// cannot be started into it, and the pending sweep refuses rather than
// reporting an empty outbox, which a caller would read as everything synced.
func TestASpoolWhoseDirectoryIsGoneRefusesBothWays(t *testing.T) {
	s := aSpool(t)
	if err := os.RemoveAll(s.directory); err != nil {
		t.Fatal(err)
	}

	if _, err := s.Begin("ra8ci:build", strings.Repeat("a", 64)); err == nil {
		t.Fatal("a run was started into a spool directory that is not there")
	}

	pending, err := s.Pending()
	if err == nil {
		t.Fatal("a missing outbox was read as an outbox holding nothing")
	}
	if pending != nil {
		t.Fatalf("pending = %v, want nothing handed back beside the refusal", pending)
	}
}

// A terminal record is written once. A second finish of the same run is
// refused rather than replacing the record already on disk, because the
// record the sweep may already have uploaded is the one that counts.
func TestARecordAlreadyOnDiskIsNotReplaced(t *testing.T) {
	s, entry := aRunningSpool(t)
	measured := executor.Result{TaskName: "ra8ci:build", ExitCode: 0}

	finished, err := s.Finish(entry, measured, nil)
	if err != nil {
		t.Fatal(err)
	}
	if finished.SyncState != "unsynced" {
		t.Fatalf("sync state = %q, want the record waiting for the sweep", finished.SyncState)
	}

	if _, err := s.Finish(entry, measured, nil); err == nil {
		t.Fatal("a run was finished twice over the same record")
	}

	// The refusal left the outbox exactly as the first finish did: one
	// record, still unsynced, still the one a sweep would send.
	pending, err := s.Pending()
	if err != nil {
		t.Fatal(err)
	}
	if len(pending) != 1 {
		t.Fatalf("pending records = %d, want the one already written", len(pending))
	}
	if pending[0].ID != finished.ID || pending[0].SyncState != "unsynced" {
		t.Fatalf("pending record = %+v, want the first finish left standing", pending[0])
	}
}

// Nothing a failed write leaves behind is ever offered to the sweep: the
// temporary file the spool writes through is removed whether the record
// landed or not, so a directory listing holds records and nothing else.
func TestAWriteLeavesNoTemporaryBehind(t *testing.T) {
	s, entry := aRunningSpool(t)
	if _, err := s.Finish(entry, executor.Result{TaskName: "ra8ci:build", ExitCode: 0}, nil); err != nil {
		t.Fatal(err)
	}
	// A second finish fails after its temporary is written, which is the
	// case worth pinning: the failure path has to clean up too.
	if _, err := s.Finish(entry, executor.Result{TaskName: "ra8ci:build", ExitCode: 0}, nil); err == nil {
		t.Fatal("a run was finished twice over the same record")
	}

	files, err := os.ReadDir(s.directory)
	if err != nil {
		t.Fatal(err)
	}
	var left []string
	for _, file := range files {
		if strings.HasPrefix(file.Name(), ".ra8ci-") {
			left = append(left, file.Name())
		}
	}
	if len(left) != 0 {
		t.Fatalf("temporary files left behind: %v", left)
	}
	if len(files) != 2 {
		t.Fatalf("outbox holds %d files, want the start and finish records only", len(files))
	}
}
