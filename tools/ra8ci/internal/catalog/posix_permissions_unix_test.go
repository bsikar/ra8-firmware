//go:build unix

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	errors "errors"
	os "os"
	filepath "path/filepath"
	strings "strings"
	testing "testing"
)

// A checkout file the review cannot open is refused by the read itself, not
// by the parse that would have followed. Root would sail past a sealed file,
// so the test says what it needs and skips rather than asserting falsely.
func TestACheckoutFileThatWillNotOpenIsRefused(t *testing.T) {
	if os.Geteuid() == 0 {
		t.Skip("running as root: a sealed file still opens")
	}
	sealed := filepath.Join(t.TempDir(), "tasks.json")
	if err := os.WriteFile(sealed, []byte(`{"schema_version":1}`), 0o000); err != nil {
		t.Fatalf("plant a sealed manifest: %v", err)
	}
	if _, err := readCheckoutFile(sealed, maxReadableManifestBytes); err == nil {
		t.Fatal("a sealed manifest was read")
	}
	readable := filepath.Join(t.TempDir(), "tasks.json")
	if err := os.WriteFile(readable, []byte(`{"schema_version":1}`), 0o644); err != nil {
		t.Fatalf("plant a readable manifest: %v", err)
	}
	if _, err := readCheckoutFile(readable, maxReadableManifestBytes); err != nil {
		t.Fatalf("a readable manifest was refused: %v", err)
	}
}

// VerifyCheckout is handed a root by its caller and makes it absolute before
// it reads anything. A relative root is resolved against the working
// directory, so a process whose own directory has been removed underneath it
// cannot name a checkout at all. The refusal belongs here, where it names the
// checkout, rather than further down where it would surface as a missing
// .git or an unreadable manifest and send a reader looking at the wrong file.
func TestACheckoutNamedFromALostDirectoryIsRefused(t *testing.T) {
	if os.Geteuid() == 0 {
		t.Skip("running as root changes which directory operations fail")
	}
	lost := filepath.Join(t.TempDir(), "working")
	if err := os.Mkdir(lost, 0o755); err != nil {
		t.Fatal(err)
	}
	t.Chdir(lost)
	if err := os.Remove(lost); err != nil {
		t.Skip("this filesystem will not remove the working directory")
	}
	if _, err := os.Getwd(); err == nil {
		t.Skip("this kernel still names a removed working directory")
	}
	_, err := VerifyCheckout("ra8-firmware")
	if !errors.Is(err, ErrInvalidCheckout) {
		t.Fatalf("a relative root named from a removed directory was accepted: %v", err)
	}
	if strings.Contains(err.Error(), ".git") {
		t.Fatalf("the refusal blames the checkout's contents rather than the path: %v", err)
	}
	// An absolute root never asks the working directory anything, so the same
	// process still gets the refusal the checkout itself earns.
	_, err = VerifyCheckout(filepath.Join(t.TempDir(), "absent"))
	if !errors.Is(err, ErrInvalidCheckout) {
		t.Fatalf("an absolute root was not judged on its own: %v", err)
	}
	// And the empty root keeps its own refusal, which names the root rather
	// than whatever the working directory happens to be.
	_, err = VerifyCheckout("")
	if !errors.Is(err, ErrInvalidCheckout) || !strings.Contains(err.Error(), "repository root is empty") {
		t.Fatalf("an empty root did not name itself: %v", err)
	}
}
