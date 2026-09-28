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

// An empty job list and a read that never arrived look the same to the
// comparison downstream: both produce no outcomes. They must never be
// reported the same way, because a run collected as no jobs grades every
// covered task indeterminate, which reads as "nobody judged this" rather
// than "the read failed". actions_outcome_reader_test.go pins the refusals
// GitHub states in a status code; this pins the ones it never gets to say.

// brittle stands a reader up against an origin whose answers to everything
// but the token mint are written by the handler given here.
func brittle(t *testing.T, answer http.HandlerFunc) *ActionsOutcomeReader {
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
	reader, err := NewActionsOutcomeReader(ActionsOutcomeReaderConfig{
		AppClientID: "Iv1.test", InstallationID: 7, PrivateKeyFile: keyPath,
		Owner: "bsikar", Repository: "ra8-firmware", httpClient: client,
	})
	if err != nil {
		t.Fatal(err)
	}
	return reader
}

// hangUp drops the connection without answering, the way a proxy or a
// load balancer in front of the API does.
func hangUp(w http.ResponseWriter, _ *http.Request) {
	hijacker, ok := w.(http.Hijacker)
	if !ok {
		return
	}
	connection, _, err := hijacker.Hijack()
	if err == nil {
		_ = connection.Close()
	}
}

// A connection that dropped is reported as a failed read, with the piece
// that was being read named, so an operator sees which half went missing.
func TestAConnectionThatDroppedIsAFailedReadNotAnEmptyRun(t *testing.T) {
	onTheRun := brittle(t, hangUp)
	got, err := onTheRun.Outcomes(context.Background(), 41)
	if err == nil || !strings.Contains(err.Error(), "read GitHub workflow run") {
		t.Fatalf("Outcomes = %+v, %v", got, err)
	}
	if len(got.Outcomes) != 0 || got.RunID != 0 {
		t.Fatalf("a failed read carried a run: %+v", got)
	}

	// The same on the jobs half, after the run itself read cleanly: the
	// run's own details are known by then and still must not be reported.
	onTheJobs := brittle(t, func(w http.ResponseWriter, r *http.Request) {
		if strings.HasSuffix(r.URL.Path, "/jobs") {
			hangUp(w, r)
			return
		}
		_, _ = w.Write([]byte(completedRunBody(41, 1, actionsRunHead)))
	})
	got, err = onTheJobs.Outcomes(context.Background(), 41)
	if err == nil || !strings.Contains(err.Error(), "read GitHub workflow run jobs") {
		t.Fatalf("Outcomes = %+v, %v", got, err)
	}
	if got.RunID != 0 || got.HeadSHA != "" || len(got.Outcomes) != 0 {
		t.Fatalf("a run with an unread job list was reported: %+v", got)
	}
}

// An answer past the response bound is refused rather than parsed, on
// either half of the read. A megabyte of run document is not a run.
func TestAnAnswerPastTheResponseBoundIsRefused(t *testing.T) {
	flood := strings.Repeat("x", maxActionsRunResponse+1)

	oversizedRun := brittle(t, func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte(flood))
	})
	if _, err := oversizedRun.Outcomes(context.Background(), 41); err == nil ||
		!errors.Is(err, ErrActionsRunUnreadable) ||
		!strings.Contains(err.Error(), "workflow run response is unreadable or too large") {
		t.Fatalf("an oversized run: %v", err)
	}

	oversizedJobs := brittle(t, func(w http.ResponseWriter, r *http.Request) {
		if strings.HasSuffix(r.URL.Path, "/jobs") {
			_, _ = w.Write([]byte(flood))
			return
		}
		_, _ = w.Write([]byte(completedRunBody(41, 1, actionsRunHead)))
	})
	if _, err := oversizedJobs.Outcomes(context.Background(), 41); err == nil ||
		!errors.Is(err, ErrActionsRunUnreadable) ||
		!strings.Contains(err.Error(), "workflow run jobs response is unreadable or too large") {
		t.Fatalf("an oversized job list: %v", err)
	}

	// The bound itself is not the refusal: a document of exactly the bound
	// is read, and refused only on what it says.
	atTheBound := brittle(t, func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte(strings.Repeat("x", maxActionsRunResponse)))
	})
	if _, err := atTheBound.Outcomes(context.Background(), 41); err == nil ||
		!strings.Contains(err.Error(), "unreadable run document") {
		t.Fatalf("a document at the bound: %v", err)
	}
}

// A body that stops short of what its own headers promised is a failed
// read too, not a short run.
func TestABodyThatStopsShortIsAFailedRead(t *testing.T) {
	reader := brittle(t, func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Length", "4096")
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte(`{"id":41,`))
		if flusher, ok := w.(http.Flusher); ok {
			flusher.Flush()
		}
		hangUp(w, r)
	})
	got, err := reader.Outcomes(context.Background(), 41)
	if err == nil || !errors.Is(err, ErrActionsRunUnreadable) ||
		!strings.Contains(err.Error(), "unreadable or too large") {
		t.Fatalf("Outcomes = %+v, %v", got, err)
	}
}

// A caller that cannot be answered is refused before GitHub is asked, and
// a reader that does not exist answers rather than crashing the command.
func TestAnUnanswerableOutcomeRequestNeverReachesGitHub(t *testing.T) {
	asked := false
	reader := brittle(t, func(w http.ResponseWriter, _ *http.Request) {
		asked = true
		_, _ = w.Write([]byte(completedRunBody(41, 1, actionsRunHead)))
	})
	for name, run := range map[string]int64{"no run": 0, "a negative run": -41} {
		if _, err := reader.Outcomes(context.Background(), run); err == nil ||
			!strings.Contains(err.Error(), "invalid workflow run outcome request") {
			t.Fatalf("%s was accepted: %v", name, err)
		}
	}
	if _, err := reader.Outcomes(nil, 41); err == nil {
		t.Fatal("a nil caller was accepted")
	}
	var absent *ActionsOutcomeReader
	if _, err := absent.Outcomes(context.Background(), 41); err == nil {
		t.Fatal("a reader that does not exist answered with a run")
	}
	if asked {
		t.Fatal("GitHub was asked about a run nobody could name")
	}
}
