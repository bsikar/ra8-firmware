// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The attestation is what the gate believes, so where it is published is as
// much a part of the argument as what it says. These hold the directory rule
// the publish is held to, the atomic replace itself, and the bound on the
// monitor's own output buffer.

func publishedInto(t *testing.T, directory string) (string, error) {
	t.Helper()
	path := filepath.Join(directory, "backup.json")
	return path, writeBackupAttestation(path, []byte(`{"schema_version":1}`))
}

func TestPublishingAnAttestationNeedsAProtectedRealDirectory(t *testing.T) {
	root := t.TempDir()
	plainFile := filepath.Join(root, "not-a-directory")
	if err := os.WriteFile(plainFile, []byte("x"), 0o640); err != nil {
		t.Fatal(err)
	}
	link := filepath.Join(root, "linked")
	realDirectory := filepath.Join(root, "real")
	if err := os.Mkdir(realDirectory, 0o750); err != nil {
		t.Fatal(err)
	}
	symlinkTest(t, realDirectory, link)
	for name, directory := range map[string]string{
		"a directory that is not there": filepath.Join(root, "absent"),
		"a file standing in for one":    plainFile,
		"a symlink to a real one":       link,
	} {
		if _, err := publishedInto(t, directory); err == nil ||
			!strings.Contains(err.Error(), "protected real directory") {
			t.Fatalf("%s = %v", name, err)
		}
	}
}

// The publish is a rename, so a reader either sees the previous attestation
// or the new one, never a half-written file, and the mode is set before any
// bytes are written rather than after.
func TestPublishingAnAttestationReplacesWhatWasThereAtOnce(t *testing.T) {
	directory := filepath.Join(t.TempDir(), "out")
	if err := os.Mkdir(directory, 0o750); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(directory, "backup.json")
	if err := os.WriteFile(path, []byte("an older attestation"), 0o640); err != nil {
		t.Fatal(err)
	}
	fresh := []byte(`{"schema_version":1,"approval_id":"018d1234-5678-7abc-8def-123456789abc"}`)
	if err := writeBackupAttestation(path, fresh); err != nil {
		t.Fatalf("publish: %v", err)
	}
	published, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if string(published) != string(fresh) {
		t.Fatalf("published %q, want the fresh attestation", published)
	}
	info, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}
	if info.Mode().Perm() != 0o640 {
		t.Fatalf("published mode %v, want 0640", info.Mode().Perm())
	}
	left, err := os.ReadDir(directory)
	if err != nil {
		t.Fatal(err)
	}
	if len(left) != 1 {
		t.Fatalf("directory holds %d entries, want only the attestation", len(left))
	}
}

// A path already held by a directory cannot be renamed over, and the failure
// says which step could not be taken rather than reading as a write error.
func TestPublishingOverADirectoryFails(t *testing.T) {
	directory := filepath.Join(t.TempDir(), "out")
	if err := os.Mkdir(directory, 0o750); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(directory, "backup.json")
	if err := os.Mkdir(path, 0o750); err != nil {
		t.Fatal(err)
	}
	err := writeBackupAttestation(path, []byte(`{"schema_version":1}`))
	if err == nil || !strings.Contains(err.Error(), "publish backup attestation") {
		t.Fatalf("publish over a directory = %v", err)
	}
	left, err := os.ReadDir(directory)
	if err != nil {
		t.Fatal(err)
	}
	if len(left) != 1 {
		t.Fatalf("a failed publish left %d entries behind, want only the directory", len(left))
	}
}

// The buffer the monitor collects command output into is bounded so a
// backup tool printing without end cannot be read into memory unchecked.
func TestTheMonitorOutputBufferHoldsItsLimit(t *testing.T) {
	buffer := &boundedBackupBuffer{limit: 8}
	if n, err := buffer.Write([]byte("1234")); err != nil || n != 4 {
		t.Fatalf("first write = %d, %v", n, err)
	}
	if n, err := buffer.Write([]byte("5678")); err != nil || n != 4 {
		t.Fatalf("a write filling the limit exactly = %d, %v", n, err)
	}
	if string(buffer.data) != "12345678" {
		t.Fatalf("buffer holds %q", buffer.data)
	}
	n, err := buffer.Write([]byte("9"))
	if err == nil || !strings.Contains(err.Error(), "exceeds limit") {
		t.Fatalf("a write past the limit = %d, %v", n, err)
	}
	if n != 0 {
		t.Fatalf("a refused write reported %d bytes taken", n)
	}
	if string(buffer.data) != "12345678" {
		t.Fatalf("a refused write changed the buffer to %q", buffer.data)
	}
	if n, err := buffer.Write(nil); err != nil || n != 0 {
		t.Fatalf("an empty write at the limit = %d, %v", n, err)
	}
	full := &boundedBackupBuffer{limit: 0}
	if _, err := full.Write([]byte("x")); err == nil {
		t.Fatal("a buffer with no room took a byte")
	}
}
