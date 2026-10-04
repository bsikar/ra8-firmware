// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"crypto/rand"
	"crypto/rsa"
	"crypto/tls"
	"crypto/x509"
	"encoding/json"
	"encoding/pem"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/testprivatefile"
)

// appKeyAt writes a well-formed App key where the caller wants it, at the
// permission the caller wants, so the key policy can be tested on a real file.
func appKeyAt(t *testing.T, path string, perm os.FileMode) string {
	t.Helper()
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatalf("generate key: %v", err)
	}
	encoded := pem.EncodeToMemory(&pem.Block{Type: "RSA PRIVATE KEY", Bytes: x509.MarshalPKCS1PrivateKey(key)})
	if err := os.WriteFile(path, encoded, perm); err != nil {
		t.Fatalf("write key: %v", err)
	}
	if err := os.Chmod(path, perm); err != nil {
		t.Fatalf("chmod key: %v", err)
	}
	if perm&0o077 != 0 {
		if err := testprivatefile.OtherUsersReadable(path); err != nil {
			t.Fatalf("make shared-key fixture readable: %v", err)
		}
	}
	return path
}

// The configuration check passes and the key is still the thing that stops the
// reader being built. An operator who mispointed the key must be told that,
// not handed the generic configuration refusal.
func TestAPullRequestReaderIsNotBuiltOnKeyMaterialItCannotLoad(t *testing.T) {
	dir := t.TempDir()
	notPEM := filepath.Join(dir, "not-a-key.pem")
	if err := os.WriteFile(notPEM, []byte("this is not a key\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	empty := filepath.Join(dir, "empty.pem")
	if err := os.WriteFile(empty, nil, 0o600); err != nil {
		t.Fatal(err)
	}
	shared := appKeyAt(t, filepath.Join(dir, "shared.pem"), 0o644)
	directory := filepath.Join(dir, "a-directory.pem")
	if err := os.Mkdir(directory, 0o700); err != nil {
		t.Fatal(err)
	}

	for _, c := range []struct {
		name string
		file string
	}{
		{"a key that is not there", filepath.Join(dir, "absent.pem")},
		{"a file that is not PEM", notPEM},
		{"an empty file", empty},
		{"a key anyone can read", shared},
		{"a directory", directory},
	} {
		t.Run(c.name, func(t *testing.T) {
			reader, err := NewPullRequestHeadReader(PullRequestHeadReaderConfig{
				AppClientID: "Iv1.pullrequesthead", InstallationID: 42, PrivateKeyFile: c.file,
				Owner: "bsikar", Repository: "ra8-firmware",
			})
			if err == nil {
				t.Fatal("a reader was built on key material it cannot load")
			}
			if reader != nil {
				t.Fatalf("refused reader still handed back: %+v", reader)
			}
			if strings.Contains(err.Error(), "invalid GitHub pull request head reader configuration") {
				t.Fatalf("a key failure was reported as a configuration failure: %v", err)
			}
			if !strings.Contains(err.Error(), "private key") {
				t.Fatalf("refusal does not name the key: %v", err)
			}
		})
	}

	if _, err := NewPullRequestHeadReader(PullRequestHeadReaderConfig{
		AppClientID: "Iv1.pullrequesthead", InstallationID: 42,
		PrivateKeyFile: appKeyAt(t, filepath.Join(dir, "good.pem"), 0o600),
		Owner:          "bsikar", Repository: "ra8-firmware",
	}); err != nil {
		t.Fatalf("a usable key was refused: %v", err)
	}
}

// pullRequestReaderOnServer is newPullRequestHeadReader without the built-in
// token mint, so a test can decide what the mint answers.
func pullRequestReaderOnServer(t *testing.T, handler http.HandlerFunc) *PullRequestHeadReader {
	t.Helper()
	keyPath := appKeyAt(t, filepath.Join(t.TempDir(), "app.pem"), 0o600)
	server := httptest.NewTLSServer(handler)
	t.Cleanup(server.Close)
	transport := server.Client().Transport.(*http.Transport).Clone()
	transport.TLSClientConfig = &tls.Config{InsecureSkipVerify: true} //nolint:gosec // deterministic test server
	client := &http.Client{Transport: checkRunTestTransport{inner: transport, host: strings.TrimPrefix(server.URL, "https://")}}
	reader, err := NewPullRequestHeadReader(PullRequestHeadReaderConfig{
		AppClientID: "Iv1.pullrequesthead", InstallationID: 42, PrivateKeyFile: keyPath,
		Owner: "bsikar", Repository: "ra8-firmware", httpClient: client,
	})
	if err != nil {
		t.Fatalf("build reader: %v", err)
	}
	return reader
}

// A token the installation would not mint stops the read there. Reading the
// pull request anyway, unauthenticated, would answer for a public repository
// and quietly report a head nobody was authorised to ask for.
func TestAPullRequestIsNotReadWhenNoTokenWasMinted(t *testing.T) {
	for _, c := range []struct {
		name  string
		serve http.HandlerFunc
	}{
		{"the mint refuses", func(w http.ResponseWriter, r *http.Request) {
			w.WriteHeader(http.StatusForbidden)
		}},
		{"the mint answers with nothing", func(w http.ResponseWriter, r *http.Request) {
			w.WriteHeader(http.StatusCreated)
		}},
		{"the mint answers with an empty token", func(w http.ResponseWriter, r *http.Request) {
			w.WriteHeader(http.StatusCreated)
			_ = json.NewEncoder(w).Encode(installationTokenResponse{Token: ""})
		}},
	} {
		t.Run(c.name, func(t *testing.T) {
			pulls := 0
			reader := pullRequestReaderOnServer(t, func(w http.ResponseWriter, r *http.Request) {
				if strings.HasSuffix(r.URL.Path, "/access_tokens") {
					c.serve(w, r)
					return
				}
				pulls++
				w.WriteHeader(http.StatusOK)
			})
			head, err := reader.Head(context.Background(), 1481)
			if err == nil {
				t.Fatal("a head was reported without a token")
			}
			if head != (PullRequestHead{}) {
				t.Fatalf("head returned beside a refusal: %+v", head)
			}
			if pulls != 0 {
				t.Fatalf("the pull request was read %d times without a token", pulls)
			}
		})
	}
}
