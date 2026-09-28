// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"crypto/rand"
	"crypto/rsa"
	"crypto/tls"
	"crypto/x509"
	"encoding/pem"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// Both of these readers load the App key when they are built and mint a token
// on every read. A configuration that cannot work must fail at the build, in
// front of the operator who wrote it, and a token the App could not mint must
// fail the read rather than answer as a commit with nothing on it: a run with
// no outcomes and a branch with no required checks are both answers that would
// let a gate through.

// badKeyFiles returns a PEM that is not a key and a path with no file at it.
func badKeyFiles(t *testing.T) []string {
	t.Helper()
	unreadable := filepath.Join(t.TempDir(), "app.pem")
	if err := os.WriteFile(unreadable, []byte(
		"-----BEGIN RSA PRIVATE KEY-----\nnot a key\n-----END RSA PRIVATE KEY-----\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	return []string{unreadable, filepath.Join(t.TempDir(), "absent.pem")}
}

// soundKeyFile writes an App key a constructor will accept.
func soundKeyFile(t *testing.T) string {
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
	return keyPath
}

// A reader whose configuration cannot work is refused when it is built, never
// on the first commit of the day.
func TestAnUnworkableReaderConfigurationIsRefusedAtTheBuild(t *testing.T) {
	sound := soundKeyFile(t)
	for _, origin := range []string{"http://api.github.com", "api.github.com", "https://api.github.com/../x", ":"} {
		if _, err := NewActionsOutcomeReader(ActionsOutcomeReaderConfig{
			APIBaseURL: origin, AppClientID: "Iv1.test", InstallationID: 7,
			PrivateKeyFile: sound, Owner: "bsikar", Repository: "ra8-firmware",
		}); err == nil {
			t.Fatalf("outcome reader accepted origin %q", origin)
		}
		if _, err := NewCheckRunReconciler(CheckRunReconcilerConfig{
			APIBaseURL: origin, AppClientID: "Iv1.test", InstallationID: 7,
			PrivateKeyFile: sound, Owner: "bsikar", Repository: "ra8-firmware",
		}); err == nil {
			t.Fatalf("reconciler accepted origin %q", origin)
		}
	}

	for _, keyFile := range badKeyFiles(t) {
		reader, err := NewActionsOutcomeReader(ActionsOutcomeReaderConfig{
			AppClientID: "Iv1.test", InstallationID: 7, PrivateKeyFile: keyFile,
			Owner: "bsikar", Repository: "ra8-firmware",
		})
		if err == nil || reader != nil {
			t.Fatalf("outcome reader built on key %q: %v", keyFile, err)
		}
		reconciler, err := NewCheckRunReconciler(CheckRunReconcilerConfig{
			AppClientID: "Iv1.test", InstallationID: 7, PrivateKeyFile: keyFile,
			Owner: "bsikar", Repository: "ra8-firmware",
		})
		if err == nil || reconciler != nil {
			t.Fatalf("reconciler built on key %q: %v", keyFile, err)
		}
	}

	// The identifying fields are refused one at a time, so a typo in any
	// one of them is named at the build rather than carried into a read.
	for _, broken := range []ActionsOutcomeReaderConfig{
		{InstallationID: 7, PrivateKeyFile: sound, Owner: "bsikar", Repository: "ra8-firmware"},
		{AppClientID: strings.Repeat("i", 257), InstallationID: 7, PrivateKeyFile: sound, Owner: "bsikar", Repository: "ra8-firmware"},
		{AppClientID: "Iv1.test", PrivateKeyFile: sound, Owner: "bsikar", Repository: "ra8-firmware"},
		{AppClientID: "Iv1.test", InstallationID: -1, PrivateKeyFile: sound, Owner: "bsikar", Repository: "ra8-firmware"},
		{AppClientID: "Iv1.test", InstallationID: 7, Owner: "bsikar", Repository: "ra8-firmware"},
		{AppClientID: "Iv1.test", InstallationID: 7, PrivateKeyFile: sound, Owner: "bsikar/ra8", Repository: "ra8-firmware"},
		{AppClientID: "Iv1.test", InstallationID: 7, PrivateKeyFile: sound, Owner: "bsikar", Repository: "ra8 firmware"},
		{AppClientID: "Iv1.test", InstallationID: 7, PrivateKeyFile: sound, Owner: "", Repository: ""},
	} {
		if _, err := NewActionsOutcomeReader(broken); err == nil {
			t.Fatalf("outcome reader accepted %+v", broken)
		}
		if _, err := NewCheckRunReconciler(CheckRunReconcilerConfig{
			APIBaseURL: broken.APIBaseURL, AppClientID: broken.AppClientID,
			InstallationID: broken.InstallationID, PrivateKeyFile: broken.PrivateKeyFile,
			Owner: broken.Owner, Repository: broken.Repository,
		}); err == nil {
			t.Fatalf("reconciler accepted %+v", broken)
		}
	}
}

// A token the App could not mint fails the read. Neither reader may answer
// with the empty document, because a run with no outcomes and a branch with
// no required checks each read as a gate with nothing to satisfy.
//
// The handler-taking fixtures in this package answer the mint themselves, so
// this stands the three readers up against one origin that refuses it. Any
// path other than the mint means a read was attempted without a token.
func TestATokenTheAppCouldNotMintIsNotAnEmptyAnswer(t *testing.T) {
	keyPath := soundKeyFile(t)
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !strings.HasSuffix(r.URL.Path, "/access_tokens") {
			t.Errorf("a read of %s was made without a token", r.URL.Path)
		}
		w.WriteHeader(http.StatusUnauthorized)
	}))
	t.Cleanup(server.Close)
	transport := server.Client().Transport.(*http.Transport).Clone()
	transport.TLSClientConfig = &tls.Config{InsecureSkipVerify: true} //nolint:gosec // deterministic test server
	client := &http.Client{Transport: checkRunTestTransport{
		inner: transport, host: strings.TrimPrefix(server.URL, "https://"),
	}}

	outcomeReader, err := NewActionsOutcomeReader(ActionsOutcomeReaderConfig{
		AppClientID: "Iv1.test", InstallationID: 7, PrivateKeyFile: keyPath,
		Owner: "bsikar", Repository: "ra8-firmware", httpClient: client,
	})
	if err != nil {
		t.Fatal(err)
	}
	outcomes, err := outcomeReader.Outcomes(context.Background(), 4429117744)
	if err == nil {
		t.Fatalf("Outcomes answered %+v without a token", outcomes)
	}
	if outcomes.RunID != 0 || len(outcomes.Outcomes) != 0 || outcomes.HeadSHA != "" {
		t.Fatalf("a tokenless read carried a run: %+v", outcomes)
	}

	reconciler, err := NewCheckRunReconciler(CheckRunReconcilerConfig{
		AppClientID: "Iv1.test", InstallationID: 7, PrivateKeyFile: keyPath,
		Owner: "bsikar", Repository: "ra8-firmware", httpClient: client,
	})
	if err != nil {
		t.Fatal(err)
	}
	runs, err := reconciler.PublishedRuns(context.Background(), actionsRunHead)
	if err == nil {
		t.Fatalf("PublishedRuns answered %+v without a token", runs)
	}
	if runs.HeadSHA != "" || len(runs.Runs) != 0 {
		t.Fatalf("a tokenless listing carried a commit: %+v", runs)
	}

	checkReader, err := NewRequiredCheckReader(RequiredCheckReaderConfig{
		AppClientID: "Iv1.test", InstallationID: 7, PrivateKeyFile: keyPath,
		Owner: "bsikar", Repository: "ra8-firmware", httpClient: client,
	})
	if err != nil {
		t.Fatal(err)
	}
	contexts, err := checkReader.RequiredContexts(context.Background(), "ra8ci/dev")
	if err == nil {
		t.Fatalf("RequiredContexts answered %+v without a token", contexts)
	}
	if len(contexts) != 0 {
		t.Fatalf("a tokenless protection read carried a gate: %+v", contexts)
	}
}
