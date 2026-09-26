// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"errors"
	"math/big"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/mtls"
)

// stateClientPair mints a client key pair from a template the caller shaped,
// signed by an authority the caller shaped, and presents the chain the way a
// loaded PEM file does: leaf first, then the issuer when one is asked for.
func stateClientPair(t *testing.T, leaf *x509.Certificate, authority *x509.Certificate, presentIssuer bool) tls.Certificate {
	t.Helper()
	authorityKey, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	authorityDER, err := x509.CreateCertificate(rand.Reader, authority, authority, &authorityKey.PublicKey, authorityKey)
	if err != nil {
		t.Fatal(err)
	}
	parent, err := x509.ParseCertificate(authorityDER)
	if err != nil {
		t.Fatal(err)
	}
	leafKey, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	leafDER, err := x509.CreateCertificate(rand.Reader, leaf, parent, &leafKey.PublicKey, authorityKey)
	if err != nil {
		t.Fatal(err)
	}
	pair := tls.Certificate{Certificate: [][]byte{leafDER}, PrivateKey: leafKey}
	if presentIssuer {
		pair.Certificate = append(pair.Certificate, authorityDER)
	}
	return pair
}

func stateClientAuthority(t *testing.T, notBefore, notAfter time.Time, usage x509.KeyUsage) *x509.Certificate {
	t.Helper()
	return &x509.Certificate{
		SerialNumber:          big.NewInt(100),
		Subject:               pkix.Name{CommonName: "ra8ci state client CA"},
		NotBefore:             notBefore,
		NotAfter:              notAfter,
		IsCA:                  true,
		BasicConstraintsValid: true,
		KeyUsage:              usage,
	}
}

func stateClientLeaf(notBefore, notAfter time.Time, usage x509.KeyUsage, extended []x509.ExtKeyUsage) *x509.Certificate {
	return &x509.Certificate{
		SerialNumber: big.NewInt(101),
		Subject:      pkix.Name{CommonName: "ra8ci-terraform-state"},
		NotBefore:    notBefore,
		NotAfter:     notAfter,
		KeyUsage:     usage,
		ExtKeyUsage:  extended,
	}
}

func TestTheStateClientPairThePlaneActuallyShipsIsAccepted(t *testing.T) {
	now := time.Now()
	pair := stateClientPair(t,
		stateClientLeaf(now.Add(-time.Hour), now.Add(time.Hour),
			x509.KeyUsageDigitalSignature, []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth}),
		stateClientAuthority(t, now.Add(-time.Hour), now.Add(24*time.Hour),
			x509.KeyUsageCertSign|x509.KeyUsageDigitalSignature), true)
	if err := checkTerraformStateClientIdentity(pair, now); err != nil {
		t.Fatalf("the shipped client identity was refused: %v", err)
	}
}

// The first of the two rules the local copy did not carry. A leaf that
// declares key usages without digital signature cannot sign the TLS 1.3
// client authentication, so it fails at the state server rather than here.
func TestAStateClientThatMayNotSignIsRefused(t *testing.T) {
	now := time.Now()
	pair := stateClientPair(t,
		stateClientLeaf(now.Add(-time.Hour), now.Add(time.Hour),
			x509.KeyUsageKeyEncipherment, []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth}),
		stateClientAuthority(t, now.Add(-time.Hour), now.Add(24*time.Hour),
			x509.KeyUsageCertSign|x509.KeyUsageDigitalSignature), true)
	err := checkTerraformStateClientIdentity(pair, now)
	if err == nil || !strings.Contains(err.Error(), "may not be used to sign") {
		t.Fatalf("a client identity that cannot sign was accepted: %v", err)
	}
}

// The second. Every issuer in a presented chain is walked by the far end
// exactly as the leaf is judged here, and a rule that only reads
// Certificate[0] cannot see any of them.
func TestAnExpiredIssuerInThePresentedChainIsRefused(t *testing.T) {
	now := time.Now()
	pair := stateClientPair(t,
		stateClientLeaf(now.Add(-time.Hour), now.Add(time.Hour),
			x509.KeyUsageDigitalSignature, []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth}),
		stateClientAuthority(t, now.Add(-48*time.Hour), now.Add(-time.Hour),
			x509.KeyUsageCertSign|x509.KeyUsageDigitalSignature), true)
	err := checkTerraformStateClientIdentity(pair, now)
	if err == nil || !strings.Contains(err.Error(), "position 1") {
		t.Fatalf("an expired issuer in the presented chain was accepted: %v", err)
	}
}

func TestAnIssuerThatMayNotSignCertificatesIsRefused(t *testing.T) {
	now := time.Now()
	pair := stateClientPair(t,
		stateClientLeaf(now.Add(-time.Hour), now.Add(time.Hour),
			x509.KeyUsageDigitalSignature, []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth}),
		stateClientAuthority(t, now.Add(-time.Hour), now.Add(24*time.Hour),
			x509.KeyUsageDigitalSignature), true)
	err := checkTerraformStateClientIdentity(pair, now)
	if err == nil || !strings.Contains(err.Error(), "may not sign certificates") {
		t.Fatalf("an issuer that may not sign certificates was accepted: %v", err)
	}
}

