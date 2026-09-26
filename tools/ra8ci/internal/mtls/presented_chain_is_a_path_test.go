package mtls

import (
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"errors"
	"strings"
	"testing"
	"time"
)

// strangerAuthority is a second live intermediate, as good as the first and
// from somewhere else entirely: the certificate an operator reaches for when
// assembling a chain file out of whatever PEM was to hand.
func strangerAuthority() *x509.Certificate {
	other := chainAuthority()
	other.Subject = pkix.Name{CommonName: "ra8ci-other-intermediate"}
	return other
}

// presentedBy builds a client identity whose leaf is issued by signer and which
// presents links instead, so a test can send a chain that is not the path the
// leaf was actually issued under.
func presentedBy(t *testing.T, signer *x509.Certificate, links ...*x509.Certificate) tls.Certificate {
	t.Helper()
	issuer := mintLink(t, signer, nil, nil, 20)
	leaf := mintLink(t, clientTemplate(), &issuer, nil, 1)
	identity := tls.Certificate{Certificate: [][]byte{leaf.der}, PrivateKey: leaf.key}
	for position, link := range links {
		minted := mintLink(t, link, nil, nil, int64(30+position))
		identity.Certificate = append(identity.Certificate, minted.der)
	}
	return identity
}

func TestARealPathIsAccepted(t *testing.T) {
	root := chainAuthority()
	root.Subject = pkix.Name{CommonName: "ra8ci-root"}
	for name, identity := range map[string]tls.Certificate{
		"one intermediate":         presenting(t, chainAuthority()),
		"an intermediate and root": presenting(t, chainAuthority(), root),
	} {
		if err := ValidateClientIdentity(identity, testNow); err != nil {
			t.Fatalf("%s: a real path was refused: %v", name, err)
		}
	}
}

func TestAnIntermediateFromADifferentAuthorityIsRefused(t *testing.T) {
	identity := presentedBy(t, chainAuthority(), strangerAuthority())
	err := ValidateClientIdentity(identity, testNow)
	if err == nil || !strings.Contains(err.Error(), "did not issue the certificate at position 0") {
		t.Fatalf("expected a path refusal naming position 0, got %v", err)
	}
	if !strings.Contains(err.Error(), "position 1 of the presented client chain") {
		t.Fatalf("the refusal does not name the link's own position: %v", err)
	}
	// The operator's fix is to compare two names in one file, so the refusal
	// carries the name the leaf actually asks for as well as the one sent.
	if !strings.Contains(err.Error(), "ra8ci-other-intermediate") || !strings.Contains(err.Error(), "ra8ci-intermediate") {
		t.Fatalf("the refusal does not name both the link and the issuer the leaf asks for: %v", err)
	}
	if !errors.Is(err, ErrIdentity) {
		t.Fatalf("refusal is not ErrIdentity: %v", err)
	}
}

// A break further up is the same mistake one certificate along, and the message
// has to move with it rather than always blaming the leaf's issuer.
func TestARootThatDidNotIssueTheIntermediateIsRefused(t *testing.T) {
	intermediate := chainAuthority()
	stranger := strangerAuthority()
	issuer := mintLink(t, intermediate, nil, nil, 20)
	leaf := mintLink(t, clientTemplate(), &issuer, nil, 1)
	unrelatedRoot := mintLink(t, stranger, nil, nil, 21)
	identity := tls.Certificate{
		Certificate: [][]byte{leaf.der, issuer.der, unrelatedRoot.der},
		PrivateKey:  leaf.key,
	}
	err := ValidateClientIdentity(identity, testNow)
	if err == nil || !strings.Contains(err.Error(), "did not issue the certificate at position 1") {
		t.Fatalf("expected a path refusal naming position 1, got %v", err)
	}
	if !strings.Contains(err.Error(), "position 2 of the presented client chain") {
		t.Fatalf("the refusal does not name the link's own position: %v", err)
	}
}

// The reading this pins: the names lining up is not the signature verifying.
// Go's CheckSignatureFrom compares key identifiers and never subject names, so
// the name check is its own line; here it passes and the signature is what is
// wrong.
func TestAnAuthorityWithTheRightNameAndTheWrongKeyIsRefused(t *testing.T) {
	signer := mintLink(t, chainAuthority(), nil, nil, 20)
	leaf := mintLink(t, clientTemplate(), &signer, nil, 1)
	// Same subject, same shape, minted from a key of its own.
	impostor := mintLink(t, chainAuthority(), nil, nil, 21)
	identity := tls.Certificate{
		Certificate: [][]byte{leaf.der, impostor.der},
		PrivateKey:  leaf.key,
	}
	err := ValidateClientIdentity(identity, testNow)
	if err == nil || !strings.Contains(err.Error(), "did not sign the certificate at position 0") {
		t.Fatalf("expected a signature refusal, got %v", err)
	}
	if strings.Contains(err.Error(), "did not issue") {
		t.Fatalf("the name reading fired instead of the signature reading: %v", err)
	}
}

