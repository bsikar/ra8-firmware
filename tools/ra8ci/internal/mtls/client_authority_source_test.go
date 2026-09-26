// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package mtls

import (
	"crypto/x509"
	"errors"
	"strings"
	"testing"
	"time"
)

// clientAuthorityBundle issues a self-signed authority with the window given
// and returns its PEM. It is the client-side twin of serverAuthorityPEM.
func clientAuthorityBundle(t *testing.T, name string, notBefore, notAfter time.Time) []byte {
	t.Helper()
	template := authorityTemplate(name)
	template.NotBefore = notBefore
	template.NotAfter = notAfter
	return encodePEM(t, issue(t, template))
}

func TestClientAuthoritySourceAcceptsALiveBundle(t *testing.T) {
	clock := testNow
	bundle := clientAuthorityBundle(t, "ra8ci-ca", testNow.Add(-time.Hour), testNow.Add(time.Hour))
	pool, usable, err := ClientAuthoritySource(bundle, at(&clock))
	if err != nil {
		t.Fatalf("a live client CA bundle was refused: %v", err)
	}
	if pool == nil {
		t.Fatal("no pool was returned for a live bundle")
	}
	if err := usable(); err != nil {
		t.Fatalf("the first handshake was refused: %v", err)
	}
}

func TestAnAuthorityExpiringUnderARunningListenerIsRefused(t *testing.T) {
	clock := testNow
	bundle := clientAuthorityBundle(t, "ra8ci-ca", testNow.Add(-time.Hour), testNow.Add(time.Hour))
	_, usable, err := ClientAuthoritySource(bundle, at(&clock))
	if err != nil {
		t.Fatalf("a live client CA bundle was refused: %v", err)
	}
	if err := usable(); err != nil {
		t.Fatalf("the handshake before expiry was refused: %v", err)
	}
	clock = testNow.Add(2 * time.Hour)
	err = usable()
	if !errors.Is(err, ErrIdentity) {
		t.Fatalf("an expired bundle was still accepted at the handshake: %v", err)
	}
	if !strings.Contains(err.Error(), "client CA bundle") {
		t.Fatalf("the refusal does not name the file to go and look at: %v", err)
	}
}

func TestARotatingBundleKeepsTheListenerServing(t *testing.T) {
	clock := testNow
	retiring := clientAuthorityBundle(t, "ra8ci-ca-old", testNow.Add(-72*time.Hour), testNow.Add(time.Hour))
	live := clientAuthorityBundle(t, "ra8ci-ca-new", testNow.Add(-time.Hour), testNow.Add(72*time.Hour))
	_, usable, err := ClientAuthoritySource(append(retiring, live...), at(&clock))
	if err != nil {
		t.Fatalf("a rotating bundle was refused: %v", err)
	}
	clock = testNow.Add(2 * time.Hour)
	if err := usable(); err != nil {
		t.Fatalf("a bundle whose retiring authority expired was refused: %v", err)
	}
}

func TestAnAuthorityBecomesUsableWhenItsWindowOpens(t *testing.T) {
	clock := testNow
	live := clientAuthorityBundle(t, "ra8ci-ca-now", testNow.Add(-time.Hour), testNow.Add(time.Hour))
	future := clientAuthorityBundle(t, "ra8ci-ca-next", testNow.Add(30*time.Minute), testNow.Add(72*time.Hour))
	_, usable, err := ClientAuthoritySource(append(live, future...), at(&clock))
	if err != nil {
		t.Fatalf("a bundle holding tomorrow's authority was refused: %v", err)
	}
	clock = testNow.Add(2 * time.Hour)
	if err := usable(); err != nil {
		t.Fatalf("the incoming authority was not picked up when its window opened: %v", err)
	}
}

func TestAnUnusableBundleIsRefusedBeforeTheSocketOpens(t *testing.T) {
	clock := testNow
	expired := clientAuthorityBundle(t, "ra8ci-ca", testNow.Add(-72*time.Hour), testNow.Add(-time.Hour))
	pool, usable, err := ClientAuthoritySource(expired, at(&clock))
	if !errors.Is(err, ErrIdentity) {
		t.Fatalf("an entirely expired bundle was accepted: %v", err)
	}
	if pool != nil || usable != nil {
		t.Fatal("a refused bundle still handed back something to serve with")
	}
}

func TestTheStartupRefusalsAreTheOnesClientAuthoritiesAlreadyMakes(t *testing.T) {
	clock := testNow
	for _, bundle := range [][]byte{nil, []byte("not pem at all"), encodePEM(t, issue(t, clientTemplate()))} {
		_, _, sourceErr := ClientAuthoritySource(bundle, at(&clock))
		_, poolErr := ClientAuthorities(bundle, testNow)
		if (sourceErr == nil) != (poolErr == nil) {
			t.Fatalf("the source and the pool disagree: %v vs %v", sourceErr, poolErr)
		}
		if sourceErr != nil && sourceErr.Error() != poolErr.Error() {
			t.Fatalf("the source rewords a refusal: %q vs %q", sourceErr, poolErr)
		}
	}
}

func TestTheHandshakeCheckReadsTheSamePoolTheVerifierDoes(t *testing.T) {
	clock := testNow
	bundle := clientAuthorityBundle(t, "ra8ci-ca", testNow.Add(-time.Hour), testNow.Add(time.Hour))
	pool, usable, err := ClientAuthoritySource(bundle, at(&clock))
	if err != nil {
		t.Fatalf("a live bundle was refused: %v", err)
	}
	fromClientAuthorities, err := ClientAuthorities(bundle, testNow)
	if err != nil {
		t.Fatalf("the same bundle was refused by ClientAuthorities: %v", err)
	}
	if !pool.Equal(fromClientAuthorities) {
		t.Fatal("the source hands the verifier a different pool than ClientAuthorities builds")
	}
	if err := usable(); err != nil {
		t.Fatalf("the handshake check refused a live bundle: %v", err)
	}
}

func TestANilClockReadsTheWallClockForAuthorities(t *testing.T) {
	now := time.Now()
	bundle := clientAuthorityBundle(t, "ra8ci-ca", now.Add(-time.Hour), now.Add(time.Hour))
	_, usable, err := ClientAuthoritySource(bundle, nil)
	if err != nil {
		t.Fatalf("a bundle live right now was refused with a nil clock: %v", err)
	}
	if err := usable(); err != nil {
		t.Fatalf("the handshake check refused a bundle live right now: %v", err)
	}
}

func TestTheParsedBundleIsNotReReadPerHandshake(t *testing.T) {
	clock := testNow
	bundle := clientAuthorityBundle(t, "ra8ci-ca", testNow.Add(-time.Hour), testNow.Add(time.Hour))
	pool, usable, err := ClientAuthoritySource(bundle, at(&clock))
	if err != nil {
		t.Fatalf("a live bundle was refused: %v", err)
	}
	// Overwriting the caller's bytes must not change what the listener
	// verifies with: the bundle was parsed once, at construction.
	for i := range bundle {
		bundle[i] = 0
	}
	if err := usable(); err != nil {
		t.Fatalf("clobbering the caller's bytes broke the handshake check: %v", err)
	}
	if pool.Equal(x509.NewCertPool()) {
		t.Fatal("the pool went empty after the caller's bytes changed")
	}
}
