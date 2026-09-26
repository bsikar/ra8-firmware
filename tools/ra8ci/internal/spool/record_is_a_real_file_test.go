// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// linkOver replaces one name under the spool with a symlink to target,
// which is what a record written by something other than this package looks
// like. The original is read back out of elsewhere so the test can prove the
// difference between refusing the link and following it.
func linkOver(t *testing.T, s *Spool, name, target string) {
	t.Helper()
	if runtime.GOOS == "windows" {
		t.Skip("symlinks need a privilege this test does not assume on Windows")
	}
	path := filepath.Join(s.directory, name)
	if err := os.Remove(path); err != nil && !os.IsNotExist(err) {
		t.Fatal(err)
	}
	if err := os.Symlink(target, path); err != nil {
		t.Fatal(err)
	}
}

// moveAside carries a real record out of the spool and returns where it went,
// so a symlink can be pointed at content that is genuinely valid. A refusal
// that only worked on unreadable bytes would prove nothing.
func moveAside(t *testing.T, s *Spool, name string) string {
	t.Helper()
	raw, err := os.ReadFile(filepath.Join(s.directory, name))
	if err != nil {
		t.Fatal(err)
	}
	outside := filepath.Join(t.TempDir(), name)
	if err := os.WriteFile(outside, raw, 0600); err != nil {
		t.Fatal(err)
	}
	return outside
}

func TestAnHonestPairIsStillReadAsRegularFiles(t *testing.T) {
	s, entry := spooledRun(t)
	pending, err := s.Pending()
	if err != nil {
		t.Fatal(err)
	}
	if len(pending) != 1 || pending[0].ID != entry.ID {
		t.Fatalf("regular records were not read: %+v %v", pending, err)
	}
}

// The terminal record is what the upload marshals, so a link here puts a file
// from outside the private spool into durable server history under this run.
func TestALinkedTerminalRecordIsRefusedRatherThanFollowed(t *testing.T) {
	s, entry := spooledRun(t)
	outside := moveAside(t, s, entry.ID+".finished.json")
	linkOver(t, s, entry.ID+".finished.json", outside)
	pending, err := s.Pending()
	if err == nil || !strings.Contains(err.Error(), "is not a regular file") {
		t.Fatalf("linked terminal record was followed: %+v %v", pending, err)
	}
	if len(pending) != 0 {
		t.Fatalf("records offered for upload past a refusal: %+v", pending)
	}
}

// The content behind the link is a valid record this spool itself wrote, so
// the refusal is about the name being a link and nothing else.
func TestTheRefusalIsAboutTheLinkNotItsContents(t *testing.T) {
	s, entry := spooledRun(t)
	outside := moveAside(t, s, entry.ID+".finished.json")
	raw, err := os.ReadFile(outside)
	if err != nil {
		t.Fatal(err)
	}
	linkOver(t, s, entry.ID+".finished.json", outside)
	if _, err := s.Pending(); err == nil {
		t.Fatal("linked record accepted")
	}
	if err := os.Remove(filepath.Join(s.directory, entry.ID+".finished.json")); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(s.directory, entry.ID+".finished.json"), raw, 0600); err != nil {
		t.Fatal(err)
	}
	pending, err := s.Pending()
	if err != nil || len(pending) != 1 || pending[0].ID != entry.ID {
		t.Fatalf("the same bytes as a regular file were refused: %+v %v", pending, err)
	}
}

// Read as present, a linked receipt retires a record the server never
// acknowledged, which is the one loss this outbox exists to prevent.
func TestALinkedSyncReceiptDoesNotRetireARecord(t *testing.T) {
	s, entry := spooledRun(t)
	elsewhere := filepath.Join(t.TempDir(), "receipt.json")
	if err := os.WriteFile(elsewhere, []byte(`{"local_id":"x","server_run_id":"y"}`), 0600); err != nil {
		t.Fatal(err)
	}
	linkOver(t, s, entry.ID+".synced.json", elsewhere)
	pending, err := s.Pending()
	if err == nil || !strings.Contains(err.Error(), "sync receipt") {
		t.Fatalf("linked receipt was trusted: %+v %v", pending, err)
	}
	if len(pending) != 0 {
		t.Fatalf("pass continued past a refused receipt: %+v", pending)
	}
}

// A receipt that is not there is the ordinary state of an unsynced record and
// stays absent, so the new refusal cannot swallow the common path.
func TestAnAbsentReceiptIsStillAbsent(t *testing.T) {
	s, entry := spooledRun(t)
	present, err := syncReceiptPresent(filepath.Join(s.directory, entry.ID+".synced.json"))
	if err != nil || present {
		t.Fatalf("missing receipt was not read as absent: %v %v", present, err)
	}
	if err := s.MarkSynced(entry.ID, "server-run-1"); err != nil {
		t.Fatal(err)
	}
	present, err = syncReceiptPresent(filepath.Join(s.directory, entry.ID+".synced.json"))
	if err != nil || !present {
		t.Fatalf("a written receipt was not read as present: %v %v", present, err)
	}
}

// A directory is the other shape that is not a regular file, and it reaches
// Pending because a directory entry that is a directory is filtered out while
// a name that only becomes one through a link is not.
func TestADirectoryInPlaceOfARecordIsRefused(t *testing.T) {
	s, entry := spooledRun(t)
	elsewhere := t.TempDir()
	linkOver(t, s, entry.ID+".finished.json", elsewhere)
	if _, err := s.Pending(); err == nil || !strings.Contains(err.Error(), "is not a regular file") {
		t.Fatalf("a linked directory was read as a record: %v", err)
	}
}

// readRegularFile still reports a name that is simply not there as missing,
// rather than folding every failure into one refusal.
func TestAMissingRecordIsStillMissing(t *testing.T) {
	s, _ := spooledRun(t)
	_, err := readRegularFile(filepath.Join(s.directory, "nothing.finished.json"))
	if err == nil || !os.IsNotExist(err) {
		t.Fatalf("absent name was not reported as missing: %v", err)
	}
}
