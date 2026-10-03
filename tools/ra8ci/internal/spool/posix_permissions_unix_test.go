//go:build unix

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestOpenRejectsSharedDirectory(t *testing.T) {
	target := filepath.Join(t.TempDir(), "outbox")
	if err := os.Mkdir(target, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(target, 0o777); err != nil {
		t.Fatal(err)
	}
	if _, err := Open(target); err == nil {
		t.Fatal("shared directory accepted")
	}
}

// sealed writes content at path and takes every permission off it, so the
// name is a regular file that cannot be opened.
func sealed(t *testing.T, path, content string) string {
	t.Helper()
	if os.Geteuid() == 0 {
		t.Skip("running as root: a mode of 0 would still be readable")
	}
	if err := os.WriteFile(path, []byte(content), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(path, 0o000); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(path, 0o600) })
	return path
}

func TestAStartRecordThatCannotBeReadIsReportedAsMissing(t *testing.T) {
	s := aSpool(t)
	id := "33333333333333333333333333333333"
	sealed(t, filepath.Join(s.directory, id+".started.json"), `{"id":"`+id+`"}`)
	_, err := s.readStarted(id)
	if err == nil || !strings.Contains(err.Error(), "missing start record") {
		t.Fatalf("error = %v, want an unreadable record reported missing", err)
	}
	// Not the other refusal: the file IS regular, and saying it is not would
	// send an operator looking for a link that is not there.
	if strings.Contains(err.Error(), "not a regular file") {
		t.Fatalf("an unreadable regular record was reported as the wrong kind of file: %v", err)
	}
}

// And the same window one step further in: the receipt is a regular file when
// it is looked at and refuses to open when it is read. The record stays
// pending, which is the recoverable direction: an operator who really did
// upload it can restate the receipt, while a record retired on a read nobody
// finished is gone.
func TestAReceiptThatCannotBeReadDoesNotRetireItsRecord(t *testing.T) {
	s := aSpool(t)
	id := "44444444444444444444444444444444"
	path := filepath.Join(s.directory, id+".synced.json")
	sealed(t, path, `{"local_id":"`+id+`","server_run_id":"run-1"}`)
	retired, err := receiptRetiresRecord(path, id)
	if err == nil {
		t.Fatal("an unreadable receipt was judged")
	}
	if retired {
		t.Fatal("an unreadable receipt retired the record it sits beside")
	}
}
