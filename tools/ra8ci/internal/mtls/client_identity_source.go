// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package mtls

import (
	"crypto/tls"
	"time"
)

// ClientIdentitySource turns a loaded key pair into the callback a TLS client
// config uses to present it, and re-checks the identity every time it is asked
// for one.
//
// LoadClientIdentity decides the question once, at construction: is this a
// usable client certificate now. That is the whole story for a command that
// makes one request and exits, which is what ra8ci sync and ra8ci report slow
// are. It is half the story for the three processes that hold their client
// open: the agent polls until cancelled, the board client polls on an
// interval, and the run client is built once per command but reused across a
// run. Each of them validated the identity at New and then handed the bare
// certificate to the TLS stack, which presents it unexamined for the life of
// the process.
//
// A certificate issued for a shift, a day, or a week therefore passes its
// NotAfter with the process still running and still presenting it. What the
// operator sees then is the failure this package exists to replace: the
// handshake breaks, or the server refuses the expired certificate, and the
// message reaching the bench is a connection error or a denial that looks
// exactly like a revoked grant. Nothing says the certificate on this host ran
// out twenty minutes ago.
//
// The callback closes that by asking the same local question at each
// handshake, so the first request after expiry fails with the refusal
// ValidateClientIdentity already writes: the subject, the public fingerprint,
// and the instant it expired.
//
// now may be nil, which means time.Now. The leaf is parsed once here rather
// than per handshake; the re-check reads the parsed certificate.
func ClientIdentitySource(identity tls.Certificate, now func() time.Time) (func(*tls.CertificateRequestInfo) (*tls.Certificate, error), error) {
	if now == nil {
		now = time.Now
	}
	leaf, err := Leaf(identity)
	if err != nil {
		return nil, err
	}
	// Refuse at construction as well as at each handshake. A process started
	// with an identity it may never present should say so before it opens a
	// socket, not on its first request.
	if err := ValidateClientIdentity(identity, now()); err != nil {
		return nil, err
	}
	held := identity
	held.Leaf = leaf
	return func(*tls.CertificateRequestInfo) (*tls.Certificate, error) {
		if err := ValidateClientIdentity(held, now()); err != nil {
			return nil, err
		}
		return &held, nil
	}, nil
}
