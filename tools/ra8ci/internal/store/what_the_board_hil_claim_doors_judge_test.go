//go:build integration

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

// What the two board HIL claim doors judge before they touch anything.
//
// Both doors open with one long guard, and a guard written as a single
// chain of ors is easy to break silently: drop a clause and nothing fails,
// because every other clause still refuses the obvious cases. So each
// field is spoiled on its own against arguments that are otherwise sound,
// and a sound call is run through the same door to prove the guard is not
// simply refusing everything.
//
// THESE CANNOT BE WRITTEN AGAINST A ZERO-VALUE STORE. Unlike the agent
// doors, both guards include s.pool == nil in the same chain, so &Store{}
// returns ErrInvalid no matter what else is passed and every case below
// would pass while proving nothing. They need a real plane.

// a catalog that exists but cannot name itself, which is the one bad
// definitions value a caller can hold without holding nil.
type digestlessHILCatalog struct{}

func (digestlessHILCatalog) Digest() string { return "" }

func (digestlessHILCatalog) Task(string) (catalog.Task, bool) { return catalog.Task{}, false }

func TestIntegrationWhatTheBoardHILClaimDoorsJudge(t *testing.T) {
	st, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	boardID := "board-" + mustID(t)
	sound := boardTestActor(t, ctx, st, pool, boardID, "board_agent", "board_agent")
	definitions, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	taskID := mustID(t)
	leaseID := mustID(t)
	trustedCommit := strings.Repeat("a", 40)

	// Every actor shape both doors refuse, spoiled one field at a time.
	badActors := map[string]func(*BoardActor){
		"a principal that is not a board agent": func(a *BoardActor) { a.kind = "agent" },
		"a board agent acting in another role":  func(a *BoardActor) { a.role = "operator" },
		"no board named at all":                 func(a *BoardActor) { a.boardID = "" },
		"a board name padded with spaces":       func(a *BoardActor) { a.boardID = " " + boardID + " " },
		"a board name past its length":          func(a *BoardActor) { a.boardID = strings.Repeat("b", 129) },
	}

	t.Run("the task claim door", func(t *testing.T) {
		for name, spoil := range badActors {
			t.Run(name, func(t *testing.T) {
				actor := sound
				spoil(&actor)
				if _, err := st.StartBoardHILAttempt(ctx, actor, taskID, leaseID, testStart(taskID)); !errors.Is(err, ErrInvalid) {
					t.Fatalf("the door took %s: %v", name, err)
				}
			})
		}
		for name, args := range map[string][2]string{
			"no task named":                     {"", leaseID},
			"a task that is not an identifier":  {"task-7", leaseID},
			"no lease named":                    {taskID, ""},
			"a lease that is not an identifier": {taskID, "lease-7"},
		} {
			t.Run(name, func(t *testing.T) {
				if _, err := st.StartBoardHILAttempt(ctx, sound, args[0], args[1], testStart(args[0])); !errors.Is(err, ErrInvalid) {
					t.Fatalf("the door took %s: %v", name, err)
				}
			})
		}
		t.Run("sound arguments get past the guard", func(t *testing.T) {
			// There is no such lease, so this cannot succeed. What it
			// must not be is the shape refusal: anything else means
			// the guard let it through and the plane judged it on
			// what it found.
			if _, err := st.StartBoardHILAttempt(ctx, sound, taskID, leaseID, testStart(taskID)); errors.Is(err, ErrInvalid) {
				t.Fatalf("sound arguments were refused on their shape: %v", err)
			}
		})
	})

	t.Run("the queue claim door", func(t *testing.T) {
		for name, spoil := range badActors {
			t.Run(name, func(t *testing.T) {
				actor := sound
				spoil(&actor)
				if _, err := st.ClaimNextBoardHILAttempt(ctx, actor, leaseID, testStart(taskID), definitions, trustedCommit); !errors.Is(err, ErrInvalid) {
					t.Fatalf("the door took %s: %v", name, err)
				}
			})
		}
		t.Run("no lease named", func(t *testing.T) {
			if _, err := st.ClaimNextBoardHILAttempt(ctx, sound, "", testStart(taskID), definitions, trustedCommit); !errors.Is(err, ErrInvalid) {
				t.Fatalf("the door took a claim with no lease: %v", err)
			}
		})
		t.Run("no catalog to judge the work against", func(t *testing.T) {
			if _, err := st.ClaimNextBoardHILAttempt(ctx, sound, leaseID, testStart(taskID), nil, trustedCommit); !errors.Is(err, ErrInvalid) {
				t.Fatalf("the door took a claim with no catalog: %v", err)
			}
		})
		t.Run("a catalog that cannot name itself", func(t *testing.T) {
			// Not nil, but it cannot be pinned to a revision, so an
			// attempt claimed under it could never be shown to have
			// run the definition anybody expected.
			if _, err := st.ClaimNextBoardHILAttempt(ctx, sound, leaseID, testStart(taskID), digestlessHILCatalog{}, trustedCommit); !errors.Is(err, ErrInvalid) {
				t.Fatalf("the door took a claim against an unnamed catalog: %v", err)
			}
		})
		for name, commit := range map[string]string{
			"no commit named":          "",
			"a commit one short":       strings.Repeat("a", 39),
			"a commit one long":        strings.Repeat("a", 41),
			"a commit in upper case":   strings.Repeat("A", 40),
			"a commit that is not hex": strings.Repeat("z", 40),
		} {
			t.Run(name, func(t *testing.T) {
				if _, err := st.ClaimNextBoardHILAttempt(ctx, sound, leaseID, testStart(taskID), definitions, commit); !errors.Is(err, ErrInvalid) {
					t.Fatalf("the door took %s: %v", name, err)
				}
			})
		}
		t.Run("sound arguments get past the guard", func(t *testing.T) {
			if _, err := st.ClaimNextBoardHILAttempt(ctx, sound, leaseID, testStart(taskID), definitions, trustedCommit); errors.Is(err, ErrInvalid) {
				t.Fatalf("sound arguments were refused on their shape: %v", err)
			}
		})
	})
}
