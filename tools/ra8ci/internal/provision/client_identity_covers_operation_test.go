// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"crypto/tls"
	"crypto/x509"
	"strings"
	"testing"
	"time"
)

// coveringPair mints a state client identity whose leaf and issuer expire when
// the caller says, using the chain builder client_identity_rule_test.go
// already has. presentIssuer decides whether the chain reaches the far end as
// a leaf alone or as a leaf beside the authority that signed it.
//
// Every caller below takes its "now" from secondGranularity. A certificate
// carries its validity window to the second (ASN.1 has nowhere to put
// anything finer), so a fixture built from an unrounded clock is asking the
// rule about a boundary the certificate cannot express. That is a property
// of certificates, not of this rule, and the boundary cases are written in
// whole seconds because of it.
func coveringPair(t *testing.T, now time.Time, leafNotAfter, issuerNotAfter time.Time, presentIssuer bool) tls.Certificate {
	t.Helper()
	return stateClientPair(t,
		stateClientLeaf(now.Add(-time.Hour), leafNotAfter,
			x509.KeyUsageDigitalSignature, []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth}),
		stateClientAuthority(t, now.Add(-time.Hour), issuerNotAfter,
			x509.KeyUsageCertSign|x509.KeyUsageDigitalSignature),
		presentIssuer)
}

// secondGranularity is the clock every fixture here is built from, rounded to
// the precision a certificate can actually record.
func secondGranularity() time.Time {
	return time.Now().Truncate(time.Second)
}

func TestAStateClientThatCannotCoverOneCommandIsRefused(t *testing.T) {
	now := secondGranularity()
	for _, testCase := range []struct {
		name     string
		left     time.Duration
		deadline time.Duration
	}{
		{"a minute left against the default deadline", time.Minute, 20 * time.Minute},
		{"one second short", 20*time.Minute - time.Second, 20 * time.Minute},
		{"already expired", -time.Second, time.Second},
		{"expiring exactly now", 0, time.Second},
		{"half a command", 10 * time.Minute, 20 * time.Minute},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			pair := coveringPair(t, now, now.Add(testCase.left), now.Add(48*time.Hour), true)
			if err := checkClientIdentityCoversOperation(pair, testCase.deadline, now); err == nil {
				t.Fatal("an identity that cannot cover one command was accepted")
			}
		})
	}
}

func TestAStateClientThatOutlastsTheDeadlineIsOrdinary(t *testing.T) {
	now := secondGranularity()
	for _, left := range []time.Duration{
		20*time.Minute + time.Second,
		21 * time.Minute,
		24 * time.Hour,
		90 * 24 * time.Hour,
	} {
		pair := coveringPair(t, now, now.Add(left), now.Add(365*24*time.Hour), true)
		if err := checkClientIdentityCoversOperation(pair, 20*time.Minute, now); err != nil {
			t.Fatalf("%s of validity against a 20m command was refused: %v", left, err)
		}
	}
}

func TestAStateClientThatExactlyCoversTheDeadlineIsAccepted(t *testing.T) {
	now := secondGranularity()
	deadline := 20 * time.Minute
	pair := coveringPair(t, now, now.Add(deadline), now.Add(48*time.Hour), true)
	if err := checkClientIdentityCoversOperation(pair, deadline, now); err != nil {
		t.Fatalf("an identity that exactly covers one command was refused: %v", err)
	}
	// One second either side of the boundary decides it, and nothing else
	// about the identity changed between these two calls.
	if err := checkClientIdentityCoversOperation(pair, deadline+time.Second, now); err == nil {
		t.Fatal("a deadline one second past the certificate was accepted")
	}
}

func TestTheEarliestCertificateInThePresentedChainDecides(t *testing.T) {
	now := secondGranularity()
	deadline := 20 * time.Minute
	// The leaf is good for a year; the issuer beside it has a minute left.
	// A rule that reads Certificate[0] sees nothing wrong here.
	pair := coveringPair(t, now, now.Add(365*24*time.Hour), now.Add(time.Minute), true)
	err := checkClientIdentityCoversOperation(pair, deadline, now)
	if err == nil {
		t.Fatal("an expiring issuer in the presented chain was accepted")
	}
	if !strings.Contains(err.Error(), "state client CA") {
		t.Fatalf("the refusal did not name the certificate that expires first: %v", err)
	}
	// The same leaf presented alone carries no expiring issuer and is fine,
	// which is what makes this a statement about the chain and not the leaf.
	alone := coveringPair(t, now, now.Add(365*24*time.Hour), now.Add(time.Minute), false)
	if err := checkClientIdentityCoversOperation(alone, deadline, now); err != nil {
		t.Fatalf("a long-lived leaf presented alone was refused: %v", err)
	}
}

