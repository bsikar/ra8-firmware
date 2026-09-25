// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"crypto/rand"
	"encoding/hex"
	"net/http"
)

// correlationHeader carries one request's identifier on the way in and on the
// way back out. The implementation contract requires every mutation to define
// a correlation ID alongside its body limit, idempotency scope and
// retryability; without one, an operator holding a 503 has no thread back to
// the audit rows and server output that request produced.
const correlationHeader = "X-Correlation-Id"

// correlationIDBytes is the minted length in bytes before hex encoding. A
// request identifier only has to stay distinct inside a bounded window of
// traffic, and 128 bits does that with room to spare.
const correlationIDBytes = 16

// maxCorrelationID bounds an identifier a caller supplied. Long enough for a
// UUID with braces or a build identifier carried down from CI, short enough
// that it can be read in a line of output.
const maxCorrelationID = 64

// withCorrelation gives every request an identifier and returns it on the
// response before the handler runs, so a body written by any path (a problem
// document, a normal JSON reply, or nothing at all) carries the same thread an
// operator can search for.
//
// A caller may supply its own identifier, which is what lets one CI job
// correlate the requests it made, but only a well-formed one is echoed. An
// identifier is never an authorization input and never names a principal: it
// is a thread through the output, and treating a client-supplied string as
// anything more would make it worth forging.
func withCorrelation(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		id := r.Header.Get(correlationHeader)
		if !validCorrelationID(id) {
			id = newCorrelationID()
		}
		w.Header().Set(correlationHeader, id)
		next.ServeHTTP(w, r)
	})
}

// validCorrelationID accepts the unreserved URL characters and nothing else.
// A supplied identifier is written into a response header and read back out of
// server output, so a value carrying a newline, a control byte, a space or a
// non-ASCII rune is replaced rather than reflected: reflecting it would let a
// caller choose what a later reader sees around it.
func validCorrelationID(id string) bool {
	if len(id) == 0 || len(id) > maxCorrelationID {
		return false
	}
	for i := 0; i < len(id); i++ {
		c := id[i]
		switch {
		case c >= 'a' && c <= 'z', c >= 'A' && c <= 'Z', c >= '0' && c <= '9':
		case c == '-', c == '_', c == '.', c == '~':
		default:
			return false
		}
	}
	return true
}

// newCorrelationID mints an identifier. rand.Read is documented never to
// return an error, and an identifier is not a secret: a failure here must not
// be able to refuse a request that would otherwise have been served.
func newCorrelationID() string {
	var raw [correlationIDBytes]byte
	if _, err := rand.Read(raw[:]); err != nil {
		return ""
	}
	return hex.EncodeToString(raw[:])
}