// And the other way round: two authorities sharing one key pair under different
// names satisfy the signature, which is exactly why the names are read too.
func TestAnAuthorityWithTheRightKeyAndTheWrongNameIsRefused(t *testing.T) {
	signer := mintLink(t, chainAuthority(), nil, nil, 20)
	leaf := mintLink(t, clientTemplate(), &signer, nil, 1)
	renamed := mintLink(t, strangerAuthority(), nil, signer.key, 21)
	identity := tls.Certificate{
		Certificate: [][]byte{leaf.der, renamed.der},
		PrivateKey:  leaf.key,
	}
	parsed, err := x509.ParseCertificate(renamed.der)
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if err := leaf.cert.CheckSignatureFrom(parsed); err != nil {
		t.Fatalf("the fixture is not the one this test is about, the signature already fails: %v", err)
	}
	refusal := ValidateClientIdentity(identity, testNow)
	if refusal == nil || !strings.Contains(refusal.Error(), "did not issue the certificate at position 0") {
		t.Fatalf("expected a name refusal, got %v", refusal)
	}
}

func TestThePathRefusalNamesTheSubjectAndFingerprint(t *testing.T) {
	identity := presentedBy(t, chainAuthority(), strangerAuthority())
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

// The server presents a path for the same reason the client does, and says
// which end it is talking about.
func TestTheServerPathIsHeldToTheSameRule(t *testing.T) {
	server := clientTemplate()
	server.Subject = pkix.Name{CommonName: "ra8ci-server"}
	server.ExtKeyUsage = []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth}
	issuer := mintLink(t, chainAuthority(), nil, nil, 20)
	leaf := mintLink(t, server, &issuer, nil, 1)
	stranger := mintLink(t, strangerAuthority(), nil, nil, 21)
	identity := tls.Certificate{
		Certificate: [][]byte{leaf.der, stranger.der},
		PrivateKey:  leaf.key,
	}
	err := ValidateServerIdentity(identity, testNow)
	if err == nil || !strings.Contains(err.Error(), "presented server chain") {
		t.Fatalf("expected a server path refusal, got %v", err)
	}
	if err := ValidateServerIdentity(presentingLeaf(t, server, chainAuthority()), testNow); err != nil {
		t.Fatalf("an honest server path was refused: %v", err)
	}
}

// The order this pins: what a link IS comes before how it is joined on. An
// operator sent an expired certificate from the wrong authority has to replace
// it either way, and the expiry is the reading that says so without asking them
// to compare names first.
func TestALinkIsJudgedOnItsOwnPropertiesBeforeThePath(t *testing.T) {
	expiredStranger := strangerAuthority()
	expiredStranger.NotBefore = testNow.Add(-48 * time.Hour)
	expiredStranger.NotAfter = testNow.Add(-time.Hour)
	err := ValidateClientIdentity(presentedBy(t, chainAuthority(), expiredStranger), testNow)
	if err == nil || !strings.Contains(err.Error(), "expired at") {
		t.Fatalf("expected the expiry refusal first, got %v", err)
	}
	if strings.Contains(err.Error(), "did not issue") {
		t.Fatalf("the path reading was reported instead of the expiry: %v", err)
	}
}

// Separation: a key pair sending only its leaf presents no path to be wrong
// about, so this rule has nothing to say about it, and neither about a key pair
// carrying no certificate at all, which Leaf refuses long before this runs.
func TestALeafAloneHasNoPathToJudge(t *testing.T) {
	if err := checkPresentedChainIsAPath(issue(t, clientTemplate()), "client"); err != nil {
		t.Fatalf("a lone leaf was refused: %v", err)
	}
	if err := checkPresentedChainIsAPath(tls.Certificate{}, "client"); err != nil {
		t.Fatalf("an empty key pair is Leaf's refusal to make, not this one: %v", err)
	}
	if err := ValidateClientIdentity(tls.Certificate{}, testNow); err == nil {
		t.Fatal("an empty key pair reached the end of validation unrefused")
	}
}
