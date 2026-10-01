// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package mtls

import (
	"crypto/tls"
	"crypto/x509"
	"fmt"
	"time"
)

// checkPresentedChain holds the certificates a key pair presents BESIDE its
// leaf to the job they are presented for.
//
// A tls.Certificate carries a chain, not a certificate: position zero is the
// identity and every position after it is an issuer sent along so the far end
// can build a path from that identity to an authority it already trusts. Every
// check in this package read position zero and stopped there. Leaf parses
// Certificate[0], ValidateClientIdentity and ValidateServerIdentity judge what
// Leaf hands back, and provision/http_backend.go parses Certificate[0] for the
// same reason. Nothing ever looked at the rest, so a chain file holding an
// end-entity certificate after the leaf, an intermediate that expired over the
// weekend, or an intermediate that may not sign certificates loaded without
// complaint and was presented on every handshake.
//
// The far end is where it fails, and it fails the way this package exists to
// stop: Go's verifier refuses a path through an intermediate that is outside
// its validity window or that may not sign, and reports "certificate signed by
// unknown authority". That reaches the operator as a denial indistinguishable
// from a missing grant, from a host whose own leaf is perfectly good and whose
// error says nothing about the second certificate in the file.
//
// A presented chain is a PATH, not a bundle of alternatives, which is why the
// expiry reading here is stricter than the one parseAuthorities gives a CA
// bundle. An expired authority in a trust bundle sits beside a live one and is
// a rotation in progress; an expired certificate in a presented chain is a link
// the far end has to walk through, and there is no other link to walk instead.
func checkPresentedChain(identity tls.Certificate, now time.Time, role string) error {
	for position, der := range identity.Certificate {
		if position == 0 {
			// The leaf is the identity, judged by the caller.
			continue
		}
		issuer, err := x509.ParseCertificate(der)
		if err != nil {
			return fmt.Errorf("%w: parse the certificate at position %d of the presented %s chain: %v",
				ErrIdentity, position, role, err)
		}
		if err := checkChainLink(issuer, position, role, now); err != nil {
			return err
		}
	}
	return nil
}

// checkChainLink states what one issuer in a presented chain must be. The rules
// are the ones parseAuthorityBundle and checkAuthorityCanSign already apply to a
// trusted authority, because they are the same rules Go's verifier applies when
// it walks this link: an authority, allowed to sign certificates if it declares
// any usage at all, and usable today.
//
// The message names the position rather than a file, because a chain arrives as
// one certificate file and the position is what tells the operator which
// certificate in it to go and look at. It names the subject and the public
// fingerprint, never the key, the same way every other refusal here does.
func checkChainLink(issuer *x509.Certificate, position int, role string, now time.Time) error {
	where := fmt.Sprintf("subject %q sha256 %s", issuer.Subject.String(), Fingerprint(issuer))
	if !issuer.IsCA {
		return fmt.Errorf("%w: %s at position %d of the presented %s chain is not a certificate authority",
			ErrIdentity, where, position, role)
	}
	at := fmt.Sprintf("at position %d of the presented %s chain", position, role)
	if err := checkNoUnhandledCriticalExtension(issuer, where, at); err != nil {
		return err
	}
	if err := checkSignatureIsVerifiable(issuer, where, at); err != nil {
		return err
	}
	if issuer.KeyUsage != 0 && issuer.KeyUsage&x509.KeyUsageCertSign == 0 {
		return fmt.Errorf("%w: %s at position %d of the presented %s chain may not sign certificates",
			ErrIdentity, where, position, role)
	}
	if now.Before(issuer.NotBefore) {
		return fmt.Errorf("%w: %s at position %d of the presented %s chain is not valid until %s",
			ErrIdentity, where, position, role, issuer.NotBefore.UTC().Format(time.RFC3339))
	}
	if !now.Before(issuer.NotAfter) {
		return fmt.Errorf("%w: %s at position %d of the presented %s chain expired at %s",
			ErrIdentity, where, position, role, issuer.NotAfter.UTC().Format(time.RFC3339))
	}
	return nil
}
