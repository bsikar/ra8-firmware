// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"errors"
	"net/http"
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

func TestValidateGitHubConfigURL(t *testing.T) {
	tests := []struct {
		name string
		url  string
		want bool
	}{
		{name: "organization", url: "https://github.com/bsikar", want: true},
		{name: "repository", url: "https://github.com/bsikar/ra8-firmware", want: true},
		{name: "case insensitive owner", url: "https://github.com/BSIKAR/ra8-firmware", want: true},
		{name: "wrong owner", url: "https://github.com/other/ra8-firmware"},
		{name: "http", url: "http://github.com/bsikar"},
		{name: "alternate host", url: "https://github.com.evil.invalid/bsikar"},
		{name: "explicit port", url: "https://github.com:443/bsikar"},
		{name: "userinfo", url: "https://user@github.com/bsikar"},
		{name: "query", url: "https://github.com/bsikar?next=evil"},
		{name: "fragment", url: "https://github.com/bsikar#evil"},
		{name: "encoded path", url: "https://github.com/%62sikar"},
		{name: "trailing slash", url: "https://github.com/bsikar/"},
		{name: "extra path", url: "https://github.com/bsikar/ra8-firmware/issues"},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			err := validateGitHubConfigURL(test.url, "bsikar")
			if (err == nil) != test.want {
				t.Fatalf("validateGitHubConfigURL(%q) err=%v; want success=%v", test.url, err, test.want)
			}
		})
	}
}

func TestRejectGitHubEnterpriseOverride(t *testing.T) {
	t.Setenv("GITHUB_ACTIONS_FORCE_GHES", "")
	if err := rejectGitHubEnterpriseOverride(); err == nil {
		t.Fatal("GHES override environment variable was accepted")
	}
}

func TestNoGitHubProxyValidatesDirectRequestDestinations(t *testing.T) {
	tests := []struct {
		name string
		url  string
		host string
		want bool
	}{
		{name: "github api", url: "https://api.github.com/app", want: true},
		{name: "github config", url: "https://github.com/bsikar", want: true},
		{name: "actions service", url: "https://pipelines.actions.githubusercontent.com/queue", want: true},
		{name: "actions queue port 443", url: "https://queue.actions.githubusercontent.com:443/messages", want: true},
		{name: "unapproved host", url: "https://evil.example.invalid/", want: false},
		{name: "suffix lookalike", url: "https://actions.githubusercontent.com.evil.invalid/", want: false},
		{name: "nonstandard port", url: "https://api.github.com:8443/app", want: false},
		{name: "insecure scheme", url: "http://api.github.com/app", want: false},
		{name: "mismatched Host override", url: "https://api.github.com/app", host: "evil.example.invalid", want: false},
		{name: "userinfo", url: "https://user@api.github.com/app", want: false},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			request, err := http.NewRequest(http.MethodGet, test.url, nil)
			if err != nil {
				t.Fatal(err)
			}
			request.Host = test.host
			proxy, err := noGitHubProxy(request)
			if (err == nil) != test.want {
				t.Fatalf("noGitHubProxy(%q) err=%v; want success=%v", test.url, err, test.want)
			}
			if test.want && proxy != nil {
				t.Fatalf("approved request unexpectedly uses proxy %s", proxy)
			}
		})
	}
}

func TestNoGitHubProxyRejectsNilRequest(t *testing.T) {
	if _, err := noGitHubProxy(nil); err == nil {
		t.Fatal("nil request was accepted")
	}
}
