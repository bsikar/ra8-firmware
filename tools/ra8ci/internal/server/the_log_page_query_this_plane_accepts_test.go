// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"net/url"
	"strconv"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// A log page query is the one request a caller can paginate, so it is the one
// a caller can walk past its window. Everything the query decides is decided
// before the store is asked, which is why it can be pinned on this box at
// all: the refusals below never reach Postgres.

// anAttempt is a well-formed attempt identifier, so a case that is meant to
// fail fails for the reason under test rather than for its identifier.
const anAttempt = "0193a7b1-2c3d-7e4f-8a9b-0c1d2e3f4a5b"

// A query carrying anything the page does not read is refused whole rather
// than filtered down to the keys it recognises. A caller who misspells
// "limit" is asking for a page the plane would otherwise silently widen to
// the maximum.
func TestALogPageQueryCarryingAnUnreadKeyIsRefused(t *testing.T) {
	for _, key := range []string{"cursor", "before", "Limit", "attempt", ""} {
		query := url.Values{"attempt_id": {anAttempt}, key: {"1"}}
		if _, _, _, valid := parseLogQuery(query); valid {
			t.Errorf("a query carrying %q was accepted", key)
		}
	}
}

// Each key is read exactly once. A repeated key is the shape a caller gets
// from stitching two query strings together, and taking the first value
// silently serves a page they did not ask for.
func TestALogPageQueryRepeatingAKeyIsRefused(t *testing.T) {
	for name, query := range map[string]url.Values{
		"attempt_id twice": {"attempt_id": {anAttempt, anAttempt}},
		"after twice":      {"attempt_id": {anAttempt}, "after": {"0", "1"}},
		"limit twice":      {"attempt_id": {anAttempt}, "limit": {"1", "2"}},
	} {
		if _, _, _, valid := parseLogQuery(query); valid {
			t.Errorf("%s was accepted", name)
		}
	}
}

// An attempt identifier is required and is held to the store's own shape, so
// a page is never read for an attempt the store could not have written.
func TestALogPageQueryWithoutAWellFormedAttemptIsRefused(t *testing.T) {
	for _, attempt := range []string{"", "not-an-id", anAttempt + "x", " " + anAttempt} {
		query := url.Values{"attempt_id": {attempt}}
		if _, _, _, valid := parseLogQuery(query); valid {
			t.Errorf("attempt_id=%q was accepted", attempt)
		}
	}
}

// The cursor is a byte offset into a log, so a value that is not a
// non-negative number is refused rather than clamped. Clamping a negative
// cursor to zero would re-serve a log from its start to a caller who is
// paging through it.
func TestALogPageCursorThatIsNotANonNegativeNumberIsRefused(t *testing.T) {
	for _, after := range []string{"", "-1", "1.5", "0x10", "1e3", " 1", "1 ", "99999999999999999999"} {
		query := url.Values{"attempt_id": {anAttempt}, "after": {after}}
		if _, _, _, valid := parseLogQuery(query); valid {
			t.Errorf("after=%q was accepted", after)
		}
	}
	attempt, after, limit, valid := parseLogQuery(url.Values{"attempt_id": {anAttempt}, "after": {"0"}})
	if !valid || attempt != anAttempt || after != 0 || limit != store.MaxLogPageSize {
		t.Fatalf("after=0 gave %q %d %d %t, want the attempt at offset 0 and the full page", attempt, after, limit, valid)
	}
	// A signed positive cursor is taken as the number it spells, because the
	// parse is a base-10 ParseInt and that accepts a leading plus. Pinned as
	// it stands rather than tightened: "+1" and "1" name the same offset, so
	// reading it is not a way past the window the refusals above hold.
	if _, after, _, valid := parseLogQuery(url.Values{"attempt_id": {anAttempt}, "after": {"+1"}}); !valid || after != 1 {
		t.Fatalf(`after="+1" gave %d %t, want it read as offset 1`, after, valid)
	}
}

// The page size is bounded on both sides and the bound is exact, because it
// is what keeps one caller from asking the store for an unbounded read.
func TestALogPageSizeIsBoundedOnBothSidesExactly(t *testing.T) {
	ceiling := store.MaxLogPageSize
	for _, limit := range []string{"0", "-1", "", "1.5", "all", strconv.Itoa(ceiling + 1)} {
		query := url.Values{"attempt_id": {anAttempt}, "limit": {limit}}
		if _, _, _, valid := parseLogQuery(query); valid {
			t.Errorf("limit=%q was accepted", limit)
		}
	}
	for _, limit := range []int{1, ceiling} {
		query := url.Values{"attempt_id": {anAttempt}, "limit": {strconv.Itoa(limit)}}
		_, _, given, valid := parseLogQuery(query)
		if !valid || given != limit {
			t.Errorf("limit=%d gave %d %t, want it accepted as asked", limit, given, valid)
		}
	}
}

// An absent limit is the full page rather than nothing, which is the default
// a caller who passes only an attempt is relying on.
func TestALogPageWithNoLimitIsTheFullPage(t *testing.T) {
	_, _, limit, valid := parseLogQuery(url.Values{"attempt_id": {anAttempt}})
	if !valid || limit != store.MaxLogPageSize {
		t.Fatalf("limit = %d, valid = %t; want the full page of %d", limit, valid, store.MaxLogPageSize)
	}
}

// The two doors in front of the store, over a plane whose store is a zero
// value: reaching the store at all would panic, so answering at all is the
// assertion that neither door consulted it.
func TestTheLogDoorsAnswerBeforeTheStore(t *testing.T) {
	plane := planeWithNoDatabase(t)
	for name, target := range map[string]string{
		"a run ID the store could not have written": "/v1/runs/not-an-id/logs?attempt_id=" + anAttempt,
		"a query the page does not read":            "/v1/runs/" + anAttempt + "/logs?cursor=1",
		"no query at all":                           "/v1/runs/" + anAttempt + "/logs",
	} {
		t.Run(name, func(t *testing.T) {
			got := ask(t, plane, "GET", target)
			if got.status != 400 {
				t.Fatalf("status = %d, want 400 (body %v)", got.status, got.body)
			}
			if got.thread == "" {
				t.Fatalf("no correlation thread on the refusal (body %v)", got.body)
			}
		})
	}
}
