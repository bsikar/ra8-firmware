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

// chainAuthority is an intermediate as a real deployment sends one: a live
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

// mintedLink is one certificate of a presented chain kept beside its key, so
// the certificate below it can be signed by it rather than by itself.
type mintedLink struct {
	der  []byte
	cert *x509.Certificate
	key  *ecdsa.PrivateKey
}

// mintLink mints template: signed by parent when one is given, self-signed
// otherwise, and reusing key when one is given so two certificates can share a
// key pair. A chain here is minted as a real path, because the rule in
// presented_chain_is_a_path.go reads whether each link actually issued the one
// before it, which a bundle of self-signed certificates does not.
func mintLink(t *testing.T, template *x509.Certificate, parent *mintedLink, key *ecdsa.PrivateKey, serial int64) mintedLink {
	t.Helper()
	if key == nil {
		generated, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
		if err != nil {
			t.Fatalf("generate key: %v", err)
		}
		key = generated
	}
	template.SerialNumber = big.NewInt(serial)
	signerTemplate := template
	var signerKey any = key
	if parent != nil {
		signerTemplate = parent.cert
		signerKey = parent.key
	}
	der, err := x509.CreateCertificate(rand.Reader, template, signerTemplate, &key.PublicKey, signerKey)
	if err != nil {
		t.Fatalf("create certificate: %v", err)
	}
	cert, err := x509.ParseCertificate(der)
	if err != nil {
		t.Fatalf("parse minted certificate: %v", err)
	}
	return mintedLink{der: der, cert: cert, key: key}
}

// presenting is a valid client identity sending the given issuers after its
// leaf, which is exactly the shape tls.LoadX509KeyPair builds from a
// certificate file holding a leaf and its issuers.
func presenting(t *testing.T, issuers ...*x509.Certificate) tls.Certificate {
	t.Helper()
	return presentingLeaf(t, clientTemplate(), issuers...)
}

// presentingLeaf builds the same shape around a caller's leaf. It mints from
// the far end inward: the last issuer signs itself, each issuer before it is
// signed by the one after, and the leaf is signed by the first, so what comes
// back is the path a deployment really presents.
func presentingLeaf(t *testing.T, leafTemplate *x509.Certificate, issuers ...*x509.Certificate) tls.Certificate {
	t.Helper()
	minted := make([]mintedLink, len(issuers))
	for position := len(issuers) - 1; position >= 0; position-- {
		var parent *mintedLink
		if position+1 < len(issuers) {
			parent = &minted[position+1]
		}
		minted[position] = mintLink(t, issuers[position], parent, nil, int64(10+position))
	}
	var parent *mintedLink
	if len(minted) > 0 {
		parent = &minted[0]
	}
	leaf := mintLink(t, leafTemplate, parent, nil, 1)
	identity := tls.Certificate{Certificate: [][]byte{leaf.der}, PrivateKey: leaf.key}
	for position := range minted {
		identity.Certificate = append(identity.Certificate, minted[position].der)
	}
	return identity
}

func TestALiveIntermediateIsAccepted(t *testing.T) {
	if err := ValidateClientIdentity(presenting(t, chainAuthority()), testNow); err != nil {
		t.Fatalf("an honest chain was refused: %v", err)
	}
	// An intermediate declaring no key usage at all is unconstrained and
	// accepted, the same reading the leaf and a trusted authority get.
	unconstrained := chainAuthority()
	unconstrained.KeyUsage = 0
	if err := ValidateClientIdentity(presenting(t, unconstrained), testNow); err != nil {
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
	err := ValidateClientIdentity(presenting(t, notAnAuthority), testNow)
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
	err := ValidateClientIdentity(presenting(t, expired, chainAuthority()), testNow)
	if err == nil || !strings.Contains(err.Error(), "expired at") {
		t.Fatalf("expected an expiry refusal, got %v", err)
	}
}

func TestANotYetValidIntermediateIsRefused(t *testing.T) {
	early := chainAuthority()
	early.NotBefore = testNow.Add(time.Hour)
	early.NotAfter = testNow.Add(48 * time.Hour)
	err := ValidateClientIdentity(presenting(t, early), testNow)
	if err == nil || !strings.Contains(err.Error(), "not valid until") {
		t.Fatalf("expected a not-yet-valid refusal, got %v", err)
	}
}

func TestAnIntermediateThatMayNotSignCertificatesIsRefused(t *testing.T) {
	cannotSign := chainAuthority()
	cannotSign.KeyUsage = x509.KeyUsageDigitalSignature
	err := ValidateClientIdentity(presenting(t, cannotSign), testNow)
	if err == nil || !strings.Contains(err.Error(), "may not sign certificates") {
		t.Fatalf("expected a signing refusal, got %v", err)
	}
}

func TestUnparseableChainBytesAreRefusedNamingThePosition(t *testing.T) {
	identity := presenting(t, chainAuthority())
	identity.Certificate = append(identity.Certificate, []byte("not a certificate"))
	err := ValidateClientIdentity(identity, testNow)
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
	expired := chainAuthority()
	expired.NotBefore = testNow.Add(-48 * time.Hour)
	expired.NotAfter = testNow.Add(-time.Hour)
	err := ValidateServerIdentity(presentingLeaf(t, server, expired), testNow)
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
		identity := presenting(t, template)
		parsed, err := x509.ParseCertificate(identity.Certificate[1])
		if err != nil {
			t.Fatalf("%s: parse: %v", name, err)
		}
		where := fmt.Sprintf("subject %q sha256 %s", parsed.Subject.String(), Fingerprint(parsed))
		bundleRefused := !parsed.IsCA || checkAuthorityCanSign(parsed, where, "client") != nil
		chainRefused := ValidateClientIdentity(identity, testNow) != nil
		if bundleRefused != chainRefused {
			t.Fatalf("%s: the trust bundle and the presented chain disagree (bundle refused %v, chain refused %v)",
				name, bundleRefused, chainRefused)
		}
	}
}

func TestEveryChainRefusalNamesTheSubjectAndFingerprint(t *testing.T) {
	notAnAuthority := chainAuthority()
	notAnAuthority.IsCA = false
	identity := presenting(t, notAnAuthority)
	parsed, err := x509.ParseCertificate(identity.Certificate[1])
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	refusal := ValidateClientIdentity(identity, testNow)
	if refusal == nil {
		t.Fatal("expected a refusal")
	}
	if !strings.Contains(refusal.Error(), Fingerprint(parsed)) || !strings.Contains(refusal.Error(), parsed.Subject.String()) {
		t.Fatalf("the refusal names neither the subject nor the fingerprint: %v", refusal)
	}
}
