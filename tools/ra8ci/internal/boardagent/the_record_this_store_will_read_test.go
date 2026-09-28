// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"errors"
	"os"
	"strings"
	"testing"
)

// The high-water file is the durable fence a grant is acknowledged behind, so
// a generation that is not a real one never reaches the disk and one that
// moves backward is refused rather than written.
func TestAdvanceRefusesAGenerationItMustNotRecord(t *testing.T) {
	state, _ := newTestHighWater(t)
	if err := state.Advance(0); !errors.Is(err, ErrGenerationInvalid) {
		t.Errorf("generation zero: err = %v, want ErrGenerationInvalid", err)
	}
	var absent *FileHighWater
	if err := absent.Advance(4); !errors.Is(err, ErrGenerationInvalid) {
		t.Errorf("no store: err = %v, want ErrGenerationInvalid", err)
	}
	if _, err := absent.Load(); !errors.Is(err, ErrUnsafeState) {
		t.Errorf("load from no store: err = %v, want ErrUnsafeState", err)
	}

	if err := state.Advance(9); err != nil {
		t.Fatal(err)
	}
	if err := state.Advance(8); !errors.Is(err, ErrGenerationRollback) {
		t.Fatalf("a lower generation: err = %v, want ErrGenerationRollback", err)
	}
	if err := state.Advance(9); err != nil {
		t.Fatalf("re-recording the same generation was refused: %v", err)
	}
	generation, err := state.Load()
	if err != nil || generation != 9 {
		t.Fatalf("generation = %d, err = %v, want 9", generation, err)
	}
}

// The state file's contents are read as a fixed three-field record. Anything
// else is an unsafe state rather than a best-effort parse, because a
// half-understood file would hand back a generation the agent then fences on.
func TestLoadRefusesEveryShapeThatIsNotTheRecord(t *testing.T) {
	sound := "schema_version=1\nboard_id=ek-ra8d2\nhigh_water=3\n"
	for name, body := range map[string]string{
		"empty":                    "",
		"one field":                "high_water=3\n",
		"two fields":               "schema_version=1\nhigh_water=3\n",
		"a fourth field":           sound + "extra=1\n",
		"no separator":             "schema_version 1\nboard_id=ek-ra8d2\nhigh_water=3\n",
		"an empty key":             "=1\nboard_id=ek-ra8d2\nhigh_water=3\n",
		"an empty value":           "schema_version=\nboard_id=ek-ra8d2\nhigh_water=3\n",
		"another schema":           "schema_version=2\nboard_id=ek-ra8d2\nhigh_water=3\n",
		"another board":            "schema_version=1\nboard_id=ek-ra8m1\nhigh_water=3\n",
		"a high water in words":    "schema_version=1\nboard_id=ek-ra8d2\nhigh_water=three\n",
		"a padded high water":      "schema_version=1\nboard_id=ek-ra8d2\nhigh_water=003\n",
		"a signed high water":      "schema_version=1\nboard_id=ek-ra8d2\nhigh_water=+3\n",
		"a high water past uint64": "schema_version=1\nboard_id=ek-ra8d2\nhigh_water=18446744073709551616\n",
		"a blank line":             "schema_version=1\n\nboard_id=ek-ra8d2\nhigh_water=3\n",
		"longer than the bound":    sound + strings.Repeat("padding=x\n", 60),
	} {
		state, path := newTestHighWater(t)
		if err := os.WriteFile(path, []byte(body), 0o600); err != nil {
			t.Fatal(err)
		}
		if _, err := state.Load(); !errors.Is(err, ErrUnsafeState) {
			t.Errorf("%s: err = %v, want ErrUnsafeState", name, err)
		}
	}

	state, path := newTestHighWater(t)
	if err := os.WriteFile(path, []byte(sound), 0o600); err != nil {
		t.Fatal(err)
	}
	generation, err := state.Load()
	if err != nil || generation != 3 {
		t.Fatalf("the record itself: generation = %d, err = %v, want 3", generation, err)
	}
}

// An uninitialized store is zero rather than an error: the first agent to run
// on a board has written nothing yet, and refusing there would stop it before
// it ever acknowledged a grant.
func TestLoadAnswersZeroForAStoreNothingHasWrittenYet(t *testing.T) {
	state, path := newTestHighWater(t)
	if _, err := os.Stat(path); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("the fixture already wrote state: %v", err)
	}
	generation, err := state.Load()
	if err != nil || generation != 0 {
		t.Fatalf("generation = %d, err = %v, want 0", generation, err)
	}
}

// A state file swapped for a directory is not a record, and the store says so
// rather than reading whatever the open happens to return.
func TestLoadRefusesAStatePathThatIsNotAFile(t *testing.T) {
	state, path := newTestHighWater(t)
	if err := os.Mkdir(path, 0o700); err != nil {
		t.Fatal(err)
	}
	if _, err := state.Load(); !errors.Is(err, ErrUnsafeState) {
		t.Fatalf("err = %v, want ErrUnsafeState", err)
	}
}
