// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package mtls

import (
	"crypto/tls"
	"time"
)

// ServerIdentitySource turns a loaded key pair into the callback a TLS server
// config uses to present it, and re-checks the identity at every handshake.
//
// This is the mirror of ClientIdentitySource, and the listener needs it more
// than any client does. LoadServerIdentity decides the question once, as the
// server starts: is this a usable server certificate now. The API server then
// runs for as long as the service is up, weeks at a time, holding that one
// answer. tls.Config.Certificates is presented unexamined for the life of the
// process, so a certificate issued for ninety days passes its NotAfter with
// the listener still serving and still offering it.
//
// What reaches the bench then is the failure this package exists to replace,
// on every client at once: the handshake breaks with a certificate error the
// clients report as the server being unreachable, and nothing on the server
// says its own certificate ran out. The runbook answer is to read the process
// log, and until this callback existed the log had nothing to say, because
// from the listener's point of view nothing had gone wrong.
//
// The callback closes that by asking the same local question the startup
// check asked, at each handshake, so the first connection after expiry is
// refused with the message ValidateServerIdentity already writes: the subject,
// the public fingerprint, and the instant it expired.
//
// now may be nil, which means time.Now. The leaf is parsed once here rather
// than per handshake; the re-check reads the parsed certificate.
func ServerIdentitySource(identity tls.Certificate, now func() time.Time) (func(*tls.ClientHelloInfo) (*tls.Certificate, error), error) {
	if now == nil {
		now = time.Now
	}
	leaf, err := Leaf(identity)
	if err != nil {
		return nil, err
	}
	// Refuse at construction as well as at each handshake. A listener started
	// with an identity it may never present should refuse to open the socket
	// rather than accept connections it cannot complete.
	if err := ValidateServerIdentity(identity, now()); err != nil {
		return nil, err
	}
	held := identity
	held.Leaf = leaf
	return func(*tls.ClientHelloInfo) (*tls.Certificate, error) {
		if err := ValidateServerIdentity(held, now()); err != nil {
			return nil, err
		}
		return &held, nil
	}, nil
}
