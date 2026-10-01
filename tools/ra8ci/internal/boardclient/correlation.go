// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardclient

import (
	"context"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/correlate"
)

// correlationHeader carries one request's identifier out to the server and
// back. Without sending or reading one, a holder that fails at a bench can
// report a status and a detail string and nothing an operator can search the
// server for.
//
// The rule behind it lives in internal/correlate, shared with the server, so
// an identifier this client is willing to send is one the server is willing
// to answer with rather than replace.
const correlationHeader = correlate.Header

// maxCorrelationID bounds an identifier a caller pinned.
const maxCorrelationID = correlate.MaxID

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
	pinned, ok := correlate.WithID(ctx, id)
	if !ok {
		return pinned, ErrInvalidRequest
	}
	return pinned, nil
}

// CorrelationIDFrom reports the identifier pinned to ctx, or "" when none is.
func CorrelationIDFrom(ctx context.Context) string { return correlate.IDFrom(ctx) }

// validCorrelationID accepts the unreserved URL characters and nothing else,
// the same rule the server applies before echoing a supplied identifier.
func validCorrelationID(id string) bool { return correlate.Valid(id) }

// newCorrelationID mints an identifier, or "" when the system source of
// randomness fails, in which case the header is omitted and the server mints
// the thread instead.
func newCorrelationID() string { return correlate.New() }

// servedCorrelationID reports the thread the SERVER used for a response: the
// header it actually adopted first, the problem document second, and never
// what this client sent.
func servedCorrelationID(header, body string) string { return correlate.Served(header, body) }
