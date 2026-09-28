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

// A commit whose listing failed to arrive must never be reported as a
// commit this plane has published nothing on: the publisher would then
// write a second run under a name that already carries one, which is the
// duplicate an operator reconciles to avoid.
// check_run_reconciler_test.go pins the listings GitHub answers with;
// this pins the ones that never finish arriving.

// wobbly stands a reconciler up against an origin that mints the token
// normally and answers the listing from the handler given. The recorded-
// body fixture cannot drop a connection mid-answer.
func wobbly(t *testing.T, answer http.HandlerFunc) *CheckRunReconciler {
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
	reconciler, err := NewCheckRunReconciler(CheckRunReconcilerConfig{
		AppClientID: "Iv1.test", InstallationID: 7, PrivateKeyFile: keyPath,
		Owner: "bsikar", Repository: "ra8-firmware", httpClient: client,
	})
	if err != nil {
		t.Fatal(err)
	}
	return reconciler
}

// A listing whose connection dropped is a failed read, and a commit that
// carried runs on an earlier page is not reported with the part that did
// arrive.
func TestAListingThatNeverArrivedIsNotACleanCommit(t *testing.T) {
	dropped := wobbly(t, hangUp)
	got, err := dropped.PublishedRuns(context.Background(), actionsRunHead)
	if err == nil || !strings.Contains(err.Error(), "read GitHub check run listing") {
		t.Fatalf("PublishedRuns = %+v, %v", got, err)
	}
	if got.HeadSHA != "" || len(got.Runs) != 0 {
		t.Fatalf("a failed read carried a commit: %+v", got)
	}

	// The first page reads cleanly and carries a run of ours; the second
	// drops. The run already in hand must not be reported, because a
	// partial listing is what makes a duplicate publish look safe.
	shadow, _ := reconcilerTaskNames(t)
	page := 0
	partial := wobbly(t, func(w http.ResponseWriter, r *http.Request) {
		page++
		if page > 1 {
			hangUp(w, r)
			return
		}
		runs := make([]string, 0, publishedCheckRunPageSize)
		for i := 0; i < publishedCheckRunPageSize; i++ {
			runs = append(runs, publishedRunJSON(int64(700+i),
				fmt.Sprintf("ra8ci/shadow/%s-%d", shadow, i),
				actionsRunHead, "completed", "success", "held"))
		}
		fmt.Fprintf(w, `{"total_count":%d,"check_runs":[%s]}`,
			publishedCheckRunPageSize*2, strings.Join(runs, ","))
	})
	got, err = partial.PublishedRuns(context.Background(), actionsRunHead)
	if err == nil || !strings.Contains(err.Error(), "read GitHub check run listing") {
		t.Fatalf("PublishedRuns = %+v, %v", got, err)
	}
	if len(got.Runs) != 0 {
		t.Fatalf("%d runs were reported from a half-read walk", len(got.Runs))
	}
}

// A listing past the response bound is refused rather than read up to the
// bound, and the bound itself is not the refusal.
func TestAListingPastTheResponseBoundIsRefused(t *testing.T) {
	oversized := wobbly(t, func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte(strings.Repeat("x", maxPublishedCheckRunResponse+1)))
	})
	if _, err := oversized.PublishedRuns(context.Background(), actionsRunHead); err == nil ||
		!errors.Is(err, ErrPublishedCheckRunsUnreadable) ||
		!strings.Contains(err.Error(), "unreadable check run listing") {
		t.Fatalf("an oversized listing: %v", err)
	}

	atTheBound := wobbly(t, func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte(strings.Repeat("x", maxPublishedCheckRunResponse)))
	})
	if _, err := atTheBound.PublishedRuns(context.Background(), actionsRunHead); err == nil ||
		!errors.Is(err, ErrPublishedCheckRunsUnreadable) {
		t.Fatalf("a listing at the bound: %v", err)
	}

	// A body that stops short of its own Content-Length is the same
	// failure, not a listing with fewer runs in it.
	truncated := wobbly(t, func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Length", "4096")
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte(`{"total_count":1,"check_runs":[{"id":7,`))
		if flusher, ok := w.(http.Flusher); ok {
			flusher.Flush()
		}
		hangUp(w, r)
	})
	got, err := truncated.PublishedRuns(context.Background(), actionsRunHead)
	if err == nil || !errors.Is(err, ErrPublishedCheckRunsUnreadable) {
		t.Fatalf("PublishedRuns = %+v, %v", got, err)
	}
	if len(got.Runs) != 0 {
		t.Fatalf("a truncated listing became runs: %+v", got.Runs)
	}
}

// A request nobody could answer is refused before a token is minted, and a
// reconciler that does not exist answers rather than crashing the command.
func TestAnUnanswerableListingRequestNeverReachesGitHub(t *testing.T) {
	asked := false
	reconciler := wobbly(t, func(w http.ResponseWriter, _ *http.Request) {
		asked = true
		_, _ = w.Write([]byte(`{"total_count":0,"check_runs":[]}`))
	})
	if _, err := reconciler.PublishedRuns(nil, actionsRunHead); err == nil ||
		!strings.Contains(err.Error(), "invalid published check run request") {
		t.Fatalf("a nil caller was accepted: %v", err)
	}
	var absent *CheckRunReconciler
	if _, err := absent.PublishedRuns(context.Background(), actionsRunHead); err == nil ||
		!strings.Contains(err.Error(), "invalid published check run request") {
		t.Fatalf("a reconciler that does not exist answered: %v", err)
	}
	if asked {
		t.Fatal("GitHub was asked about a commit nobody could name")
	}

	// An upper-case SHA is the same commit and is asked about in lower
	// case, which is what keeps two reads of one commit comparable.
	runs, err := reconciler.PublishedRuns(context.Background(), strings.ToUpper(actionsRunHead))
	if err != nil {
		t.Fatalf("an upper-case SHA was refused: %v", err)
	}
	if runs.HeadSHA != actionsRunHead {
		t.Fatalf("commit reported as %q", runs.HeadSHA)
	}
	if !asked {
		t.Fatal("a commit this reconciler can name was never asked about")
	}
}
