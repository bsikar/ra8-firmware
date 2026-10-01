//go:build integration

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"encoding/json"
	"errors"
	"testing"
	"time"
)

// The run a reader may ask for.
//
// A status read answers durable state or refuses. An identifier the plane
// could never have issued is refused before a transaction opens, and an
// identifier nobody holds is not-found, never an empty run that a caller
// could mistake for one still queued.
func TestIntegrationTheRunAReaderMayAskFor(t *testing.T) {
	s, _ := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	t.Run("an identifier the plane never issues", func(t *testing.T) {
		for _, id := range []string{"", "not-a-uuid", "../runs"} {
			if run, err := s.GetRun(ctx, id); !errors.Is(err, ErrInvalid) || run.ID != "" {
				t.Fatalf("GetRun(%q) answered %+v %v", id, run, err)
			}
		}
	})

	t.Run("a run nobody created", func(t *testing.T) {
		if run, err := s.GetRun(ctx, mustID(t)); !errors.Is(err, ErrNotFound) || run.ID != "" || run.Tasks != nil {
			t.Fatalf("an unknown run was answered: %+v %v", run, err)
		}
	})

	t.Run("a run that exists", func(t *testing.T) {
		created, err := s.CreateRun(ctx, childlessRun(t, boardTestRepo))
		if err != nil {
			t.Fatal(err)
		}
		read, err := s.GetRun(ctx, created.ID)
		if err != nil || read.ID != created.ID || read.State != "queued" || read.Tasks == nil {
			t.Fatalf("a created run read back as %+v %v", read, err)
		}
	})
}

// The page of events a reader may ask for.
//
// The page is bounded on both ends before Postgres is asked, and the
// cursor is exclusive, so a reader that resumes from the last sequence it
// saw is never handed that event twice.
func TestIntegrationTheEventPageAReaderMayAskFor(t *testing.T) {
	s, _ := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	run, err := s.CreateRun(ctx, childlessRun(t, boardTestRepo))
	if err != nil {
		t.Fatal(err)
	}

	t.Run("a page the plane will not serve", func(t *testing.T) {
		for name, page := range map[string]struct {
			id    string
			after int64
			limit int
		}{
			"no run":            {"not-a-uuid", 0, 10},
			"a negative cursor": {run.ID, -1, 10},
			"an empty page":     {run.ID, 0, 0},
			"a page over 1000":  {run.ID, 0, 1001},
			"a negative page":   {run.ID, 0, -5},
		} {
			t.Run(name, func(t *testing.T) {
				if items, err := s.GetRunEvents(ctx, page.id, page.after, page.limit); !errors.Is(err, ErrInvalid) || items != nil {
					t.Fatalf("an unservable page was served: %v %v", items, err)
				}
			})
		}
	})

	t.Run("the largest page the plane serves", func(t *testing.T) {
		if _, err := s.GetRunEvents(ctx, run.ID, 0, 1000); err != nil {
			t.Fatalf("a 1000-event page was refused: %v", err)
		}
	})

	t.Run("a run nobody created answers an empty page, not nil", func(t *testing.T) {
		items, err := s.GetRunEvents(ctx, mustID(t), 0, 10)
		if err != nil || items == nil || len(items) != 0 {
			t.Fatalf("an unknown run's page: %v %v", items, err)
		}
	})

	t.Run("the cursor is exclusive", func(t *testing.T) {
		first, err := s.GetRunEvents(ctx, run.ID, 0, 1)
		if err != nil || len(first) != 1 {
			t.Fatalf("the creation event is missing: %v %v", first, err)
		}
		var event struct {
			Seq  int64  `json:"seq"`
			Kind string `json:"kind"`
		}
		if err := json.Unmarshal(first[0], &event); err != nil {
			t.Fatal(err)
		}
		if event.Kind != "run.created" || event.Seq < 1 {
			t.Fatalf("the first event is %+v", event)
		}
		rest, err := s.GetRunEvents(ctx, run.ID, event.Seq, 1000)
		if err != nil {
			t.Fatal(err)
		}
		for _, raw := range rest {
			var later struct {
				Seq int64 `json:"seq"`
			}
			if err := json.Unmarshal(raw, &later); err != nil {
				t.Fatal(err)
			}
			if later.Seq <= event.Seq {
				t.Fatalf("resuming after %d handed back %d", event.Seq, later.Seq)
			}
		}
	})
}
