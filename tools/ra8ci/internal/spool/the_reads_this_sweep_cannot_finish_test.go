// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// Every read in this package looks at a name before it reads it, and the look
// can pass while the read fails: the record is a regular file this spool
// wrote, and the open is still refused. A host whose outbox was installed by
// one account and swept by another reaches exactly that, and so does one where
// a mode was tightened by hand.
//
// What matters is which way each read falls. A start record that cannot be
// read is reported as missing rather than as a malformed one, so the operator
// looks for a file rather than at its contents. A receipt that cannot be read
// stops the sweep instead of being treated as absent or as present: read as
// absent it uploads a run the server may already hold, read as present it
// retires evidence the server never received.

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

// A receipt is looked for under the spool directory. When the name cannot even
// be looked at, because what the path walks through is not a directory, that
// is neither absent nor present and the sweep says so rather than guessing.
func TestAReceiptThatCannotBeLookedAtIsNeitherAbsentNorPresent(t *testing.T) {
	root := t.TempDir()
	notADirectory := filepath.Join(root, "outbox")
	if err := os.WriteFile(notADirectory, []byte("this is a file"), 0o600); err != nil {
		t.Fatal(err)
	}
	present, err := syncReceiptPresent(filepath.Join(notADirectory, "x.synced.json"))
	if err == nil {
		t.Fatal("a receipt that could not be looked at was answered for")
	}
	if present {
		t.Fatal("a receipt that could not be looked at was read as present")
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
