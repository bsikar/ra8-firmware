package mtls

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"errors"
	"fmt"
	"math/big"
	"strings"
	"testing"
	"time"
)

// authorityTemplate is an intermediate as a real deployment sends one: a live
// certificate authority allowed to sign certificates.
func chainAuthority() *x509.Certificate {
	return &x509.Certificate{
		Subject:               pkix.Name{CommonName: "ra8ci-intermediate"},
		NotBefore:             testNow.Add(-24 * time.Hour),
		NotAfter:              testNow.Add(24 * time.Hour),
		KeyUsage:              x509.KeyUsageCertSign,
		IsCA:                  true,
		BasicConstraintsValid: true,
	}
}

// link mints one certificate to be presented beside a leaf. The chain is never
// verified here, only judged certificate by certificate, so a self-signed link
// carries every property the rule reads.
func link(t *testing.T, template *x509.Certificate) []byte {
	t.Helper()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatalf("generate key: %v", err)
	}
	template.SerialNumber = big.NewInt(2)
	der, err := x509.CreateCertificate(rand.Reader, template, template, &key.PublicKey, key)
	if err != nil {
		t.Fatalf("create certificate: %v", err)
	}
	return der
}

// presenting is a valid client identity sending the given certificates after
// its leaf, which is exactly the shape tls.LoadX509KeyPair builds from a
// certificate file holding a leaf and its issuers.
func presenting(t *testing.T, links ...[]byte) tls.Certificate {
	t.Helper()
	identity := issue(t, clientTemplate())
	identity.Certificate = append(identity.Certificate, links...)
	return identity
}

func TestALiveIntermediateIsAccepted(t *testing.T) {
	identity := presenting(t, link(t, chainAuthority()))
	if err := ValidateClientIdentity(identity, testNow); err != nil {
		t.Fatalf("an honest chain was refused: %v", err)
	}
	// An intermediate declaring no key usage at all is unconstrained and
	// accepted, the same reading the leaf and a trusted authority get.
	unconstrained := chainAuthority()
	unconstrained.KeyUsage = 0
	if err := ValidateClientIdentity(presenting(t, link(t, unconstrained)), testNow); err != nil {
		t.Fatalf("an unconstrained intermediate was refused: %v", err)
	}
}

func TestALeafPresentedAloneIsStillAccepted(t *testing.T) {
	if err := ValidateClientIdentity(issue(t, clientTemplate()), testNow); err != nil {
		t.Fatalf("a key pair sending only its leaf was refused: %v", err)
	}
}

func TestAnEndEntityCertificateInThePresentedChainIsRefused(t *testing.T) {
	notAnAuthority := chainAuthority()
	notAnAuthority.IsCA = false
	notAnAuthority.KeyUsage = x509.KeyUsageDigitalSignature
	err := ValidateClientIdentity(presenting(t, link(t, notAnAuthority)), testNow)
	if err == nil || !strings.Contains(err.Error(), "is not a certificate authority") {
		t.Fatalf("expected an authority refusal, got %v", err)
	}
	if !strings.Contains(err.Error(), "position 1") {
		t.Fatalf("the refusal does not name the position: %v", err)
	}
}

// The judgement this test pins: a presented chain is a path, not a bundle of
// alternatives. parseAuthorities forgives an expired authority sitting beside a
// live one because that is a rotation in progress; here the far end has to walk
// through this link and there is no other link to walk instead.
func TestAnExpiredIntermediateIsRefusedEvenBesideALiveOne(t *testing.T) {
	expired := chainAuthority()
	expired.NotBefore = testNow.Add(-48 * time.Hour)
	expired.NotAfter = testNow.Add(-time.Hour)
	err := ValidateClientIdentity(presenting(t, link(t, expired), link(t, chainAuthority())), testNow)
	if err == nil || !strings.Contains(err.Error(), "expired at") {
		t.Fatalf("expected an expiry refusal, got %v", err)
	}
}

func TestANotYetValidIntermediateIsRefused(t *testing.T) {
	early := chainAuthority()
	early.NotBefore = testNow.Add(time.Hour)
	early.NotAfter = testNow.Add(48 * time.Hour)
	err := ValidateClientIdentity(presenting(t, link(t, early)), testNow)
	if err == nil || !strings.Contains(err.Error(), "not valid until") {
		t.Fatalf("expected a not-yet-valid refusal, got %v", err)
	}
}

