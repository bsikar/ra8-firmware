// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package mtls

import (
	"crypto/x509"
	"time"
)

// ServerAuthoritySource parses the PEM bundle a client trusts the server from
// and hands back two things: the pool it verifies with, and the same question
// asked again whenever the caller is about to use it, can this bundle still
// authenticate the server.
//
// This is the mirror of ClientAuthoritySource, and it closes the last startup
// answer a long-lived process in this tree was holding for its whole life. The
// listener got all three of its own: ServerIdentitySource for the certificate
// it presents, ClientIdentitySource for the identity the long-lived clients
// present, ClientAuthoritySource for the authorities it verifies clients
// against. The authorities a CLIENT verifies the SERVER against were still
// decided once, at construction, by every process that holds a client open:
// the agent polls until cancelled, the board client polls on an interval, the
// run client is built once per command and reused across a run.
//
// An authority outlives a leaf by years, so the window this leaves open is not
// narrow, it is the whole remaining life of the CA. What the bench sees on the
// day the root lapses is the failure this package exists to replace, and worse
// than the others because it points the wrong way: verification of the
// server's chain fails, so the error names the SERVER's certificate and the
// operator goes and reads the listener's log, where nothing is wrong. Nothing
// says the trust file on this host ran out.
//
// The check is the bundle-level one parseAuthorities already makes, asked
// again on demand. It deliberately says nothing about the chain the server
// actually presents; whether THAT certificate verifies is the TLS stack's
// question and stays with it.
//
// now may be nil, which means time.Now. The bundle is parsed once here rather
// than per ask; the re-check reads the parsed authorities.
func ServerAuthoritySource(bundle []byte, now func() time.Time) (*x509.CertPool, func() error, error) {
	if now == nil {
		now = time.Now
	}
	pool, authorities, err := parseAuthorityBundle(bundle, "server")
	if err != nil {
		return nil, nil, err
	}
	// Refuse at construction as well as on demand. A client started with a
	// trust file that can authenticate nobody should say so before it opens a
	// socket, not on its first request.
	if err := checkAuthoritiesUsable(authorities, now(), "server"); err != nil {
		return nil, nil, err
	}
	held := authorities
	return pool, func() error { return checkAuthoritiesUsable(held, now(), "server") }, nil
}