func TestAnUnstatedDeadlineLeavesTheIdentityToTheValidityRule(t *testing.T) {
	now := secondGranularity()
	pair := coveringPair(t, now, now.Add(time.Second), now.Add(48*time.Hour), true)
	for _, deadline := range []time.Duration{0, -time.Second, -time.Hour} {
		if err := checkClientIdentityCoversOperation(pair, deadline, now); err != nil {
			t.Fatalf("an unstated deadline (%s) produced a finding: %v", deadline, err)
		}
	}
	// And the validity rule still has its own say about the same pair.
	if err := checkTerraformStateClientIdentity(pair, now); err != nil {
		t.Fatalf("a currently valid identity was refused by the validity rule: %v", err)
	}
}

func TestTheIdentityRefusalNamesWhatIsLeftAndWhatIsNeeded(t *testing.T) {
	now := secondGranularity()
	pair := coveringPair(t, now, now.Add(3*time.Minute), now.Add(48*time.Hour), true)
	err := checkClientIdentityCoversOperation(pair, 20*time.Minute, now)
	if err == nil {
		t.Fatal("expected a refusal")
	}
	message := err.Error()
	for _, want := range []string{"ra8ci-terraform-state", "3m0s", "20m0s"} {
		if !strings.Contains(message, want) {
			t.Fatalf("refusal %q does not carry %q", message, want)
		}
	}
}

func TestAnIdentityWithNoCertificateIsRefusedOnlyWhenADeadlineIsStated(t *testing.T) {
	now := secondGranularity()
	if err := checkClientIdentityCoversOperation(tls.Certificate{}, 20*time.Minute, now); err == nil {
		t.Fatal("an empty identity was accepted against a stated deadline")
	}
	if err := checkClientIdentityCoversOperation(tls.Certificate{}, 0, now); err != nil {
		t.Fatalf("an empty identity with no deadline produced a finding: %v", err)
	}
	if err := checkClientIdentityCoversOperation(
		tls.Certificate{Certificate: [][]byte{[]byte("not a certificate")}},
		20*time.Minute, now); err == nil {
		t.Fatal("an unreadable certificate was accepted")
	}
}

func TestTheTwoIdentityRulesDoNotFitInsideOneAnother(t *testing.T) {
	now := secondGranularity()
	deadline := 20 * time.Minute
	// This is the whole point of the rule: an identity the validity rule
	// calls currently valid, handed to work that outlives it. Neither rule
	// subsumes the other, so both doors are walked over the same pairs.
	for _, left := range []time.Duration{time.Second, time.Minute, 19 * time.Minute} {
		pair := coveringPair(t, now, now.Add(left), now.Add(48*time.Hour), true)
		if err := checkTerraformStateClientIdentity(pair, now); err != nil {
			t.Fatalf("%s of validity was not currently valid: %v", left, err)
		}
		if err := checkClientIdentityCoversOperation(pair, deadline, now); err == nil {
			t.Fatalf("%s of validity covered a %s command", left, deadline)
		}
	}
	// And the reverse: an expired identity fails the validity rule, so this
	// rule is not the one that catches it.
	expired := coveringPair(t, now, now.Add(-time.Hour), now.Add(48*time.Hour), true)
	if err := checkTerraformStateClientIdentity(expired, now); err == nil {
		t.Fatal("an expired identity passed the validity rule")
	}
}

func TestTheBackendDoorHoldsTheIdentityToTheDeadline(t *testing.T) {
	// testHTTPBackendConfig mints a client leaf with an hour of validity left.
	base := testHTTPBackendConfig(t)
	if _, err := HTTPBackendEnvironment(base); err != nil {
		t.Fatalf("the backend door refused its own fixture with no deadline stated: %v", err)
	}
	withinReach := base
	withinReach.OperationTimeout = 20 * time.Minute
	if _, err := HTTPBackendEnvironment(withinReach); err != nil {
		t.Fatalf("an hour of validity did not cover a 20m command: %v", err)
	}
	outOfReach := base
	outOfReach.OperationTimeout = 2 * time.Hour
	if _, err := HTTPBackendEnvironment(outOfReach); err == nil {
		t.Fatal("an hour of validity covered a two-hour command")
	}
}
