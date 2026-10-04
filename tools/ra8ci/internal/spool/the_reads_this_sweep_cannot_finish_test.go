// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"os"
	"path/filepath"
	"runtime"

	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/testprivatefile"
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

// A receipt is looked for under the spool directory. When the name cannot even
// be looked at, because what the path walks through is not a directory, that
// is neither absent nor present and the sweep says so rather than guessing.
func TestAReceiptThatCannotBeLookedAtIsNeitherAbsentNorPresent(t *testing.T) {
	root := t.TempDir()
	if runtime.GOOS != "windows" {
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
		return
	}
	receipt := filepath.Join(root, "receipt.synced.json")
	if err := os.WriteFile(receipt, []byte("{}"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := testprivatefile.Unreadable(receipt); err != nil {
		t.Fatalf("make receipt unreadable: %v", err)
	}
	present, err := syncReceiptPresent(receipt)
	if err == nil {
		t.Fatal("a receipt that could not be looked at was answered for")
	}
	if present {
		t.Fatal("a receipt that could not be looked at was read as present")
	}
}
