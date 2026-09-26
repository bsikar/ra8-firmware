// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"crypto/ed25519"
	"crypto/rand"
	"encoding/base64"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// publicKeyInDirectory writes a real base64 Ed25519 public key into its own
// subdirectory held at directoryMode. The key file itself stays 0600, so a
// refusal can only come from the directory rule under test.
func publicKeyInDirectory(t *testing.T, directoryMode os.FileMode) string {
	t.Helper()
	directory := filepath.Join(t.TempDir(), "public")
	if err := os.Mkdir(directory, 0o700); err != nil {
		t.Fatal(err)
	}
	publicKey, _, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(directory, "backup.pub")
	if err := os.WriteFile(path, []byte(base64.StdEncoding.EncodeToString(publicKey)+"\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	// Chmod after the write: the create mode is masked by the process umask,
	// and a non-writable directory would refuse the write itself.
	if err := os.Chmod(directory, directoryMode); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(directory, 0o700) })
	return path
}

func TestProtectedPublicKeyDirectoryIsStillAccepted(t *testing.T) {
	key, err := LoadBackupPublicKey(publicKeyInDirectory(t, 0o755))
	if err != nil || len(key) != ed25519.PublicKeySize {
		t.Fatalf("protected public key directory was refused: %v", err)
	}
}

// The API service reading a key out of a directory it owns is ordinary.
func TestOwnerWritablePublicKeyDirectoryIsStillAccepted(t *testing.T) {
	key, err := LoadBackupPublicKey(publicKeyInDirectory(t, 0o700))
	if err != nil || len(key) != ed25519.PublicKeySize {
		t.Fatalf("owner-writable public key directory was refused: %v", err)
	}
}

func TestGroupWritablePublicKeyDirectoryIsRefused(t *testing.T) {
	key, err := LoadBackupPublicKey(publicKeyInDirectory(t, 0o770))
	if err == nil {
		t.Fatal("group-writable public key directory was accepted")
	}
	if key != nil {
		t.Fatal("a refused load returned key material")
	}
	if !strings.Contains(err.Error(), "public key directory must not be group or world writable") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}

func TestWorldWritablePublicKeyDirectoryIsRefused(t *testing.T) {
	_, err := LoadBackupPublicKey(publicKeyInDirectory(t, 0o757))
	if err == nil {
		t.Fatal("world-writable public key directory was accepted")
	}
	if !strings.Contains(err.Error(), "public key directory must not be group or world writable") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}

// Same call as every other input in this file: the sticky bit buys no
// exception, so a future fire that wants one changes it deliberately.
func TestStickyWorldWritablePublicKeyDirectoryIsRefused(t *testing.T) {
	if _, err := LoadBackupPublicKey(publicKeyInDirectory(t, os.ModeSticky|0o777)); err == nil {
		t.Fatal("sticky world-writable public key directory was accepted")
	}
}

func TestSymlinkedPublicKeyDirectoryIsRefused(t *testing.T) {
	path := publicKeyInDirectory(t, 0o755)
	real := filepath.Dir(path)
	link := filepath.Join(filepath.Dir(real), "public-link")
	if err := os.Symlink(real, link); err != nil {
		t.Fatal(err)
	}
	_, err := LoadBackupPublicKey(filepath.Join(link, filepath.Base(path)))
	if err == nil {
		t.Fatal("symlinked public key directory was accepted")
	}
	if !strings.Contains(err.Error(), "public key directory must be a real directory") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}

func TestMissingPublicKeyDirectoryNamesTheDirectory(t *testing.T) {
	path := filepath.Join(t.TempDir(), "absent", "backup.pub")
	_, err := LoadBackupPublicKey(path)
	if err == nil {
		t.Fatal("a public key under a missing directory was accepted")
	}
	if !strings.Contains(err.Error(), "public key directory must be a real directory") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}

// A writable key file keeps naming the file when the directory is protected.
func TestWritablePublicKeyStillNamesTheKeyFile(t *testing.T) {
	path := publicKeyInDirectory(t, 0o755)
	if err := os.Chmod(path, 0o666); err != nil {
		t.Fatal(err)
	}
	_, err := LoadBackupPublicKey(path)
	if err == nil {
		t.Fatal("a world-writable public key was accepted")
	}
	if !strings.Contains(err.Error(), "bounded non-writable regular file") {
		t.Fatalf("the pre-existing rule no longer names its own reason: %v", err)
	}
}

// The absolute-path rule is judged before anything is stat'd.
func TestRelativePublicKeyPathStillNamesTheRule(t *testing.T) {
	_, err := LoadBackupPublicKey("backup.pub")
	if err == nil {
		t.Fatal("a relative public key path was accepted")
	}
	if !strings.Contains(err.Error(), "must be absolute") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}
