// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// These drive MarkSynced itself rather than the helper, because the wiring is
// what was weak: the rule is only worth the door it is called from.

// recordPath is where MarkSynced looks for the record a receipt retires.
func recordPath(s *Spool, id string) string {
	return filepath.Join(s.directory, id+".finished.json")
}

func TestARecordThisSpoolWroteIsStillRetired(t *testing.T) {
	s, entry := spooledRun(t)
	if err := s.MarkSynced(entry.ID, "server-run-1"); err != nil {
		t.Fatalf("an honest record was not retired: %v", err)
	}
	pending, err := s.Pending()
	if err != nil || len(pending) != 0 {
		t.Fatalf("a retired record is still pending: %d %v", len(pending), err)
	}
}

func TestALinkedRecordIsNotRetiredByAReceipt(t *testing.T) {
	s, entry := spooledRun(t)
	elsewhere := filepath.Join(t.TempDir(), "somebody-elses-record.json")
	if err := os.WriteFile(elsewhere, []byte(`{"id":"x"}`), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.Remove(recordPath(s, entry.ID)); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(elsewhere, recordPath(s, entry.ID)); err != nil {
		t.Fatal(err)
	}
	err := s.MarkSynced(entry.ID, "server-run-1")
	if !errors.Is(err, errRetiredRecordIsNotAFile) {
		t.Fatalf("a link standing in for a record was retired: %v", err)
	}
	if _, statErr := os.Lstat(receiptPath(s, entry.ID)); !os.IsNotExist(statErr) {
		t.Fatalf("a refused record still had a receipt written beside it: %v", statErr)
	}
}

func TestADirectoryNamedLikeARecordIsNotRetired(t *testing.T) {
	s, entry := spooledRun(t)
	if err := os.Remove(recordPath(s, entry.ID)); err != nil {
		t.Fatal(err)
	}
	if err := os.Mkdir(recordPath(s, entry.ID), 0700); err != nil {
		t.Fatal(err)
	}
	if err := s.MarkSynced(entry.ID, "server-run-1"); !errors.Is(err, errRetiredRecordIsNotAFile) {
		t.Fatalf("a directory standing in for a record was retired: %v", err)
	}
}

func TestAMissingRecordIsStillReportedAsMissingByMarkSynced(t *testing.T) {
	s, entry := spooledRun(t)
	if err := os.Remove(recordPath(s, entry.ID)); err != nil {
		t.Fatal(err)
	}
	err := s.MarkSynced(entry.ID, "server-run-1")
	if !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("a missing record was reported as something other than missing: %v", err)
	}
	if errors.Is(err, errRetiredRecordIsNotAFile) {
		t.Fatal("a missing record was reported under the file-kind refusal")
	}
}

func TestAStartRecordCannotBeRetiredInPlaceOfATerminalOne(t *testing.T) {
	s, entry := spooledRun(t)
	if err := os.Remove(recordPath(s, entry.ID)); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Lstat(filepath.Join(s.directory, entry.ID+".started.json")); err != nil {
		t.Fatalf("the start record this test rests on is gone: %v", err)
	}
	if err := s.MarkSynced(entry.ID, "server-run-1"); err == nil {
		t.Fatal("the start record was retired in place of the terminal one")
	}
}

func TestTheRefusalNamesTheRecordAndNotItsContents(t *testing.T) {
	s, entry := spooledRun(t)
	secret := filepath.Join(t.TempDir(), "private.json")
	if err := os.WriteFile(secret, []byte(`{"token":"hunter2"}`), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.Remove(recordPath(s, entry.ID)); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(secret, recordPath(s, entry.ID)); err != nil {
		t.Fatal(err)
	}
	err := s.MarkSynced(entry.ID, "server-run-1")
	if err == nil {
		t.Fatal("a link standing in for a record was retired")
	}
	if !strings.Contains(err.Error(), entry.ID+".finished.json") {
		t.Fatalf("the refusal did not name the record it refused: %q", err.Error())
	}
	for _, chosen := range []string{"hunter2", "token"} {
		if strings.Contains(err.Error(), chosen) {
			t.Fatalf("the refusal put the linked file's contents into the pass: %q", err.Error())
		}
	}
}
