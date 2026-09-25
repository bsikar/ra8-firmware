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
	if len(bundle) == 0 {
		return nil, fmt.Errorf("%w: %s certificate authority bundle is empty", ErrIdentity, role)
	}
	pool := x509.NewCertPool()
	rest := bundle
	parsed := 0
	usable := 0
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
			return nil, fmt.Errorf("%w: parse %s certificate authority: %v", ErrIdentity, role, err)
		}
		parsed++
		where := fmt.Sprintf("subject %q sha256 %s", authority.Subject.String(), Fingerprint(authority))
		if !authority.IsCA {
			return nil, fmt.Errorf("%w: %s in the %s CA bundle is not a certificate authority", ErrIdentity, where, role)
		}
		if err := checkAuthorityCanSign(authority, where, role); err != nil {
			return nil, err
		}
		pool.AddCert(authority)
		if !now.Before(authority.NotBefore) && now.Before(authority.NotAfter) {
			usable++
		}
	}
	if parsed == 0 {
		return nil, fmt.Errorf("%w: %s certificate authority bundle holds no certificate", ErrIdentity, role)
	}
	if usable == 0 {
		return nil, fmt.Errorf("%w: every certificate authority in the %s CA bundle is outside its validity window", ErrIdentity, role)
	}
	return pool, nil
}
