// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"errors"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/hilspec"
)

// What the HIL-history and offline-sync doors judge before the database, and
// which of the two refusals a caller gets.
//
// These run against unreachablePlane (what_every_runner_vm_door_judges_test.go):
// the pool is built but points nowhere, so an argument term is the only thing
// that can answer invalid and a well-formed call goes past it and fails
// reaching the database instead.
//
// The offline-sync pair is the one place in this package where the two
// refusals are deliberately NOT the same: a plane with no pool answers
// unavailable, because an offline client that synced against a half-built
// server has not sent anything wrong and should retry rather than discard its
// receipt, while a malformed receipt answers invalid and must never be
// retried. Both arms are pinned below.

func TestAnObservationNamesItsAttemptActorAndStep(t *testing.T) {
	plane, ctx := unreachablePlane(t), context.Background()
	whole := HILObservationInput{
		AttemptID: doorReservation, ActorID: "board-agent-1", StepKey: "uart-scrape",
	}
	for name, bend := range map[string]func(*HILObservationInput){
		"no attempt":                func(in *HILObservationInput) { in.AttemptID = "" },
		"an attempt that is a name": func(in *HILObservationInput) { in.AttemptID = "attempt-1" },
		"no actor":                  func(in *HILObservationInput) { in.ActorID = "" },
		"an actor past its column":  func(in *HILObservationInput) { in.ActorID = strings.Repeat("a", 257) },
		// A padded actor is refused rather than trimmed: the column is
		// the identity a later query matches on, so the plane must not
		// invent a second spelling of the same agent.
		"a padded actor":          func(in *HILObservationInput) { in.ActorID = " board-agent-1" },
		"a trailing-padded actor": func(in *HILObservationInput) { in.ActorID = "board-agent-1 " },
		"no step":                 func(in *HILObservationInput) { in.StepKey = "" },
		"a step past its column":  func(in *HILObservationInput) { in.StepKey = strings.Repeat("s", 129) },
	} {
		in := whole
		bend(&in)
		refusedBefore(t, name, plane.RecordHILObservation(ctx, in))
	}

	refusedBefore(t, "no context", plane.RecordHILObservation(nil, whole)) //nolint:staticcheck // the nil is the case
	reached(t, "a whole observation", plane.RecordHILObservation(ctx, whole))

	stepAtBound := whole
	stepAtBound.StepKey = strings.Repeat("s", 128)
	reached(t, "a step at its bound", plane.RecordHILObservation(ctx, stepAtBound))
}

// The history read is the cohort key itself, so a workload that is not an
// exact cohort is refused rather than turned into a query that would match a
// different board or a different program.
func TestAHistoryReadStatesAWholeCohort(t *testing.T) {
	plane, ctx := unreachablePlane(t), context.Background()
	whole := hilspec.Workload{
		ManifestPath:    "examples/ek_ra8d2/hw_validated/hil/demo/hil.conf",
		BoardModel:      "EK-RA8D2",
		FixtureRevision: "fixture-v1",
		ProfileSHA256:   strings.Repeat("a", 64),
		ProgramFamily:   "uart-demo",
		Mode:            hilspec.ModeUARTScrape,
	}

	_, err := plane.Observations(ctx, hilspec.Workload{})
	refusedBefore(t, "an empty cohort", err)
	bent := whole
	bent.ManifestPath = "docs/hil.conf"
	_, err = plane.Observations(ctx, bent)
	refusedBefore(t, "a manifest outside examples", err)
	bent = whole
	bent.Mode = hilspec.Mode("teleport")
	_, err = plane.Observations(ctx, bent)
	refusedBefore(t, "a mode nobody runs", err)
	bent = whole
	bent.ProfileSHA256 = strings.Repeat("A", 64)
	_, err = plane.Observations(ctx, bent)
	refusedBefore(t, "an uppercase profile digest", err)

	_, err = plane.Observations(nil, whole) //nolint:staticcheck // the nil is the case
	refusedBefore(t, "no context", err)
	_, err = plane.Observations(ctx, whole)
	reached(t, "a whole cohort", err)
}

// An offline client's receipt lookup and its ingest both answer unavailable
// when the plane has no pool and invalid when the receipt is malformed. The
// difference is the whole point: the first is worth retrying and the second
// never is.
func TestAnOfflineSyncSeparatesRetryFromRefusal(t *testing.T) {
	ctx := context.Background()
	half := &Store{}

	_, err := half.LookupLocalRunReceipt(ctx, "principal", strings.Repeat("a", 32), strings.Repeat("a", 64))
	if !errors.Is(err, ErrUnavailable) {
		t.Fatalf("a lookup against a plane with no store: err %v, want ErrUnavailable", err)
	}
	_, err = half.IngestLocalRun(ctx, localRun())
	if !errors.Is(err, ErrUnavailable) {
		t.Fatalf("an ingest against a plane with no store: err %v, want ErrUnavailable", err)
	}
	// And the same half-built plane must not answer unavailable to a
	// malformed receipt, or an offline client would retry forever. The
	// pool is judged first here, so this is the ordering that decides it.
	_, err = half.LookupLocalRunReceipt(ctx, "", "", "")
	if !errors.Is(err, ErrUnavailable) {
		t.Fatalf("the pool is judged ahead of the arguments: err %v, want ErrUnavailable", err)
	}

	plane := unreachablePlane(t)
	local, payload := strings.Repeat("a", 32), strings.Repeat("a", 64)
	for _, c := range []struct{ name, principal, local, payload string }{
		{"no principal", "", local, payload},
		{"a principal past its column", strings.Repeat("p", 257), local, payload},
		{"no local identifier", "principal", "", payload},
		{"a local identifier that is a name", "principal", "run-1", payload},
		{"a short local identifier", "principal", strings.Repeat("a", 31), payload},
		{"an uppercase local identifier", "principal", strings.Repeat("A", 32), payload},
		{"no payload digest", "principal", local, ""},
		{"a commit-sized payload digest", "principal", local, strings.Repeat("a", 40)},
	} {
		_, err := plane.LookupLocalRunReceipt(ctx, c.principal, c.local, c.payload)
		refusedBefore(t, c.name, err)
	}
	_, err = plane.LookupLocalRunReceipt(ctx, strings.Repeat("p", 256), local, payload)
	reached(t, "a principal at its bound", err)
	_, err = plane.LookupLocalRunReceipt(ctx, "principal", local, payload)
	reached(t, "a stated receipt", err)

	// The ingest defers to validateLocalRun, which is pinned field by
	// field elsewhere. What this adds is that the door consults it at all.
	bent := localRun()
	bent.Result = "abandoned"
	_, err = plane.IngestLocalRun(ctx, bent)
	refusedBefore(t, "a result nobody reports", err)
	_, err = plane.IngestLocalRun(ctx, localRun())
	reached(t, "a whole local run", err)
}
