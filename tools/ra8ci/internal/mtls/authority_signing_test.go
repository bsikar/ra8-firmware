// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package mtls

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"errors"
	"math/big"
	"strings"
	"testing"
	"time"
)

// issuedBy signs template with the key of an authority issue() already made,
// which is the only way to build the real chain the verifier walks. issue()
// self-signs, so it cannot produce one.
func issuedBy(t *testing.T, template *x509.Certificate, authority tls.Certificate) *x509.Certificate {
	t.Helper()
	parent, err := Leaf(authority)
	if err != nil {
		t.Fatalf("parse authority: %v", err)
	}
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatalf("generate key: %v", err)
	}
	template.SerialNumber = big.NewInt(2)
	der, err := x509.CreateCertificate(rand.Reader, template, parent, &key.PublicKey, authority.PrivateKey)
	if err != nil {
		t.Fatalf("create certificate: %v", err)
	}
	leaf, err := x509.ParseCertificate(der)
	if err != nil {
		t.Fatalf("parse leaf: %v", err)
	}
	return leaf
}

// nonSigningAuthority is an authority in every respect the bundle reader
// looked at before this check existed: it parses, it declares IsCA, and it is
// inside its validity window. It declares key usages, and certificate signing
// is not among them.
func nonSigningAuthority(commonName string) *x509.Certificate {
	authority := authorityTemplate(commonName)
	authority.KeyUsage = x509.KeyUsageDigitalSignature
	return authority
}

func TestClientAuthoritiesRefusesAnAuthorityThatMayNotSign(t *testing.T) {
	bundle := encodePEM(t, issue(t, nonSigningAuthority("ra8ci-client-ca-nosign")))
	_, err := ClientAuthorities(bundle, testNow)
	if err == nil || !strings.Contains(err.Error(), "may not sign certificates") {
		t.Fatalf("expected a signing refusal, got %v", err)
	}
	if !errors.Is(err, ErrIdentity) {
		t.Fatalf("refusal is not classifiable as an identity problem: %v", err)
	}
}

// The refusal names the certificate the operator has to go and look at, and
// nothing else. Same wording as every other refusal in this package.
func TestASigningRefusalNamesTheAuthority(t *testing.T) {
	authority := nonSigningAuthority("ra8ci-client-ca-nosign")
	identity := issue(t, authority)
	leaf, err := Leaf(identity)
	if err != nil {
		t.Fatalf("parse leaf: %v", err)
	}
	_, err = ClientAuthorities(encodePEM(t, identity), testNow)
	if err == nil {
		t.Fatal("a non-signing authority was accepted")
	}
	if !strings.Contains(err.Error(), "ra8ci-client-ca-nosign") {
		t.Fatalf("refusal does not name the subject: %v", err)
	}
	if !strings.Contains(err.Error(), Fingerprint(leaf)) {
		t.Fatalf("refusal does not name the fingerprint: %v", err)
	}
}

// This is the reason the check is here: the standard verifier refuses a chain
// through such an authority, and the operator reads that as every client
// being denied. If Go ever stops refusing it, this check is pinning a rule
// that no longer exists and the test says so.
func TestTheStandardVerifierRefusesANonSigningAuthority(t *testing.T) {
	authority := nonSigningAuthority("ra8ci-client-ca-nosign")
	authorityIdentity := issue(t, authority)
	parent, err := Leaf(authorityIdentity)
	if err != nil {
		t.Fatalf("parse authority: %v", err)
	}
	client := issuedBy(t, clientTemplate(), authorityIdentity)
	pool := x509.NewCertPool()
	pool.AddCert(parent)
	if _, err := client.Verify(x509.VerifyOptions{
		Roots:       pool,
		CurrentTime: testNow,
		KeyUsages:   []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth},
	}); err == nil {
		t.Fatal("the standard verifier accepted a chain through an authority that may not sign; this check is pinning the wrong thing")
	}
}

// An authority declaring no key usages at all is unconstrained, the same
// reading an identity with no declared extended key usage gets.
func TestAnUnconstrainedAuthorityIsAccepted(t *testing.T) {
	authority := authorityTemplate("ra8ci-client-ca-unconstrained")
	authority.KeyUsage = 0
	pool, err := ClientAuthorities(encodePEM(t, issue(t, authority)), testNow)
	if err != nil {
		t.Fatalf("an unconstrained authority was refused: %v", err)
	}
	if pool == nil {
		t.Fatal("no pool returned")
	}
}

// Certificate signing alongside other declared usages is the ordinary shape
// and stays accepted.
func TestASigningAuthorityIsAccepted(t *testing.T) {
	authority := authorityTemplate("ra8ci-client-ca-signing")
	authority.KeyUsage = x509.KeyUsageCertSign | x509.KeyUsageCRLSign
	if _, err := ClientAuthorities(encodePEM(t, issue(t, authority)), testNow); err != nil {
		t.Fatalf("a signing authority was refused: %v", err)
	}
}

// A bundle mixing one usable authority with one that may not sign is refused
// rather than quietly reduced to the half that works: the operator put both
// in the file, and a client whose certificate came from the refused one would
// be denied with no reason available anywhere.
func TestOneNonSigningAuthorityRefusesTheWholeBundle(t *testing.T) {
	live := encodePEM(t, issue(t, authorityTemplate("ra8ci-client-ca")))
	broken := encodePEM(t, issue(t, nonSigningAuthority("ra8ci-client-ca-nosign")))
	_, err := ClientAuthorities(append(live, broken...), testNow)
	if err == nil || !strings.Contains(err.Error(), "may not sign certificates") {
		t.Fatalf("expected the bundle to be refused, got %v", err)
	}
}

// A retiring authority that can still sign stays acceptable next to a live
// one. The signing rule is not the expiry rule and must not have taken over
// the rotation case.
func TestARotatingBundleIsStillAccepted(t *testing.T) {
	retiring := authorityTemplate("ra8ci-client-ca-old")
	retiring.NotBefore = testNow.Add(-72 * time.Hour)
	retiring.NotAfter = testNow.Add(-time.Hour)
	bundle := append(encodePEM(t, issue(t, retiring)), encodePEM(t, issue(t, authorityTemplate("ra8ci-client-ca")))...)
	if _, err := ClientAuthorities(bundle, testNow); err != nil {
		t.Fatalf("a rotating bundle was refused: %v", err)
	}
}

func TestCheckAuthorityCanSignRefusesNothing(t *testing.T) {
	if err := checkAuthorityCanSign(nil, "subject \"none\""); !errors.Is(err, ErrIdentity) {
		t.Fatalf("a nil authority was not refused: %v", err)
	}
}
