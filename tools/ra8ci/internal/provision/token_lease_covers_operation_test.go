// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"net/http"
	"strings"
	"testing"
	"time"
)

func leasedToken(issued time.Time, lease time.Duration) *AppRoleToken {
	return &AppRoleToken{
		value:     []byte("s.0123456789abcdef"),
		client:    &http.Client{},
		revokeURL: "https://vault.example.net:8200/v1/auth/token/revoke-self",
		issued:    issued,
		lease:     lease,
	}
}

func TestALeaseThatCannotCoverOneCommandIsRefused(t *testing.T) {
	now := time.Date(2026, 9, 26, 11, 0, 0, 0, time.UTC)
	err := checkTokenLeaseCoversOperation(leasedToken(now, time.Minute), 20*time.Minute, now)
	if err == nil {
		t.Fatal("a one-minute lease must not be handed to a twenty-minute command")
	}
}

func TestALeaseLongerThanTheDeadlineIsOrdinary(t *testing.T) {
	now := time.Date(2026, 9, 26, 11, 0, 0, 0, time.UTC)
	if err := checkTokenLeaseCoversOperation(leasedToken(now, time.Hour), 20*time.Minute, now); err != nil {
		t.Fatalf("the ordinary configuration must be accepted: %v", err)
	}
}

func TestALeaseThatExactlyCoversTheDeadlineIsAccepted(t *testing.T) {
	now := time.Date(2026, 9, 26, 11, 0, 0, 0, time.UTC)
	if err := checkTokenLeaseCoversOperation(leasedToken(now, 20*time.Minute), 20*time.Minute, now); err != nil {
		t.Fatalf("an exactly covering lease must be accepted: %v", err)
	}
	if err := checkTokenLeaseCoversOperation(leasedToken(now, 20*time.Minute-time.Second),
		20*time.Minute, now); err == nil {
		t.Fatal("one second short of the deadline must be refused")
	}
}

func TestATokenIsJudgedByWhatIsLeftOfItsLease(t *testing.T) {
	issued := time.Date(2026, 9, 26, 11, 0, 0, 0, time.UTC)
	token := leasedToken(issued, time.Hour)
	if err := checkTokenLeaseCoversOperation(token, 20*time.Minute, issued.Add(30*time.Minute)); err != nil {
		t.Fatalf("thirty minutes left covers a twenty-minute command: %v", err)
	}
	if err := checkTokenLeaseCoversOperation(token, 20*time.Minute, issued.Add(50*time.Minute)); err == nil {
		t.Fatal("ten minutes left cannot cover a twenty-minute command")
	}
}

func TestAnAlreadyExpiredLeaseIsRefused(t *testing.T) {
	issued := time.Date(2026, 9, 26, 11, 0, 0, 0, time.UTC)
	err := checkTokenLeaseCoversOperation(leasedToken(issued, time.Minute), time.Second, issued.Add(time.Hour))
	if err == nil {
		t.Fatal("an expired lease must be refused however short the deadline is")
	}
}

func TestATokenWithNoRecordedLeaseIsRefused(t *testing.T) {
	now := time.Date(2026, 9, 26, 11, 0, 0, 0, time.UTC)
	cases := map[string]*AppRoleToken{
		"nil token":     nil,
		"no lease":      leasedToken(now, 0),
		"negative":      leasedToken(now, -time.Minute),
		"no issue time": {value: []byte("s.0123456789abcdef"), client: &http.Client{}, lease: time.Hour},
	}
	for name, token := range cases {
		if err := checkTokenLeaseCoversOperation(token, time.Minute, now); err == nil {
			t.Fatalf("%s: a token whose lease cannot be judged must be refused", name)
		}
	}
	if err := checkTokenLeaseCoversOperation(leasedToken(now, time.Hour), 0, now); err == nil {
		t.Fatal("an unstated operation timeout must be refused")
	}
}

func TestTheRefusalNamesWhatIsLeftAndWhatIsNeeded(t *testing.T) {
	now := time.Date(2026, 9, 26, 11, 0, 0, 0, time.UTC)
	err := checkTokenLeaseCoversOperation(leasedToken(now, 90*time.Second), 20*time.Minute, now)
	if err == nil {
		t.Fatal("expected a refusal")
	}
	message := err.Error()
	if !strings.Contains(message, "1m30s") || !strings.Contains(message, "20m0s") {
		t.Fatalf("the refusal must name both durations, got %q", message)
	}
}

// The two policies this check sits between are bounded independently:
// LoginAppRole accepts a lease of 1s to 1h and OpenTerraformRuntime accepts a
// deadline of 1s to 30m. Neither bound constrains the other, which is the
// whole reason this check exists, so sweep the corners of both and assert the
// refusals are exactly the pairs where the lease is shorter than the deadline.
func TestTheTwoBoundedPoliciesDoNotFitInsideOneAnother(t *testing.T) {
	now := time.Date(2026, 9, 26, 11, 0, 0, 0, time.UTC)
	leases := []time.Duration{time.Second, time.Minute, 5 * time.Minute, 20 * time.Minute,
		30 * time.Minute, time.Hour}
	deadlines := []time.Duration{time.Second, 30 * time.Second, 5 * time.Minute,
		20 * time.Minute, 30 * time.Minute}
	refusals := 0
	for _, lease := range leases {
		for _, deadline := range deadlines {
			err := checkTokenLeaseCoversOperation(leasedToken(now, lease), deadline, now)
			if (err != nil) != (lease < deadline) {
				t.Fatalf("lease %s against deadline %s: refused=%v", lease, deadline, err != nil)
			}
			if err != nil {
				refusals++
			}
		}
	}
	if refusals == 0 {
		t.Fatal("no in-policy pair was refused; the sweep proves nothing")
	}
}

func TestAnHourLeaseAgainstTheDefaultDeadlineIsTheHealthyShape(t *testing.T) {
	now := time.Date(2026, 9, 26, 11, 0, 0, 0, time.UTC)
	if err := checkTokenLeaseCoversOperation(leasedToken(now, time.Hour), 20*time.Minute, now); err != nil {
		t.Fatalf("the default deployment must not be refused: %v", err)
	}
}
