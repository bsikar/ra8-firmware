// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package correlate holds the one rule for the request identifier that threads
// a client failure to the server output and audit rows that request produced.
//
// The rule lives here because it is only useful while every side agrees on it:
// the server replaces an identifier it will not echo, so a client applying a
// looser rule would quietly send identifiers that never come back, and a
// client applying a stricter one would refuse identifiers the server would
// have honoured. One copy, three callers.
package correlate

import (
	"context"
	"crypto/rand"
	"encoding/hex"
)

// Header carries one request's identifier out and back.
const Header = "X-Correlation-Id"

// IDBytes is the minted length in bytes before hex encoding. An identifier
// only has to stay distinct inside a bounded window of traffic, and 128 bits
// does that with room to spare.
const IDBytes = 16

// MaxID bounds a supplied identifier. Long enough for a UUID with braces or a
// build identifier carried down from CI, short enough to read in a line of
// output.
const MaxID = 64

type key struct{}

// WithID pins one identifier to every request made under the returned context,
// reporting false and leaving ctx alone for an identifier the server would not
// echo. Callers turn that false into their own package's invalid-request
// error: a caller that believes it pinned a thread and silently got a
// different one on every request is worse off than one told so.
func WithID(ctx context.Context, id string) (context.Context, bool) {
	if ctx == nil || !Valid(id) {
		return ctx, false
	}
	return context.WithValue(ctx, key{}, id), true
}

// IDFrom reports the identifier pinned to ctx, or "" when none is.
func IDFrom(ctx context.Context) string {
	if ctx == nil {
		return ""
	}
	id, _ := ctx.Value(key{}).(string)
	return id
}

// Outgoing reports the identifier a request under ctx should carry: the
// pinned one when there is one, a freshly minted one otherwise, so unpinned
// requests stay separately findable. "" means send no header and let the
// server mint the thread; an identifier is not a secret and a failure to mint
// one must never refuse a request that would otherwise have been sent.
func Outgoing(ctx context.Context) string {
	if id := IDFrom(ctx); Valid(id) {
		return id
	}
	return New()
}

// Valid accepts the unreserved URL characters and nothing else. A supplied
// identifier is written into a response header and read back out of server
// output, so a value carrying a newline, a control byte, a space or a
// non-ASCII rune is replaced rather than reflected: reflecting it would let a
// caller choose what a later reader sees around it.
func Valid(id string) bool {
	if len(id) == 0 || len(id) > MaxID {
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

// New mints an identifier, or "" when the system source of randomness fails.
func New() string {
	var raw [IDBytes]byte
	if _, err := rand.Read(raw[:]); err != nil {
		return ""
	}
	return hex.EncodeToString(raw[:])
}

// Served reports the thread the SERVER used for a response.
//
// The header is authoritative because the server writes it with the value it
// actually adopted, which is not always the value sent: a malformed identifier
// is replaced. The problem document is read only as a fallback, for a response
// whose headers did not survive whatever sits between the two. What a client
// sent is never the answer: an identifier that came back from nowhere would
// name a request the server may never have recorded.
func Served(header, body string) string {
	if Valid(header) {
		return header
	}
	if Valid(body) {
		return body
	}
	return ""
}
