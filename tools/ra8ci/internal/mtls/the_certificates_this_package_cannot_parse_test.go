// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package mtls

import (
	"crypto/tls"
	"crypto/x509"
	"encoding/pem"
	"errors"
	"strings"
	"testing"
	"time"
)

// The rules in this package all read a certificate that parsed. This file
// holds what each of them does when one does not, which is the state an
// operator actually reaches: a chain file assembled with a truncated copy and
// paste, a trust bundle holding a PEM header over bytes that are not a
// certificate at all. Every one of these has to be named as this host's own
// file rather than reaching the far end as a handshake denial.

// unparseable is a non-empty DER blob no verifier can read. Non-empty matters:
// an empty one is refused earlier, by the rule that a key pair carrying no
// certificate is not an identity.
func unparseable() []byte {
	return []byte{0x30, 0x03, 0x02, 0x01, 0x7f}
}

// A chain whose leaf cannot be read has no bottom to walk up from. The path
// rule asks for the leaf before it looks at a single link, so the refusal that
// reaches the operator is the leaf's, not a complaint about position 1.
func TestALeafThatCannotBeParsedIsRefusedBeforeThePathIsWalked(t *testing.T) {
	good := presenting(t, chainAuthority())
	identity := tls.Certificate{
		Certificate: [][]byte{unparseable(), good.Certificate[1]},
		PrivateKey:  good.PrivateKey,
	}
	err := checkPresentedChainIsAPath(identity, "client")
	if err == nil || !strings.Contains(err.Error(), "parse leaf certificate") {
		t.Fatalf("expected the leaf parse refusal, got %v", err)
	}
	if strings.Contains(err.Error(), "position") {
		t.Fatalf("the refusal blames a link for a leaf that cannot be read: %v", err)
	}
	if !errors.Is(err, ErrIdentity) {
		t.Fatalf("refusal is not ErrIdentity: %v", err)
	}
}

// And the mirror image: the leaf reads, a certificate beside it does not. The
// operator has one file holding several certificates, so the refusal is only
// actionable if it says which one of them to go and look at, and which end of
// the connection the file belongs to.
func TestUnparseableBytesBesideTheLeafAreRefusedNamingTheirPosition(t *testing.T) {
	for _, role := range []string{"client", "server"} {
		good := presenting(t, chainAuthority(), chainAuthority())
		identity := tls.Certificate{
			Certificate: [][]byte{good.Certificate[0], good.Certificate[1], unparseable()},
			PrivateKey:  good.PrivateKey,
		}
		err := checkPresentedChainIsAPath(identity, role)
		if err == nil || !strings.Contains(err.Error(), "parse the certificate at position 2") {
			t.Fatalf("%s: expected a parse refusal naming position 2, got %v", role, err)
		}
		if !strings.Contains(err.Error(), "presented "+role+" chain") {
			t.Fatalf("%s: the refusal does not name this end of the connection: %v", role, err)
		}
		if !errors.Is(err, ErrIdentity) {
			t.Fatalf("%s: refusal is not ErrIdentity: %v", role, err)
		}
	}
}

// The links before the unreadable one are judged first, so a chain that is
// already not a path is reported as the wrong certificate rather than as a
// parse failure further along. That ordering is what tells an operator holding
// a mangled file whether the fix is to re-export one certificate or to rebuild
// the chain.
func TestABrokenPathIsReportedAheadOfUnreadableBytesFurtherUp(t *testing.T) {
	stranger := presentedBy(t, chainAuthority(), strangerAuthority())
	identity := tls.Certificate{
		Certificate: [][]byte{stranger.Certificate[0], stranger.Certificate[1], unparseable()},
		PrivateKey:  stranger.PrivateKey,
	}
	err := checkPresentedChainIsAPath(identity, "client")
	if err == nil || !strings.Contains(err.Error(), "did not issue the certificate at position 0") {
		t.Fatalf("expected the path refusal at position 1 first, got %v", err)
	}
	if strings.Contains(err.Error(), "parse the certificate") {
		t.Fatalf("the parse failure further up was reported ahead of the broken path: %v", err)
	}
}

// A trust bundle is the other file with several certificates in it, and PEM
// armour is no evidence about what it wraps: a truncated export still carries
// its BEGIN CERTIFICATE line. The refusal names the role so the operator knows
// which trust file is theirs to fix.
func TestAnUnparseableAuthorityIsRefusedNamingTheTrustFileItCameFrom(t *testing.T) {
	bundle := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: unparseable()})
	for role, parse := range map[string]func([]byte) (*x509.CertPool, error){
		"server": func(b []byte) (*x509.CertPool, error) { return ServerAuthorities(b, testNow) },
		"client": func(b []byte) (*x509.CertPool, error) { return ClientAuthorities(b, testNow) },
	} {
		pool, err := parse(bundle)
		if err == nil || !strings.Contains(err.Error(), "parse "+role+" certificate authority") {
			t.Fatalf("%s: expected a parse refusal naming the role, got %v", role, err)
		}
		if pool != nil {
			t.Fatalf("%s: a pool was handed back beside a refusal", role)
		}
		if !errors.Is(err, ErrIdentity) {
			t.Fatalf("%s: refusal is not ErrIdentity: %v", role, err)
		}
	}
}

// The whole bundle is read, so an unreadable certificate after a perfectly good
// one is still refused. A bundle is not accepted on the strength of its first
// entry: the operator's copy and paste went wrong somewhere in the file, and
// the point of reading it here is that they find out now rather than at a
// handshake that picks the wrong authority.
func TestAnUnparseableAuthorityIsRefusedEvenBesideAGoodOne(t *testing.T) {
	good := serverTrustBundle(t, "ra8ci-server-ca", testNow.Add(-time.Hour), testNow.Add(time.Hour))
	bundle := append(append([]byte{}, good...),
		pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: unparseable()})...)
	if _, err := ServerAuthorities(bundle, testNow); err == nil ||
		!strings.Contains(err.Error(), "parse server certificate authority") {
		t.Fatalf("expected the second certificate to be read and refused, got %v", err)
	}
}

// The signature rule guards against having nothing to judge, and that guard is
// why a caller with no leaf to hand gets a decision rather than a panic. It
// accepts, deliberately: absence is another rule's complaint, and this one
// reads an algorithm or says nothing.
func TestNoCertificateAtAllIsNothingForTheSignatureRuleToJudge(t *testing.T) {
	if err := checkSignatureIsVerifiable(nil, "subject \"nobody\"", "as a client identity"); err != nil {
		t.Fatalf("the signature rule invented a complaint about no certificate: %v", err)
	}
}
