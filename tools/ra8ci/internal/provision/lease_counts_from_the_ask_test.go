// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"context"
	"encoding/pem"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestTheLeaseIsCountedFromTheAskNotTheAnswer(t *testing.T) {
	asked := time.Now()
	answered := asked.Add(4 * time.Second)
	start, err := leaseStartsWhenVaultWasAsked(asked, answered)
	if err != nil {
		t.Fatal(err)
	}
	if !start.Equal(asked) {
		t.Fatalf("lease counted from %s, not the ask at %s", start, asked)
	}
}

func TestAnInstantRoundTripStillCountsFromTheAsk(t *testing.T) {
	asked := time.Now()
	start, err := leaseStartsWhenVaultWasAsked(asked, asked)
	if err != nil || !start.Equal(asked) {
		t.Fatalf("equal stamps: %s %v", start, err)
	}
}

func TestAStampThatIsMissingIsRefused(t *testing.T) {
	now := time.Now()
	for name, pair := range map[string][2]time.Time{
		"no ask":    {{}, now},
		"no answer": {now, {}},
		"neither":   {{}, {}},
	} {
		if _, err := leaseStartsWhenVaultWasAsked(pair[0], pair[1]); err == nil {
			t.Fatalf("%s: a lease origin was invented", name)
		}
	}
}

func TestAnAnswerBeforeItsAskIsRefused(t *testing.T) {
	asked := time.Now()
	if _, err := leaseStartsWhenVaultWasAsked(asked, asked.Add(-time.Nanosecond)); err == nil {
		t.Fatal("an inverted round trip was accepted")
	}
}

// The whole point of the stamp is the arithmetic built on it, so drive that
// arithmetic rather than assert the stamp alone: the same lease and the same
// command are judged from both origins across a range of round trips.
func TestCountingFromTheAnswerOverstatesEveryRoundTrip(t *testing.T) {
	const lease = 90 * time.Second
	command := 80 * time.Second
	for _, roundTrip := range []time.Duration{time.Millisecond, time.Second, 5 * time.Second, 20 * time.Second} {
		asked := time.Now()
		answered := asked.Add(roundTrip)
		fromTheAsk := &AppRoleToken{client: &http.Client{}, value: []byte("hvs.aaaaaaaaaaaaaaaa"),
			issued: asked, lease: lease}
		fromTheAnswer := &AppRoleToken{client: &http.Client{}, value: []byte("hvs.aaaaaaaaaaaaaaaa"),
			issued: answered, lease: lease}
		honest := checkTokenLeaseCoversOperation(fromTheAsk, command, answered)
		optimistic := checkTokenLeaseCoversOperation(fromTheAnswer, command, answered)
		if optimistic != nil {
			t.Fatalf("round trip %s: the late stamp refused; fixture no longer shows the gap", roundTrip)
		}
		if roundTrip > lease-command && honest == nil {
			t.Fatalf("round trip %s: the ask-stamped token covered a command it cannot", roundTrip)
		}
	}
}

// vaultLoginServer answers one AppRole login after a delay, so the round trip
// the lease is counted across is a real one rather than a constructed stamp.
func vaultLoginServer(t *testing.T, delay time.Duration, leaseSeconds int) (*httptest.Server, AppRoleConfig) {
	t.Helper()
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/v1/auth/approle/login" {
			http.NotFound(w, r)
			return
		}
		time.Sleep(delay)
		fmt.Fprintf(w, `{"auth":{"client_token":"hvs.test-token-123456","lease_duration":%d,"renewable":true}}`,
			leaseSeconds)
	}))
	directory := t.TempDir()
	write := func(name, value string) string {
		t.Helper()
		file := filepath.Join(directory, name)
		if err := os.WriteFile(file, []byte(value), 0o600); err != nil {
			t.Fatal(err)
		}
		return file
	}
	ca := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: server.Certificate().Raw})
	return server, AppRoleConfig{
		Address: server.URL, AuthMount: "auth/approle",
		RoleIDFile:   write("role-id", "role-id-1234567890\n"),
		SecretIDFile: write("secret-id", "secret-id-12345678901234567890\n"),
		CAFile:       write("ca.pem", string(ca)),
		Timeout:      10 * time.Second,
	}
}

func TestALoginStampsItsLeaseBeforeTheRequestGoesOut(t *testing.T) {
	server, config := vaultLoginServer(t, 300*time.Millisecond, 300)
	defer server.Close()
	before := time.Now()
	token, err := LoginAppRole(context.Background(), config)
	after := time.Now()
	if err != nil {
		t.Fatal(err)
	}
	defer token.Clear()
	if token.issued.Before(before) || token.issued.After(after) {
		t.Fatalf("issue stamp %s is outside the call", token.issued)
	}
	if token.issued.After(before.Add(250 * time.Millisecond)) {
		t.Fatalf("issue stamp %s was taken after the answer, not before the ask", token.issued)
	}
	if token.lease != 300*time.Second {
		t.Fatalf("lease = %s", token.lease)
	}
}

func TestASlowAnswerSpendsLeaseTheSessionCanSee(t *testing.T) {
	server, config := vaultLoginServer(t, 600*time.Millisecond, 1)
	defer server.Close()
	token, err := LoginAppRole(context.Background(), config)
	if err != nil {
		t.Fatal(err)
	}
	defer token.Clear()
	// A one-second lease that took 600ms to arrive cannot cover a one-second
	// command, and counting from the ask is what makes that visible here
	// instead of inside a running Terraform child.
	if err := checkTokenLeaseCoversOperation(token, time.Second, time.Now()); err == nil {
		t.Fatal("a lease already spent on the round trip was approved")
	}
}

func TestAnOrdinaryLeaseStillCoversAnOrdinaryCommand(t *testing.T) {
	server, config := vaultLoginServer(t, 0, 3600)
	defer server.Close()
	token, err := LoginAppRole(context.Background(), config)
	if err != nil {
		t.Fatal(err)
	}
	defer token.Clear()
	if err := checkTokenLeaseCoversOperation(token, 20*time.Minute, time.Now()); err != nil {
		t.Fatalf("an hour's lease was refused for a twenty-minute command: %v", err)
	}
}