func TestAnIntermediateThatMayNotSignCertificatesIsRefused(t *testing.T) {
	cannotSign := chainAuthority()
	cannotSign.KeyUsage = x509.KeyUsageDigitalSignature
	err := ValidateClientIdentity(presenting(t, link(t, cannotSign)), testNow)
	if err == nil || !strings.Contains(err.Error(), "may not sign certificates") {
		t.Fatalf("expected a signing refusal, got %v", err)
	}
}

func TestUnparseableChainBytesAreRefusedNamingThePosition(t *testing.T) {
	err := ValidateClientIdentity(presenting(t, link(t, chainAuthority()), []byte("not a certificate")), testNow)
	if err == nil || !strings.Contains(err.Error(), "position 2") {
		t.Fatalf("expected a parse refusal naming position 2, got %v", err)
	}
	if !errors.Is(err, ErrIdentity) {
		t.Fatalf("refusal is not ErrIdentity: %v", err)
	}
}

// The server side presents a chain for the same reason the client does, so it
// is held to the same rule and says which end it is talking about.
func TestTheServerChainIsJudgedTheSameWay(t *testing.T) {
	server := &x509.Certificate{
		Subject:               pkix.Name{CommonName: "ra8ci-server"},
		NotBefore:             testNow.Add(-time.Hour),
		NotAfter:              testNow.Add(time.Hour),
		KeyUsage:              x509.KeyUsageDigitalSignature,
		ExtKeyUsage:           []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		BasicConstraintsValid: true,
	}
	identity := issue(t, server)
	expired := chainAuthority()
	expired.NotBefore = testNow.Add(-48 * time.Hour)
	expired.NotAfter = testNow.Add(-time.Hour)
	identity.Certificate = append(identity.Certificate, link(t, expired))
	err := ValidateServerIdentity(identity, testNow)
	if err == nil || !strings.Contains(err.Error(), "presented server chain") {
		t.Fatalf("expected a server chain refusal, got %v", err)
	}
}

// The two ends of the connection judge an issuer alike: a certificate refused
// as a link in a presented chain is refused as a trusted authority too, and one
// accepted in a bundle is accepted in a chain.
func TestAChainLinkAndATrustedAuthorityAreJudgedAlike(t *testing.T) {
	notAnAuthority := chainAuthority()
	notAnAuthority.IsCA = false
	cannotSign := chainAuthority()
	cannotSign.KeyUsage = x509.KeyUsageDigitalSignature
	for name, template := range map[string]*x509.Certificate{
		"an end-entity certificate":      notAnAuthority,
		"an authority that may not sign": cannotSign,
		"a live authority":               chainAuthority(),
	} {
		der := link(t, template)
		parsed, err := x509.ParseCertificate(der)
		if err != nil {
			t.Fatalf("%s: parse: %v", name, err)
		}
		where := fmt.Sprintf("subject %q sha256 %s", parsed.Subject.String(), Fingerprint(parsed))
		bundleRefused := !parsed.IsCA || checkAuthorityCanSign(parsed, where, "client") != nil
		chainRefused := ValidateClientIdentity(presenting(t, der), testNow) != nil
		if bundleRefused != chainRefused {
			t.Fatalf("%s: the trust bundle and the presented chain disagree (bundle refused %v, chain refused %v)",
				name, bundleRefused, chainRefused)
		}
	}
}

func TestEveryChainRefusalNamesTheSubjectAndFingerprint(t *testing.T) {
	notAnAuthority := chainAuthority()
	notAnAuthority.IsCA = false
	der := link(t, notAnAuthority)
	parsed, err := x509.ParseCertificate(der)
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	refusal := ValidateClientIdentity(presenting(t, der), testNow)
	if refusal == nil {
		t.Fatal("expected a refusal")
	}
	if !strings.Contains(refusal.Error(), Fingerprint(parsed)) || !strings.Contains(refusal.Error(), parsed.Subject.String()) {
		t.Fatalf("the refusal names neither the subject nor the fingerprint: %v", refusal)
	}
}
