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
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// The gate this reader reports decides which check runs #1481's move is
// planned against, so a read that failed must never arrive as a branch
// requiring nothing: that reads as a gate an operator may safely change.
// required_checks_reader_test.go pins what GitHub says; this pins what
// happens when it does not get to say it.

// flaky stands a required-check reader up against an origin that mints the
// token normally and answers the protection read from the handler given,
// which the recorded-body fixture cannot do for a dropped connection.
func flaky(t *testing.T, answer http.HandlerFunc) *RequiredCheckReader {
	t.Helper()
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatal(err)
	}
	keyPath := filepath.Join(t.TempDir(), "app.pem")
	if err := os.WriteFile(keyPath, pem.EncodeToMemory(&pem.Block{
		Type: "RSA PRIVATE KEY", Bytes: x509.MarshalPKCS1PrivateKey(key),
	}), 0o600); err != nil {
		t.Fatal(err)
	}
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if strings.HasSuffix(r.URL.Path, "/access_tokens") {
			w.WriteHeader(http.StatusCreated)
			_ = json.NewEncoder(w).Encode(installationTokenResponse{
				Token: "installation-token", ExpiresAt: time.Now().Add(time.Hour),
			})
			return
		}
		answer(w, r)
	}))
	t.Cleanup(server.Close)
	transport := server.Client().Transport.(*http.Transport).Clone()
	transport.TLSClientConfig = &tls.Config{InsecureSkipVerify: true} //nolint:gosec // deterministic test server
	client := &http.Client{Transport: checkRunTestTransport{
		inner: transport, host: strings.TrimPrefix(server.URL, "https://"),
	}}
	reader, err := NewRequiredCheckReader(RequiredCheckReaderConfig{
		AppClientID: "Iv1.test", InstallationID: 7, PrivateKeyFile: keyPath,
		Owner: "bsikar", Repository: "ra8-firmware", httpClient: client,
	})
	if err != nil {
		t.Fatal(err)
	}
	return reader
}

// A connection that dropped is a failed read, not a branch requiring
// nothing.
func TestAProtectionReadThatNeverArrivedIsNotAnOpenGate(t *testing.T) {
	reader := flaky(t, hangUp)
	contexts, err := reader.RequiredContexts(context.Background(), "main")
	if err == nil || !strings.Contains(err.Error(), "read branch protection") {
		t.Fatalf("RequiredContexts = %v, %v", contexts, err)
	}
	if contexts != nil {
		t.Fatalf("a failed read carried a gate: %v", contexts)
	}

	// A body that stops short of its own Content-Length is the same
	// failure, reported against the protection document rather than
	// decoded into a shorter gate.
	truncated := flaky(t, func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Length", "4096")
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte(`{"required_status_checks":{"checks":[{"context":"buil`))
		if flusher, ok := w.(http.Flusher); ok {
			flusher.Flush()
		}
		hangUp(w, r)
	})
	contexts, err = truncated.RequiredContexts(context.Background(), "main")
	if err == nil || !errors.Is(err, ErrBranchProtectionUnreadable) ||
		!strings.Contains(err.Error(), "unreadable protection document") {
		t.Fatalf("RequiredContexts = %v, %v", contexts, err)
	}
	if contexts != nil {
		t.Fatalf("a truncated document became a gate: %v", contexts)
	}
}

// A protection document past the response bound is refused rather than
// read up to the bound: a gate assembled from the first megabyte of a
// larger answer is a different gate.
func TestAProtectionDocumentPastTheBoundIsRefusedNotTruncated(t *testing.T) {
	// A well-formed document whose checks list runs past the limit: every
	// context in it is real, and the truncation falls inside the array.
	var flood strings.Builder
	flood.WriteString(`{"required_status_checks":{"checks":[`)
	for i := 0; flood.Len() < maxBranchProtectionResponse+1024; i++ {
		if i > 0 {
			flood.WriteString(",")
		}
		fmt.Fprintf(&flood, `{"context":"ra8/check-%06d","app_id":7}`, i)
	}
	flood.WriteString(`]}}`)

	reader := flaky(t, func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte(flood.String()))
	})
	contexts, err := reader.RequiredContexts(context.Background(), "main")
	if err == nil || !errors.Is(err, ErrBranchProtectionUnreadable) ||
		!strings.Contains(err.Error(), "unreadable protection document") {
		t.Fatalf("RequiredContexts = %v, %v", contexts, err)
	}
	if len(contexts) != 0 {
		t.Fatalf("%d contexts were reported from a truncated read", len(contexts))
	}
}

// A request nobody could answer is refused before a token is minted, and a
// reader that does not exist answers rather than crashing the command.
func TestAnUnanswerableProtectionRequestNeverReachesGitHub(t *testing.T) {
	asked := false
	reader := flaky(t, func(w http.ResponseWriter, _ *http.Request) {
		asked = true
		_, _ = w.Write([]byte(`{"required_status_checks":{"checks":[]}}`))
	})
	if _, err := reader.RequiredContexts(nil, "main"); err == nil ||
		!strings.Contains(err.Error(), "invalid required check read request") {
		t.Fatalf("a nil caller was accepted: %v", err)
	}
	var absent *RequiredCheckReader
	if _, err := absent.RequiredContexts(context.Background(), "main"); err == nil ||
		!strings.Contains(err.Error(), "invalid required check read request") {
		t.Fatalf("a reader that does not exist answered: %v", err)
	}
	if asked {
		t.Fatal("GitHub was asked about a branch nobody could read")
	}

	// The branch-name rule is exact at its bound on the accepting side,
	// which is what keeps the refusal above from reading as a length limit
	// nobody can meet.
	if _, err := reader.RequiredContexts(context.Background(), strings.Repeat("a", 250)); err != nil {
		t.Fatalf("a 250-character branch was refused: %v", err)
	}
	if !asked {
		t.Fatal("a branch this reader can state plainly was never asked about")
	}
}
