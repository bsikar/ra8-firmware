// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardclient

import (
	"context"
	"crypto/rand"
	"encoding/hex"
)

// correlationHeader carries one request's identifier out to the server and
// back. The server gives every request a thread through its output and its
// audit rows; without sending or reading one, a holder that fails at a bench
// can report a status and a detail string and nothing an operator can search
// the server for.
const correlationHeader = "X-Correlation-Id"

// correlationIDBytes is the minted length in bytes before hex encoding, the
// same 128 bits the server mints when a caller supplies nothing.
const correlationIDBytes = 16

// maxCorrelationID bounds an identifier a caller pinned. It matches the
// server's own bound so an identifier this client accepts is one the server
// will echo rather than replace.
const maxCorrelationID = 64

type correlationKey struct{}

// WithCorrelationID pins one identifier to every request made under the
// returned context, which is what lets a CI job or a multi-step operator
// command tie the requests it made to each other. Without it each request
// carries its own freshly minted thread.
//
// An identifier the server would refuse to echo is refused HERE, loudly,
// rather than sent: a caller that believes it pinned a thread and silently
// got a different one on every request is worse off than one told its
// identifier was unusable.
func WithCorrelationID(ctx context.Context, id string) (context.Context, error) {
	if ctx == nil || !validCorrelationID(id) {
		return ctx, ErrInvalidRequest
	}
	return context.WithValue(ctx, correlationKey{}, id), nil
}

// CorrelationIDFrom reports the identifier pinned to ctx, or "" when none is.
func CorrelationIDFrom(ctx context.Context) string {
	if ctx == nil {
		return ""
	}
	id, _ := ctx.Value(correlationKey{}).(string)
	return id
}

// validCorrelationID accepts the unreserved URL characters and nothing else,
// the same rule the server applies before echoing a supplied identifier.
// Keeping the two rules identical is the point: anything this client is
// willing to send is something the server is willing to answer with.
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

// newCorrelationID mints an identifier. An identifier is not a secret and a
// failure to mint one must never refuse a request that would otherwise have
// been sent, so a failure yields "" and the server mints the thread instead.
func newCorrelationID() string {
	var raw [correlationIDBytes]byte
	if _, err := rand.Read(raw[:]); err != nil {
		return ""
	}
	return hex.EncodeToString(raw[:])
}

// servedCorrelationID reports the thread the SERVER used for a response.
//
// The header is authoritative because the server writes it with the value it
// actually adopted, which is not always the value sent: a malformed
// identifier is replaced. The problem document is read only as a fallback,
// for a response whose headers did not survive whatever sits between the two.
// What this client sent is never used as the answer: an identifier that came
// back from nowhere would name a request the server may never have recorded.
func servedCorrelationID(header, body string) string {
	if validCorrelationID(header) {
		return header
	}
	if validCorrelationID(body) {
		return body
	}
	return ""
}
