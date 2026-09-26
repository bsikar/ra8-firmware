// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

import (
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

// The bounds New applies to each duration on its own, restated here so a test
// that claims a shape is "individually in policy" is checking that claim
// rather than asserting it.
func individuallyInPolicy(d time.Duration, ceiling time.Duration) bool {
	return d >= time.Millisecond && d <= ceiling
}

func TestAnOperationBudgetBelowOneRequestIsRefused(t *testing.T) {
	for _, tc := range []struct {
		name      string
		request   time.Duration
		operation time.Duration
		poll      time.Duration
	}{
		{"a whole second of request under a millisecond of operation", time.Second, time.Millisecond, time.Millisecond},
		{"the policy ceilings inverted", 30 * time.Second, time.Second, time.Millisecond},
		{"off by a single nanosecond", time.Second, time.Second - 1, time.Millisecond},
		{"the default request ceiling under a short operation", 15 * time.Second, 10 * time.Second, time.Millisecond},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if !individuallyInPolicy(tc.request, 30*time.Second) || !individuallyInPolicy(tc.operation, 30*time.Minute) || !individuallyInPolicy(tc.poll, 30*time.Second) {
				t.Fatal("test case is not individually in policy, so it proves nothing about the relation")
			}
			err := checkTimeoutsFitTogether(tc.request, tc.operation, tc.poll)
			if !errors.Is(err, ErrInvalid) {
				t.Fatalf("inverted budget accepted: %v", err)
			}
			if !strings.Contains(err.Error(), "operation timeout") || !strings.Contains(err.Error(), "per-request") {
				t.Fatalf("refusal does not name the two durations it is about: %v", err)
			}
		})
	}
}

func TestAPollIntervalAboveTheBudgetIsRefused(t *testing.T) {
	for _, tc := range []struct {
		name      string
		request   time.Duration
		operation time.Duration
		poll      time.Duration
	}{
		{"the poll ceiling above a one-second operation", time.Millisecond, time.Second, 30 * time.Second},
		{"off by a single nanosecond", time.Millisecond, time.Second, time.Second + 1},
		{"the default poll above a very short operation", time.Millisecond, 100 * time.Millisecond, 500 * time.Millisecond},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if !individuallyInPolicy(tc.request, 30*time.Second) || !individuallyInPolicy(tc.operation, 30*time.Minute) || !individuallyInPolicy(tc.poll, 30*time.Second) {
				t.Fatal("test case is not individually in policy, so it proves nothing about the relation")
			}
			err := checkTimeoutsFitTogether(tc.request, tc.operation, tc.poll)
			if !errors.Is(err, ErrInvalid) {
				t.Fatalf("unreachable second poll accepted: %v", err)
			}
			if !strings.Contains(err.Error(), "poll interval") {
				t.Fatalf("refusal does not name the poll interval: %v", err)
			}
		})
	}
}

// The ordinary shape is a request ceiling well under the operation budget and
// a poll well under both, and every one of these has to stay accepted: a rule
// that refuses a working deployment is worse than the fault it prevents.
func TestTheOrdinaryShapesStayAccepted(t *testing.T) {
	for _, tc := range []struct {
		name      string
		request   time.Duration
		operation time.Duration
		poll      time.Duration
	}{
		{"the defaults New fills in", 15 * time.Second, 5 * time.Minute, 500 * time.Millisecond},
		{"the shape the client tests build", time.Second, time.Second, time.Millisecond},
		{"both policy ceilings at once", 30 * time.Second, 30 * time.Minute, 30 * time.Second},
		{"a request ceiling exactly at the budget", time.Second, time.Second, time.Millisecond},
		{"a poll exactly at the budget", time.Millisecond, time.Second, time.Second},
		{"the tightest legal configuration", time.Millisecond, time.Millisecond, time.Millisecond},
		{"a long operation over a short request", time.Millisecond, 30 * time.Minute, time.Millisecond},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if err := checkTimeoutsFitTogether(tc.request, tc.operation, tc.poll); err != nil {
				t.Fatalf("ordinary configuration refused: %v", err)
			}
		})
	}
}

// Both bounds are inclusive, matching every other bound in this package: a
// value exactly at the limit is the limit, not past it. A sweep rather than a
// pair of cases, so a later edit that flips either comparison to strict is
// caught wherever it happens.
func TestBothRelationsAreInclusiveAtTheBoundary(t *testing.T) {
	for _, d := range []time.Duration{time.Millisecond, 10 * time.Millisecond, time.Second, 15 * time.Second, 30 * time.Second} {
		if err := checkTimeoutsFitTogether(d, d, time.Millisecond); err != nil {
			t.Fatalf("operation budget equal to the request ceiling refused at %s: %v", d, err)
		}
		if err := checkTimeoutsFitTogether(time.Millisecond, d, d); err != nil {
			t.Fatalf("poll interval equal to the budget refused at %s: %v", d, err)
		}
		if err := checkTimeoutsFitTogether(d, d-1, time.Millisecond); err == nil {
			t.Fatalf("operation budget one nanosecond below the request ceiling accepted at %s", d)
		}
		if err := checkTimeoutsFitTogether(time.Millisecond, d, d+1); err == nil {
			t.Fatalf("poll interval one nanosecond above the budget accepted at %s", d)
		}
	}
}

