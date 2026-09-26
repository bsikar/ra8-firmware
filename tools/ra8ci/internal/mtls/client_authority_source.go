// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package mtls

import (
	"crypto/x509"
	"time"
)

// ClientAuthoritySource parses the bundle of authorities a listener trusts
// client certificates from, and hands back two things: the pool it verifies
// with, and the same question asked again at every handshake, can this bundle
// still authenticate anyone.
//
// This is the third and last of the startup answers the API server was
// holding for the life of the process. ClientIdentitySource closed it for the
// long-lived clients and ServerIdentitySource for the listener's own
// certificate; the authorities the listener verifies clients against age in
// exactly the same way and were still decided once, as the socket opened.
//
// ClientAuthorities states the rule and its doc comment says why: a bundle
// holding nothing that can verify anything today refuses every client at the
// handshake, and that reaches the operator as a denial indistinguishable from
// a missing grant. Deciding it before the socket opens is what makes the
// refusal legible. An authority outlives a leaf by years, so the window this
// leaves open is not narrow, it is the whole life of the CA: a server started
// the week before the root expires runs on past it, and on the day it lapses
// every client is denied at once, with nothing on this side saying the trust
// file ran out rather than the grants being pulled.
//
// The check is the bundle-level one parseAuthorities already makes, asked
// again per handshake. It deliberately says nothing about the chain a
// particular client presents; whether THAT certificate verifies is the
// verifier's question and stays with it.
//
// now may be nil, which means time.Now. The bundle is parsed once here rather
// than per handshake; the re-check reads the parsed authorities.
func ClientAuthoritySource(bundle []byte, now func() time.Time) (*x509.CertPool, func() error, error) {
	if now == nil {
		now = time.Now
	}
	pool, authorities, err := parseAuthorityBundle(bundle, "client")
	if err != nil {
		return nil, nil, err
	}
	// Refuse at construction as well as at each handshake. A listener whose
	// trust file can authenticate nobody should not open a socket it can only
	// deny connections on.
	if err := checkAuthoritiesUsable(authorities, now(), "client"); err != nil {
		return nil, nil, err
	}
	held := authorities
	return pool, func() error { return checkAuthoritiesUsable(held, now(), "client") }, nil
}
