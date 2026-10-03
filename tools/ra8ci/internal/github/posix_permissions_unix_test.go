//go:build unix

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	context "context"
	os "os"
	filepath "path/filepath"
	strings "strings"
	testing "testing"
)

func TestOpenSessionRejectsGroupAccessiblePrivateKeyBeforeNetwork(t *testing.T) {
	path := filepath.Join(t.TempDir(), "app.pem")
	if err := os.WriteFile(path, []byte("not-a-private-key"), 0644); err != nil {
		t.Fatal(err)
	}
	config := SessionConfig{GitHubConfigURL: "https://github.com/bsikar", AppClientID: "client",
		InstallationID: 1, PrivateKeyFile: path, Owner: "bsikar", ScaleSetID: 42, MaxRunners: 2}
	if _, err := OpenSession(context.Background(), config); err == nil || !strings.Contains(err.Error(), "group or others") {
		t.Fatalf("permissive key mode err=%v", err)
	}
}

// A key file can be a bounded regular file readable by nobody but its owner
// and still refuse to open. Mode 0o000 satisfies every check made against the
// metadata, so the refusal has to come from the open itself. An operator who
// sealed a key and forgot needs to be told the file could not be read, not
// that GitHub rejected the credential.
func TestAPrivateKeyThatCannotBeOpenedIsRefusedBeforeTheCredential(t *testing.T) {
	config := openable(t, []byte("placeholder key material"))
	if err := os.Chmod(config.PrivateKeyFile, 0o000); err != nil {
		t.Fatal(err)
	}
	if readable, err := os.ReadFile(config.PrivateKeyFile); err == nil {
		t.Skipf("this process reads a mode 0000 file (%d bytes); the seal proves nothing here", len(readable))
	}

	_, err := OpenSession(context.Background(), config)
	if err == nil || !strings.Contains(err.Error(), "read GitHub App private key") {
		t.Fatalf("a sealed key file: %v", err)
	}
	// Every metadata check passed, so neither of their refusals is what
	// answered here.
	if strings.Contains(err.Error(), "bounded regular file") || strings.Contains(err.Error(), "group or others") {
		t.Fatalf("the open was blamed on the file's metadata: %v", err)
	}
}

// A key file that passes every check on its metadata and still cannot be
// opened is reported as the failed open it was, naming the file, rather than
// as a key that is not private or not PEM.
func TestAKeyFileThatCannotBeOpenedIsNamedAsAFailedOpen(t *testing.T) {
	sealed := soundKeyFile(t)
	if _, err := loadAppPrivateKey(sealed); err != nil {
		t.Fatalf("a sound key file was refused: %v", err)
	}
	if err := os.Chmod(sealed, 0o000); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(sealed, 0o600) })

	key, err := loadAppPrivateKey(sealed)
	if err == nil {
		t.Fatal("an unopenable key file loaded a key")
	}
	if !strings.Contains(err.Error(), "open GitHub App private key") {
		t.Fatalf("an unopenable key file answered %v", err)
	}
	if key != nil {
		t.Fatal("an unopenable key file answered a key as well as an error")
	}
}