// The two rules are separate facts about the configuration and must not stand
// in for one another: a shape that breaks only the first must be reported as
// the first, and the same for the second.
func TestTheTwoRelationsAreToldApart(t *testing.T) {
	onlyBudget := checkTimeoutsFitTogether(time.Second, 500*time.Millisecond, time.Millisecond)
	if onlyBudget == nil || !strings.Contains(onlyBudget.Error(), "per-request") || strings.Contains(onlyBudget.Error(), "poll interval") {
		t.Fatalf("an inverted budget was not reported as itself: %v", onlyBudget)
	}
	onlyPoll := checkTimeoutsFitTogether(time.Millisecond, 500*time.Millisecond, time.Second)
	if onlyPoll == nil || !strings.Contains(onlyPoll.Error(), "poll interval") || strings.Contains(onlyPoll.Error(), "per-request") {
		t.Fatalf("an unreachable poll was not reported as itself: %v", onlyPoll)
	}
	// Both wrong at once reports the budget, the one that makes every
	// request unarrangeable rather than only the second reading.
	both := checkTimeoutsFitTogether(time.Second, 500*time.Millisecond, 30*time.Second)
	if both == nil || !strings.Contains(both.Error(), "per-request") {
		t.Fatalf("both relations broken did not report the budget first: %v", both)
	}
}

// The resolved values are what is judged, so a zero field that New fills in
// with a default is held to the relation exactly as a stated value is. This is
// the shape that would otherwise slip through: an operator states a short
// operation timeout and leaves the request timeout unset, and the fifteen
// second default lands above it.
func TestADefaultedDurationIsJudgedLikeAStatedOne(t *testing.T) {
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {}))
	t.Cleanup(server.Close)
	dir := t.TempDir()
	ca := filepath.Join(dir, "ca.pem")
	if err := os.WriteFile(ca, pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: server.Certificate().Raw}), 0600); err != nil {
		t.Fatal(err)
	}
	token := filepath.Join(dir, "token")
	if err := os.WriteFile(token, []byte("ra8ci@pve!client=secret-token\n"), 0600); err != nil {
		t.Fatal(err)
	}
	base := Config{
		Endpoint: server.URL, CAFile: ca, TokenFile: token,
		Node: "pve", Pool: "ra8-tf-lab", Storage: "ra8-tf-lab",
		AllowedVMIDs: []int{9000}, TemplateVMIDs: []int{9001}, Bridges: []string{"vmbr8"},
	}

	// Request timeout unset, so New fills in fifteen seconds, over a stated
	// ten second operation budget. Individually both are in policy.
	unsetRequest := base
	unsetRequest.OperationTimeout = 10 * time.Second
	unsetRequest.TaskPollInterval = time.Millisecond
	if _, err := New(unsetRequest); !errors.Is(err, ErrInvalid) {
		t.Fatalf("defaulted request timeout above a stated operation budget accepted: %v", err)
	}

	// Poll interval unset, so New fills in five hundred milliseconds, over a
	// stated hundred millisecond operation budget.
	unsetPoll := base
	unsetPoll.RequestTimeout = time.Millisecond
	unsetPoll.OperationTimeout = 100 * time.Millisecond
	if _, err := New(unsetPoll); !errors.Is(err, ErrInvalid) {
		t.Fatalf("defaulted poll interval above a stated operation budget accepted: %v", err)
	}

	// Everything unset is the documented default shape and must build.
	if _, err := New(base); err != nil {
		t.Fatalf("the all-defaults configuration was refused: %v", err)
	}
}

// New refuses the relation as well as the individual ranges, and it refuses it
// as ErrInvalid like every other construction fault, so a caller that only
// asks whether its configuration was rejected is unaffected.
func TestNewRefusesAnInvertedBudget(t *testing.T) {
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {}))
	t.Cleanup(server.Close)
	dir := t.TempDir()
	ca := filepath.Join(dir, "ca.pem")
	if err := os.WriteFile(ca, pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: server.Certificate().Raw}), 0600); err != nil {
		t.Fatal(err)
	}
	token := filepath.Join(dir, "token")
	if err := os.WriteFile(token, []byte("ra8ci@pve!client=secret-token\n"), 0600); err != nil {
		t.Fatal(err)
	}
	cfg := Config{
		Endpoint: server.URL, CAFile: ca, TokenFile: token,
		Node: "pve", Pool: "ra8-tf-lab", Storage: "ra8-tf-lab",
		AllowedVMIDs: []int{9000}, TemplateVMIDs: []int{9001}, Bridges: []string{"vmbr8"},
		RequestTimeout: 30 * time.Second, OperationTimeout: time.Second, TaskPollInterval: time.Millisecond,
	}
	client, err := New(cfg)
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("inverted budget accepted by New: %v", err)
	}
	if client != nil {
		t.Fatal("New returned a client alongside a refusal")
	}
}