// A leaf-only key pair is the ordinary shape of the deployed state client
// certificate file, so the chain rule must not start demanding an issuer.
func TestALeafOnlyStateClientIsStillAccepted(t *testing.T) {
	now := time.Now()
	pair := stateClientPair(t,
		stateClientLeaf(now.Add(-time.Hour), now.Add(time.Hour),
			x509.KeyUsageDigitalSignature, []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth}),
		stateClientAuthority(t, now.Add(-time.Hour), now.Add(24*time.Hour),
			x509.KeyUsageCertSign|x509.KeyUsageDigitalSignature), false)
	if err := checkTerraformStateClientIdentity(pair, now); err != nil {
		t.Fatalf("a leaf-only client identity was refused: %v", err)
	}
}

// The three rules the copy did carry, pinned from this side so the swap
// cannot quietly drop one of them.
func TestTheRulesTheLocalCopyAlreadyCarriedStillRefuse(t *testing.T) {
	now := time.Now()
	authority := func() *x509.Certificate {
		return stateClientAuthority(t, now.Add(-time.Hour), now.Add(24*time.Hour),
			x509.KeyUsageCertSign|x509.KeyUsageDigitalSignature)
	}
	for name, leaf := range map[string]*x509.Certificate{
		"expired": stateClientLeaf(now.Add(-48*time.Hour), now.Add(-time.Hour),
			x509.KeyUsageDigitalSignature, []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth}),
		"not yet valid": stateClientLeaf(now.Add(time.Hour), now.Add(24*time.Hour),
			x509.KeyUsageDigitalSignature, []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth}),
		"server certificate": stateClientLeaf(now.Add(-time.Hour), now.Add(time.Hour),
			x509.KeyUsageDigitalSignature, []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth}),
	} {
		t.Run(name, func(t *testing.T) {
			if err := checkTerraformStateClientIdentity(stateClientPair(t, leaf, authority(), true), now); err == nil {
				t.Fatal("an unusable client identity was accepted")
			}
		})
	}
}

func TestAnAuthorityIsNotAStateClientIdentity(t *testing.T) {
	now := time.Now()
	leaf := stateClientAuthority(t, now.Add(-time.Hour), now.Add(time.Hour),
		x509.KeyUsageCertSign|x509.KeyUsageDigitalSignature)
	leaf.SerialNumber = big.NewInt(102)
	err := checkTerraformStateClientIdentity(stateClientPair(t, leaf,
		stateClientAuthority(t, now.Add(-time.Hour), now.Add(24*time.Hour),
			x509.KeyUsageCertSign|x509.KeyUsageDigitalSignature), true), now)
	if err == nil || !strings.Contains(err.Error(), "certificate authority") {
		t.Fatalf("an authority was accepted as the state client identity: %v", err)
	}
}

// An unconstrained leaf, one declaring no extended key usage at all, was
// accepted by the local copy and is accepted by the shared rule. Swapping the
// rules must not tighten this, since it is what the deployed certificates of
// an operator who mints without EKU look like.
func TestAnUnconstrainedStateClientIsStillAccepted(t *testing.T) {
	now := time.Now()
	pair := stateClientPair(t,
		stateClientLeaf(now.Add(-time.Hour), now.Add(time.Hour), x509.KeyUsageDigitalSignature, nil),
		stateClientAuthority(t, now.Add(-time.Hour), now.Add(24*time.Hour),
			x509.KeyUsageCertSign|x509.KeyUsageDigitalSignature), true)
	if err := checkTerraformStateClientIdentity(pair, now); err != nil {
		t.Fatalf("an unconstrained client identity was refused: %v", err)
	}
}

// The refusal is classifiable without matching on message text, and it still
// says which certificate is meant.
func TestARefusedStateClientIsClassifiableAndNamed(t *testing.T) {
	now := time.Now()
	pair := stateClientPair(t,
		stateClientLeaf(now.Add(-48*time.Hour), now.Add(-time.Hour),
			x509.KeyUsageDigitalSignature, []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth}),
		stateClientAuthority(t, now.Add(-time.Hour), now.Add(24*time.Hour),
			x509.KeyUsageCertSign|x509.KeyUsageDigitalSignature), true)
	err := checkTerraformStateClientIdentity(pair, now)
	if !errors.Is(err, mtls.ErrIdentity) {
		t.Fatalf("the refusal does not wrap the identity error: %v", err)
	}
	if !strings.Contains(err.Error(), "Terraform client certificate is not a currently valid client leaf") {
		t.Fatalf("the refusal does not name the certificate: %v", err)
	}
}

func TestAnEmptyStateClientPairIsRefusedBeforeTheSharedRule(t *testing.T) {
	err := checkTerraformStateClientIdentity(tls.Certificate{}, time.Now())
	if err == nil || !strings.Contains(err.Error(), "do not match") {
		t.Fatalf("an empty key pair was accepted: %v", err)
	}
}
