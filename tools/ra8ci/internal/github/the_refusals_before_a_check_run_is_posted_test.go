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
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// A check run cannot be taken back once it is on a commit, so every refusal
// this publisher makes has to happen before the post. check_run_publisher_test.go
// pins the runs GitHub accepts and the output bounds; this pins the refusals
// that never reach a request at all, and the one failure that happens on the
// wire.

// unsteady stands a publisher up against an origin that answers the token
// mint and the post from the handler given, which the recorded-body fixture
// cannot do: it always mints a token and never drops a connection.
func unsteady(t *testing.T, mode CheckRunMode, answer http.HandlerFunc) *CheckRunPublisher {
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
	server := httptest.NewTLSServer(answer)
	t.Cleanup(server.Close)
	transport := server.Client().Transport.(*http.Transport).Clone()
	transport.TLSClientConfig = &tls.Config{InsecureSkipVerify: true} //nolint:gosec // deterministic test server
	client := &http.Client{Transport: checkRunTestTransport{
		inner: transport, host: strings.TrimPrefix(server.URL, "https://"),
	}}
	publisher, err := NewCheckRunPublisher(CheckRunPublisherConfig{
		AppClientID: "Iv1.test", InstallationID: 7, PrivateKeyFile: keyPath,
		Owner: "bsikar", Repository: "ra8-firmware", Mode: mode, httpClient: client,
	})
	if err != nil {
		t.Fatal(err)
	}
	return publisher
}

// A publish nobody could act on is refused before a token is minted, so a
// malformed call costs the App's rate limit nothing and posts nothing.
func TestAnUnpostableCheckRunNeverReachesGitHub(t *testing.T) {
	asked := false
	publisher := unsteady(t, ModeAuthoritative, func(w http.ResponseWriter, _ *http.Request) {
		asked = true
		w.WriteHeader(http.StatusCreated)
		_ = json.NewEncoder(w).Encode(installationTokenResponse{
			Token: "installation-token", ExpiresAt: time.Now().Add(time.Hour),
		})
	})
	sound := TaskCheckRun{
		Name: "ra8ci/shadow/build", HeadSHA: testHeadSHA, Status: "completed",
		Conclusion: "success", Title: "build", Mode: ModeShadow,
	}

	if _, err := publisher.Publish(nil, sound, "held"); err == nil ||
		!strings.Contains(err.Error(), "invalid check run publish request") {
		t.Fatalf("a nil caller was accepted: %v", err)
	}
	var absent *CheckRunPublisher
	if _, err := absent.Publish(context.Background(), sound, "held"); err == nil ||
		!strings.Contains(err.Error(), "invalid check run publish request") {
		t.Fatalf("a publisher that does not exist posted: %v", err)
	}

	// A mode that is neither of the two this plane publishes is refused on
	// its own terms. The type is an integer, so a value off the end of the
	// pair is what a caller built from an unchecked configuration carries,
	// and it must not be rounded down to the shadow one the zero value is.
	for _, mode := range []CheckRunMode{ModeAuthoritative + 1, CheckRunMode(7), CheckRunMode(-1)} {
		run := sound
		run.Mode = mode
		_, err := publisher.Publish(context.Background(), run, "held")
		if err == nil || !errors.Is(err, ErrInvalidCheckRunMode) {
			t.Fatalf("mode %q was accepted: %v", mode, err)
		}
	}
	if asked {
		t.Fatal("GitHub was asked to post a run nobody could name")
	}
}

// A token the App could not mint is the publish's failure, and nothing is
// posted on the commit under a name that would then carry a run this plane
// never wrote.
func TestACheckRunIsNotPostedWithoutAToken(t *testing.T) {
	posted := 0
	refusingToMint := unsteady(t, ModeAuthoritative, func(w http.ResponseWriter, r *http.Request) {
		if strings.HasSuffix(r.URL.Path, "/access_tokens") {
			w.WriteHeader(http.StatusForbidden)
			return
		}
		posted++
		w.WriteHeader(http.StatusCreated)
	})
	run := TaskCheckRun{
		Name: "ra8ci/shadow/build", HeadSHA: testHeadSHA, Status: "completed",
		Conclusion: "success", Title: "build", Mode: ModeShadow,
	}
	id, err := refusingToMint.Publish(context.Background(), run, "held")
	if err == nil {
		t.Fatalf("a publish without a token answered %d", id)
	}
	if id != 0 {
		t.Fatalf("a failed publish carried check run %d", id)
	}
	if posted != 0 {
		t.Fatalf("%d runs were posted without a token", posted)
	}
}

// A post whose connection dropped is reported as the failed post it is. The
// run may or may not exist on the commit, which is exactly why the publisher
// must not answer with an identifier it never read.
func TestAPostThatDroppedIsNotAPublishedRun(t *testing.T) {
	dropped := unsteady(t, ModeAuthoritative, func(w http.ResponseWriter, r *http.Request) {
		if strings.HasSuffix(r.URL.Path, "/access_tokens") {
			w.WriteHeader(http.StatusCreated)
			_ = json.NewEncoder(w).Encode(installationTokenResponse{
				Token: "installation-token", ExpiresAt: time.Now().Add(time.Hour),
			})
			return
		}
		hangUp(w, r)
	})
	run := TaskCheckRun{
		Name: "ra8ci/authoritative/build", HeadSHA: testHeadSHA, Status: "completed",
		Conclusion: "success", Title: "build", Mode: ModeAuthoritative,
	}
	id, err := dropped.Publish(context.Background(), run, "held")
	if err == nil || !strings.Contains(err.Error(), "post check run") {
		t.Fatalf("Publish = %d, %v", id, err)
	}
	if id != 0 {
		t.Fatalf("a dropped post carried check run %d", id)
	}
}

// The App key is read when the publisher is built, so a key that is not a key
// is an operator's configuration error rather than a failure on the first
// commit of the day.
func TestAPublisherIsRefusedWhenItsKeyIsNotAKey(t *testing.T) {
	unreadable := filepath.Join(t.TempDir(), "app.pem")
	if err := os.WriteFile(unreadable, []byte("-----BEGIN RSA PRIVATE KEY-----\nnot a key\n-----END RSA PRIVATE KEY-----\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	for _, keyFile := range []string{unreadable, filepath.Join(t.TempDir(), "absent.pem")} {
		publisher, err := NewCheckRunPublisher(CheckRunPublisherConfig{
			AppClientID: "Iv1.test", InstallationID: 7, PrivateKeyFile: keyFile,
			Owner: "bsikar", Repository: "ra8-firmware", Mode: ModeShadow,
		})
		if err == nil {
			t.Fatalf("key %q built a publisher", keyFile)
		}
		if publisher != nil {
			t.Fatalf("key %q answered with a publisher as well as an error", keyFile)
		}
	}
}
