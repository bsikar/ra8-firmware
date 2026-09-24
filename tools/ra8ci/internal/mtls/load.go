// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package mtls

import (
	"crypto/tls"
	"fmt"
	"time"
)

// LoadClientIdentity reads a key pair from disk and refuses one this process
// must not present as its own client identity.
//
// tls.LoadX509KeyPair on its own answers one question: does this key match
// this certificate. Every caller in this tree that loads an identity has to
// ask the rest of the questions too, and a caller that forgets gets an opaque
// handshake failure at the first request instead of a refusal naming the
// certificate. Pairing the load with the check in one call is what keeps a
// new call site from starting out with only half of it.
func LoadClientIdentity(certFile, keyFile string, now time.Time) (tls.Certificate, error) {
	if certFile == "" || keyFile == "" {
		return tls.Certificate{}, fmt.Errorf("%w: a certificate path and a key path are both required", ErrIdentity)
	}
	identity, err := tls.LoadX509KeyPair(certFile, keyFile)
	if err != nil {
		// The error from the TLS package names the two paths, never the key
		// bytes, so it is safe to carry into an operator-facing message.
		return tls.Certificate{}, fmt.Errorf("%w: load key pair: %v", ErrIdentity, err)
	}
	if err := ValidateClientIdentity(identity, now); err != nil {
		return tls.Certificate{}, err
	}
	return identity, nil
}
