// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// plantedCapture writes a capture file that passes every shape check
// hilVerifyCaptureCommand makes before it opens anything, so a case can aim
// at exactly one arm past that point.
func plantedCapture(t *testing.T) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "capture.txt")
	if err := os.WriteFile(path, []byte("ra8: boot ok\n"), 0o600); err != nil {
		t.Fatalf("plant capture: %v", err)
	}
	return path
}

// The shape checks in the_hil_commands_refuse_before_they_measure_test.go
// take a capture that is not a plain file under the bound. This is the arm
// after them: a capture whose advertised shape is perfect and which still
// cannot be opened. Lstat reports a regular file of a sane size for a 0000
// capture, because Lstat answers about the file, not about this process's
// access to it.
//
// The two refusals have to stay distinguishable. "capture must be a regular
// file no larger than 8 MiB" tells an operator to point at a different file;
// "open HIL capture" tells them the file is the right one and the permissions
// are not, and it carries the underlying error so they can see which.
func TestHILVerifyCaptureRefusesACaptureItCannotOpen(t *testing.T) {
	if os.Geteuid() == 0 {
		t.Skip("running as root, which ignores the permission bits under test")
	}
	capture := plantedCapture(t)
	if err := os.Chmod(capture, 0o000); err != nil {
		t.Fatalf("seal capture: %v", err)
	}
	t.Cleanup(func() { _ = os.Chmod(capture, 0o600) })

	err := hilVerifyCaptureCommand(context.Background(), []string{
		"--manifest", "examples/does-not-matter/hil.conf", "--capture", capture,
	})
	if err == nil {
		t.Fatal("an unopenable capture was accepted")
	}
	if !strings.HasPrefix(err.Error(), "open HIL capture:") {
		t.Fatalf("refusal = %q, want it to name the open and carry the reason", err)
	}
}

// The manifest is resolved against the checkout, so a command run outside one
// has nothing to resolve it against. The order is what this pins: the capture
// is opened and re-stated first, and only then is the checkout looked for, so
// an operator who is simply standing in the wrong directory is told that
// rather than told something about their capture file.
func TestHILVerifyCaptureRefusesWhenItIsRunOutsideACheckout(t *testing.T) {
	capture := plantedCapture(t)
	elsewhere := t.TempDir()
	if _, err := os.Stat(filepath.Join(elsewhere, ".git")); err == nil {
		t.Skip("temporary directory unexpectedly sits inside a checkout")
	}
	t.Chdir(elsewhere)

	err := hilVerifyCaptureCommand(context.Background(), []string{
		"--manifest", "examples/does-not-matter/hil.conf", "--capture", capture,
	})
	if err == nil {
		t.Fatal("a capture verification outside a checkout was accepted")
	}
	if err.Error() != "no repository checkout found" {
		t.Fatalf("refusal = %q, want the missing checkout", err)
	}
}

// verifyHILCapture is reachable on its own and is the half the command shares
// with anything else that verifies a capture, so its own input guard is worth
// stating separately from the command's flag parsing. A root that is not
// exactly its own trimmed self is the interesting one: a path carrying a
// trailing newline reads as a directory name in every message it appears in
// and resolves as something else entirely, so it is refused rather than
// trimmed and used.
func TestVerifyHILCaptureRefusesAnInputItCannotWorkFrom(t *testing.T) {
	sound := strings.NewReader("ra8: boot ok\n")
	for name, call := range map[string]func() error{
		"no root": func() error {
			_, err := verifyHILCapture("", "examples/a/hil.conf", sound)
			return err
		},
		"root with a leading space": func() error {
			_, err := verifyHILCapture(" /checkout", "examples/a/hil.conf", sound)
			return err
		},
		"root with a trailing space": func() error {
			_, err := verifyHILCapture("/checkout ", "examples/a/hil.conf", sound)
			return err
		},
		"root with a trailing newline": func() error {
			_, err := verifyHILCapture("/checkout\n", "examples/a/hil.conf", sound)
			return err
		},
		"root that is only space": func() error {
			_, err := verifyHILCapture("   ", "examples/a/hil.conf", sound)
			return err
		},
		"no capture to read": func() error {
			_, err := verifyHILCapture("/checkout", "examples/a/hil.conf", nil)
			return err
		},
	} {
		t.Run(name, func(t *testing.T) {
			err := call()
			if err == nil {
				t.Fatal("an unworkable input was accepted")
			}
			if err.Error() != "invalid HIL capture verification input" {
				t.Fatalf("refusal = %q, want the invalid input", err)
			}
		})
	}
}
