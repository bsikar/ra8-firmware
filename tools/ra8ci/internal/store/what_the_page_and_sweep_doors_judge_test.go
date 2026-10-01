// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"errors"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

// A page door judges its configuration first and answers ErrUnavailable bare,
// so a reader never reads "your request was wrong" out of a plane that was
// never opened. The Terraform state backend judges the opposite way round
// (an argument is refused before the state key is consulted), and the two
// orderings are both deliberate: a page request comes from a client that can
// fix its own parameters, and it can only be told they are wrong by a plane
// that actually looked at them.
func TestAPageDoorJudgesItsConfigurationAheadOfItsArguments(t *testing.T) {
	unopened := &Store{}

	if _, err := unopened.RunEvents(context.Background(), "not-an-id", -7, 9999); !errors.Is(err, ErrUnavailable) {
		t.Fatalf("a run event page from an unopened plane: want ErrUnavailable, got %v", err)
	} else if errors.Is(err, ErrInvalid) {
		t.Fatalf("an unopened plane judged the page parameters: %v", err)
	}

	if _, err := unopened.AttemptLogs(context.Background(), "not-an-id", "also-not", -7, 9999); !errors.Is(err, ErrUnavailable) {
		t.Fatalf("an attempt log page from an unopened plane: want ErrUnavailable, got %v", err)
	} else if errors.Is(err, ErrInvalid) {
		t.Fatalf("an unopened plane judged the log page parameters: %v", err)
	}
}

// Every run event page request is bounded before a query is spent. The cursor
// cannot run backwards and the page cannot exceed MaxEventPageSize, which is
// what keeps one client from asking for the whole log in a single response.
func TestEveryRunEventPageRequestIsBounded(t *testing.T) {
	plane := unreachablePlane(t)

	for _, refusal := range []struct {
		what  string
		runID string
		after int64
		limit int
	}{
		{"a run identifier that is not canonical", "not-a-run", 0, 1},
		{"a run identifier of the wrong version", "01996f90-3415-4cfe-8ff1-600058131b10", 0, 1},
		{"an empty run identifier", "", 0, 1},
		{"a cursor before the start of the log", doorReservation, -1, 1},
		{"a page of no events", doorReservation, 0, 0},
		{"a page past the response bound", doorReservation, 0, MaxEventPageSize + 1},
	} {
		_, err := plane.RunEvents(context.Background(), refusal.runID, refusal.after, refusal.limit)
		refusedBefore(t, refusal.what, err)
	}

	for _, accepted := range []struct {
		what  string
		after int64
		limit int
	}{
		{"the first page", 0, 1},
		{"a full page at the bound", 0, MaxEventPageSize},
		{"a page after a cursor", 41, MaxEventPageSize},
	} {
		_, err := plane.RunEvents(context.Background(), doorReservation, accepted.after, accepted.limit)
		reached(t, accepted.what, err)
	}
}

// An attempt log page names both the run and the attempt, so a caller holding
// one run cannot page another run's output by attempt identifier alone. The
// page bound is smaller here because each chunk is base64 in the response.
func TestEveryAttemptLogPageRequestIsBounded(t *testing.T) {
	plane := unreachablePlane(t)

	for _, refusal := range []struct {
		what      string
		runID     string
		attemptID string
		after     int64
		limit     int
	}{
		{"a run identifier that is not canonical", "not-a-run", doorOperation, 0, 1},
		{"an attempt identifier that is not canonical", doorReservation, "not-an-attempt", 0, 1},
		{"no attempt at all", doorReservation, "", 0, 1},
		{"a cursor before the start of the log", doorReservation, doorOperation, -1, 1},
		{"a page of no chunks", doorReservation, doorOperation, 0, 0},
		{"a page past the response bound", doorReservation, doorOperation, 0, MaxLogPageSize + 1},
	} {
		_, err := plane.AttemptLogs(context.Background(), refusal.runID, refusal.attemptID, refusal.after, refusal.limit)
		refusedBefore(t, refusal.what, err)
	}

	for _, accepted := range []struct {
		what  string
		after int64
		limit int
	}{
		{"the first page", 0, 1},
		{"a full page at the bound", 0, MaxLogPageSize},
		{"a page after a cursor", 3, MaxLogPageSize},
	} {
		_, err := plane.AttemptLogs(context.Background(), doorReservation, doorOperation, accepted.after, accepted.limit)
		reached(t, accepted.what, err)
	}
}

// The reaper is the one door that can move many attempts at once, so its
// sweep is bounded and it will not run without the catalog it needs to decide
// whether a fenced attempt is retried. Its pool is judged in the same
// condition as its arguments, so an unopened plane answers ErrInvalid here
// rather than ErrUnavailable: the whole guard is one refusal.
func TestTheReaperRefusesAnUnboundedSweep(t *testing.T) {
	definitions, err := catalog.Load()
	if err != nil {
		t.Fatalf("load catalog: %v", err)
	}
	plane := unreachablePlane(t)

	for _, refusal := range []struct {
		what        string
		definitions *catalog.Catalog
		limit       int
	}{
		{"no catalog to judge a retry against", nil, 1},
		{"a sweep of no attempts", definitions, 0},
		{"a negative sweep", definitions, -1},
		{"a sweep past the batch bound", definitions, 1001},
	} {
		_, err := plane.ReapAgentAssignments(context.Background(), refusal.definitions, refusal.limit)
		refusedBefore(t, refusal.what, err)
	}

	if _, err := (&Store{}).ReapAgentAssignments(context.Background(), definitions, 1); !errors.Is(err, ErrInvalid) {
		t.Fatalf("an unopened plane reaping: want ErrInvalid, got %v", err)
	}

	for _, accepted := range []struct {
		what  string
		limit int
	}{
		{"a single attempt", 1},
		{"a sweep at the batch bound", 1000},
	} {
		_, err := plane.ReapAgentAssignments(context.Background(), definitions, accepted.limit)
		reached(t, accepted.what, err)
	}
}

// The approved fixture profile is the operator's word on what a bench is, so
// the read names exactly one board and refuses a padded identifier rather
// than trimming it into a different board's name.
func TestAFixtureProfileReadNamesOneBoard(t *testing.T) {
	plane := unreachablePlane(t)

	for _, refusal := range []struct {
		what    string
		ctx     context.Context
		boardID string
	}{
		{"no context", nil, doorBoard},
		{"no board", context.Background(), ""},
		{"a board named only in whitespace", context.Background(), " "},
		{"a padded board identifier", context.Background(), " " + doorBoard},
		{"a trailing newline on the identifier", context.Background(), doorBoard + "\n"},
		{"a board identifier past the column bound", context.Background(), strings.Repeat("b", 129)},
	} {
		_, err := plane.ApprovedBoardFixtureProfile(refusal.ctx, refusal.boardID)
		refusedBefore(t, refusal.what, err)
	}

	for _, accepted := range []struct {
		what    string
		boardID string
	}{
		{"a registered bench", doorBoard},
		{"an identifier exactly at the column bound", strings.Repeat("b", 128)},
	} {
		_, err := plane.ApprovedBoardFixtureProfile(context.Background(), accepted.boardID)
		reached(t, accepted.what, err)
	}
}
