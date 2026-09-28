// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"encoding/base64"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
)

// This key decides which signatures the gate will accept, so the file it is
// read from is held to a rule of its own. The directory half is pinned in
// backup_monitor_public_key_directory_test.go; these hold the file half and
// what the bytes in it have to decode to.

// keyFile writes contents into a protected directory at 0600 and hands back
// the path, so any refusal comes from the file rule under test.
func keyFile(t *testing.T, contents []byte) string {
	t.Helper()
	directory := filepath.Join(t.TempDir(), "public")
	if err := os.Mkdir(directory, 0o700); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(directory, "backup.pub")
	if err := os.WriteFile(path, contents, 0o600); err != nil {
		t.Fatal(err)
	}
	return path
}

func encodedKey(t *testing.T) []byte {
	t.Helper()
	public, _, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	return []byte(base64.StdEncoding.EncodeToString(public))
}

// Everything that is not a bounded regular file is refused before a byte is
// read, and each of these is a way an operator can end up with something at
// the key path that is not the key.
func TestTheBackupKeyMustBeABoundedRegularFile(t *testing.T) {
	directory := filepath.Join(t.TempDir(), "public")
	if err := os.Mkdir(directory, 0o700); err != nil {
		t.Fatal(err)
	}
	standingIn := filepath.Join(directory, "backup.pub")
	if err := os.Mkdir(standingIn, 0o700); err != nil {
		t.Fatal(err)
	}

	pipeDirectory := filepath.Join(t.TempDir(), "public")
	if err := os.Mkdir(pipeDirectory, 0o700); err != nil {
		t.Fatal(err)
	}
	pipe := filepath.Join(pipeDirectory, "backup.pub")
	if err := syscall.Mkfifo(pipe, 0o600); err != nil {
		t.Skipf("named pipes unavailable here: %v", err)
	}

	real := keyFile(t, encodedKey(t))
	linked := filepath.Join(filepath.Dir(real), "linked.pub")
	if err := os.Symlink(real, linked); err != nil {
		t.Skipf("symlinks unavailable here: %v", err)
	}

	empty := keyFile(t, nil)
	oversized := keyFile(t, []byte(strings.Repeat("A", 257)))

	for name, path := range map[string]string{
		"a directory standing in for the key": standingIn,
		"a named pipe at the key path":        pipe,
		"a symlink pointing at the real key":  linked,
		"an empty file":                       empty,
		"a file past the size bound":          oversized,
	} {
		_, err := LoadBackupPublicKey(path)
		if err == nil || !strings.Contains(err.Error(), "bounded non-writable regular file") {
			t.Fatalf("%s = %v", name, err)
		}
	}
}

// The size bound is inclusive, which is only visible when a file sitting
// exactly on it gets past the stat and is refused for what it says instead.
func TestAKeyFileExactlyOnTheSizeBoundIsStillRead(t *testing.T) {
	path := keyFile(t, []byte(strings.Repeat("A", 256)))
	info, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}
	if info.Size() != 256 {
		t.Fatalf("fixture is %d bytes, want the bound exactly", info.Size())
	}
	_, err = LoadBackupPublicKey(path)
	if err == nil || !strings.Contains(err.Error(), "malformed") {
		t.Fatalf("a key on the bound = %v, want it read and refused for its content", err)
	}
}

// What the bytes decode to is the second half of the rule: the gate would
// otherwise trust a key of the wrong length or one that never decoded.
func TestAKeyThatDoesNotDecodeToAnEd25519PublicKeyIsRefused(t *testing.T) {
	short := make([]byte, ed25519.PublicKeySize-1)
	long := make([]byte, ed25519.PublicKeySize+1)
	for name, contents := range map[string][]byte{
		"not base64 at all":           []byte("this is not a key"),
		"base64 with a stray symbol":  []byte("AAAA$AAA"),
		"a key one byte short":        []byte(base64.StdEncoding.EncodeToString(short)),
		"a key one byte long":         []byte(base64.StdEncoding.EncodeToString(long)),
		"non-canonical base64 tail":   []byte("AAAB"),
		"whitespace and nothing else": []byte("   \n\t  "),
	} {
		_, err := LoadBackupPublicKey(keyFile(t, contents))
		if err == nil || !strings.Contains(err.Error(), "malformed") {
			t.Fatalf("%s = %v", name, err)
		}
	}
}

// An operator's editor leaves a trailing newline and that must not change
// which key the gate trusts.
func TestAKeyIsReadThroughSurroundingWhitespace(t *testing.T) {
	encoded := encodedKey(t)
	plain, err := LoadBackupPublicKey(keyFile(t, encoded))
	if err != nil {
		t.Fatalf("a bare key = %v", err)
	}
	padded, err := LoadBackupPublicKey(keyFile(t, append(append([]byte("\n  "), encoded...), '\n')))
	if err != nil {
		t.Fatalf("a key with surrounding whitespace = %v", err)
	}
	if !plain.Equal(padded) {
		t.Fatal("the same key read two ways gave two different keys")
	}
	if len(plain) != ed25519.PublicKeySize {
		t.Fatalf("key is %d bytes", len(plain))
	}
}

// silentAdmin is enough to satisfy the seam; the constructor never calls it.
type silentAdmin struct{}

func (silentAdmin) RunnerByID(context.Context, int) (github.RunnerIdentity, bool, error) {
	return github.RunnerIdentity{}, false, nil
}

func (silentAdmin) RemoveRunner(context.Context, int) error { return nil }

// An observer built without a scale set would deregister runners it cannot
// scope, so the constructor refuses rather than defaulting.
func TestBuildingARunnerObserverRequiresAScopedAdmin(t *testing.T) {
	for name, scaleSetID := range map[string]int64{
		"no scale set":       0,
		"a negative one":     -42,
		"an absurd negative": -1 << 40,
	} {
		if _, err := NewGitHubRunnerObserver(scaleSetID, silentAdmin{}); err == nil ||
			!strings.Contains(err.Error(), "positive scale-set ID and admin client") {
			t.Fatalf("%s = %v", name, err)
		}
	}
	if _, err := NewGitHubRunnerObserver(42, nil); err == nil {
		t.Fatal("an observer was built with no admin client")
	}
	observer, err := NewGitHubRunnerObserver(42, silentAdmin{})
	if err != nil {
		t.Fatal(err)
	}
	if observer.scaleSetID != 42 || observer.admin == nil {
		t.Fatalf("observer built as %+v", observer)
	}
}
