// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestOpenSessionRejectsInvalidConfigBeforeReadingCredentials(t *testing.T) {
	config := SessionConfig{GitHubConfigURL: "https://github.com/bsikar", AppClientID: "client",
		InstallationID: 1, PrivateKeyFile: filepath.Join(t.TempDir(), "missing.pem"), Owner: "bsikar",
		ScaleSetID: 42, MaxRunners: 2}
	config.Owner = "../attacker"
	if _, err := OpenSession(context.Background(), config); err == nil || !strings.Contains(err.Error(), "configuration") {
		t.Fatalf("invalid owner err=%v", err)
	}
	config.Owner = "bsikar"
	if _, err := OpenSession(nil, config); err == nil || !strings.Contains(err.Error(), "configuration") {
		t.Fatalf("nil context err=%v", err)
	}
}

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

func TestOpenSessionRejectsNonregularPrivateKey(t *testing.T) {
	path := filepath.Join(t.TempDir(), "directory.pem")
	if err := os.Mkdir(path, 0700); err != nil {
		t.Fatal(err)
	}
	config := SessionConfig{GitHubConfigURL: "https://github.com/bsikar", AppClientID: "client",
		InstallationID: 1, PrivateKeyFile: path, Owner: "bsikar", ScaleSetID: 42}
	if _, err := OpenSession(context.Background(), config); err == nil || !strings.Contains(err.Error(), "regular file") {
		t.Fatalf("directory key err=%v", err)
	}
	keyPath := filepath.Join(t.TempDir(), "private.pem")
	if err := os.WriteFile(keyPath, []byte("not-a-key"), 0600); err != nil {
		t.Fatal(err)
	}
	linkPath := filepath.Join(t.TempDir(), "private-link.pem")
	if err := os.Symlink(keyPath, linkPath); err != nil {
		t.Skipf("symlink unavailable: %v", err)
	}
	config.PrivateKeyFile = linkPath
	if _, err := OpenSession(context.Background(), config); err == nil || !strings.Contains(err.Error(), "regular file") {
		t.Fatalf("symlinked key err=%v", err)
	}
}

func TestControllerSessionClosesRemoteSessionAfterRunStops(t *testing.T) {
	client := testClient()
	controller, err := NewController(client, &fakeInbox{}, &testHandler{}, testAdmission{}, 42, 1, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	closed := false
	session := &ControllerSession{controller: controller, session: &Session{Client: client, close: func(context.Context) error {
		closed = true
		return nil
	}}}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if err := session.Run(ctx); !errors.Is(err, context.Canceled) {
		t.Fatalf("run err=%v", err)
	}
	if !closed {
		t.Fatal("remote session was not closed")
	}
}
