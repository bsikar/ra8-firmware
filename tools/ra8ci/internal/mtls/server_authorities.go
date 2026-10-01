// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package mtls

import (
	"crypto/x509"
	"encoding/pem"
	"fmt"
	"time"
)

// ServerAuthorities parses the PEM bundle a client trusts the server from, and
// refuses a bundle that cannot authenticate the server.
//
// The listener already got this treatment: ClientAuthorities refuses a client
// CA bundle that can authenticate nobody rather than letting every handshake
// fail as an opaque denial. Every client in this tree had the mirror-image
// problem and none of them asked about it. runclient, boardclient, the agent,
// ra8ci sync and ra8ci report slow each read RA8CI_SERVER_CA and handed it to
// x509.CertPool.AppendCertsFromPEM, which reports one thing: whether at least
// one certificate parsed. A bundle holding the server's own end-entity
// certificate, a bundle of authorities that all expired over the weekend, and
// an authority that may not sign all pass that test and then fail at the
// handshake as "certificate signed by unknown authority", which reaches the
// operator looking like a server misconfiguration rather than their own
// trust file.
//
// The rule is the one the listener uses, stated once in parseAuthorities and
// read from both ends. An expired authority alongside a live one is a rotation
// and stays acceptable; what is refused is a bundle with nothing in it that
// can verify anything today.
func ServerAuthorities(bundle []byte, now time.Time) (*x509.CertPool, error) {
	return parseAuthorities(bundle, now, "server")
}

// parseAuthorities states what a certificate authority bundle must be, for
// whichever end of the connection is being trusted. role names that end in
// every refusal, because an operator reading one wants to know which file to
// go and look at: the listener's client CA or their own server CA.
func parseAuthorities(bundle []byte, now time.Time, role string) (*x509.CertPool, error) {
	pool, authorities, err := parseAuthorityBundle(bundle, role)
	if err != nil {
		return nil, err
	}
	if err := checkAuthoritiesUsable(authorities, now, role); err != nil {
		return nil, err
	}
	return pool, nil
}

// parseAuthorityBundle reads the bundle and applies every rule that does not
// depend on the clock: what an authority is, and that it may sign. Those are
// permanent properties of the file, so they are decided once and never asked
// again. It hands back the parsed authorities alongside the pool so a caller
// holding the pool open can re-ask the one rule that does change with time.
func parseAuthorityBundle(bundle []byte, role string) (*x509.CertPool, []*x509.Certificate, error) {
	if len(bundle) == 0 {
		return nil, nil, fmt.Errorf("%w: %s certificate authority bundle is empty", ErrIdentity, role)
	}
	pool := x509.NewCertPool()
	var authorities []*x509.Certificate
	rest := bundle
	for {
		var block *pem.Block
		block, rest = pem.Decode(rest)
		if block == nil {
			break
		}
		if block.Type != "CERTIFICATE" {
			continue
		}
		authority, err := x509.ParseCertificate(block.Bytes)
		if err != nil {
			return nil, nil, fmt.Errorf("%w: parse %s certificate authority: %v", ErrIdentity, role, err)
		}
		where := fmt.Sprintf("subject %q sha256 %s", authority.Subject.String(), Fingerprint(authority))
		if !authority.IsCA {
			return nil, nil, fmt.Errorf("%w: %s in the %s CA bundle is not a certificate authority", ErrIdentity, where, role)
		}
		if err := checkAuthorityCanSign(authority, where, role); err != nil {
			return nil, nil, err
		}
		// Same permanent rule the identities are held to: an authority
		// carrying a field the verifier cannot interpret authenticates
		// nobody, today or after any rotation.
		if err := checkNoUnhandledCriticalExtension(authority, where, "in the "+role+" CA bundle"); err != nil {
			return nil, nil, err
		}
		pool.AddCert(authority)
		authorities = append(authorities, authority)
	}
	if len(authorities) == 0 {
		return nil, nil, fmt.Errorf("%w: %s certificate authority bundle holds no certificate", ErrIdentity, role)
	}
	return pool, authorities, nil
}

// checkAuthoritiesUsable is the one rule in a CA bundle that changes without
// the file changing: whether anything in it can still verify a certificate
// today. An expired authority beside a live one is a rotation and stays
// acceptable; a bundle where none is live authenticates nobody.
func checkAuthoritiesUsable(authorities []*x509.Certificate, now time.Time, role string) error {
	for _, authority := range authorities {
		if !now.Before(authority.NotBefore) && now.Before(authority.NotAfter) {
			return nil
		}
	}
	return fmt.Errorf("%w: every certificate authority in the %s CA bundle is outside its validity window", ErrIdentity, role)
}
