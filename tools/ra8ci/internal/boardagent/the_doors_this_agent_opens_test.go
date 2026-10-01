// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// A reconciler is constructed once and then holds hardware authority for one
// board, so every part of the ask is judged at the door rather than on the
// first tick. An agent built from a wrong board identity, a missing
// dependency, or an interval outside the operable band would otherwise run.
func TestNewRefusesAnAgentItCouldNotOperate(t *testing.T) {
	highWater, _ := newTestHighWater(t)
	client := &testControlClient{}

	if _, err := New("ek-ra8d2", client, highWater, time.Second); err != nil {
		t.Fatalf("a sound agent was refused: %v", err)
	}

	for name, item := range map[string]struct {
		boardID   string
		client    ControlClient
		highWater HighWaterStore
		interval  time.Duration
	}{
		"no board":             {"", client, highWater, time.Second},
		"a board with a space": {"ek ra8d2", client, highWater, time.Second},
		"no client":            {"ek-ra8d2", nil, highWater, time.Second},
		"no high-water store":  {"ek-ra8d2", client, nil, time.Second},
		"no interval":          {"ek-ra8d2", client, highWater, 0},
		"a negative interval":  {"ek-ra8d2", client, highWater, -time.Second},
	} {
		if _, err := New(item.boardID, item.client, item.highWater, item.interval); !errors.Is(err, ErrInvalidAgent) {
			t.Errorf("%s: err = %v, want ErrInvalidAgent", name, err)
		}
	}
}

// The interval band is exact on both sides. A tick faster than 250ms is a
// reconciler hammering the control plane, and one slower than 30s is a lease
// that can expire between observations, so both bounds are pinned at the
// value and one nanosecond past it.
func TestNewHoldsTheIntervalBandExactly(t *testing.T) {
	highWater, _ := newTestHighWater(t)
	client := &testControlClient{}
	for name, interval := range map[string]time.Duration{
		"the fastest allowed": 250 * time.Millisecond,
		"the slowest allowed": 30 * time.Second,
	} {
		if _, err := New("ek-ra8d2", client, highWater, interval); err != nil {
			t.Errorf("%s (%s) was refused: %v", name, interval, err)
		}
	}
	for name, interval := range map[string]time.Duration{
		"one nanosecond too fast": 250*time.Millisecond - time.Nanosecond,
		"one nanosecond too slow": 30*time.Second + time.Nanosecond,
	} {
		if _, err := New("ek-ra8d2", client, highWater, interval); !errors.Is(err, ErrInvalidAgent) {
			t.Errorf("%s (%s): err = %v, want ErrInvalidAgent", name, interval, err)
		}
	}
}

// A board identity reaches a state file name and a server call, so it is held
// to a narrow alphabet, a length bound, and no surrounding space. The bound is
// pinned at 128 exactly and at one character past it.
func TestABoardIdentityIsHeldToANarrowAlphabet(t *testing.T) {
	for _, value := range []string{
		"ek-ra8d2", "EK_RA8D2", "board.1", "b", strings.Repeat("b", 128),
	} {
		if !validBoardID(value) {
			t.Errorf("%q was refused", value)
		}
	}
	for name, value := range map[string]string{
		"empty":              "",
		"one past the bound": strings.Repeat("b", 129),
		"a leading space":    " ek-ra8d2",
		"a trailing space":   "ek-ra8d2 ",
		"a newline":          "ek-ra8d2\n",
		"a path separator":   "ek/ra8d2",
		"a colon":            "ek:ra8d2",
		"a null byte":        "ek-ra8d2\x00",
		"a non-ASCII letter": "ek-ra8dé",
		"only whitespace":    " ",
	} {
		if validBoardID(value) {
			t.Errorf("%s: %q was accepted", name, value)
		}
	}
}

// The state file is the durable half of the fence, so the door refuses
// anything about its location it cannot stand behind: no path at all, a board
// identity the file could not be bound to, and a directory reached through a
// symlink, which is the shape that lets the file be swapped underneath.
func TestNewFileHighWaterRefusesAPlaceItCannotProtect(t *testing.T) {
	directory := t.TempDir()
	if err := os.Chmod(directory, 0o700); err != nil {
		t.Fatal(err)
	}
	sound := filepath.Join(directory, "generation.state")

	if _, err := NewFileHighWater("", "ek-ra8d2"); !errors.Is(err, ErrUnsafeState) {
		t.Errorf("no path: err = %v, want ErrUnsafeState", err)
	}
	if _, err := NewFileHighWater(sound, ""); !errors.Is(err, ErrUnsafeState) {
		t.Errorf("no board: err = %v, want ErrUnsafeState", err)
	}
	if _, err := NewFileHighWater(sound, "ek ra8d2"); !errors.Is(err, ErrUnsafeState) {
		t.Errorf("a board with a space: err = %v, want ErrUnsafeState", err)
	}
	if _, err := NewFileHighWater(filepath.Join(directory, "absent", "generation.state"), "ek-ra8d2"); !errors.Is(err, ErrUnsafeState) {
		t.Errorf("an absent directory: err = %v, want ErrUnsafeState", err)
	}

	linked := filepath.Join(t.TempDir(), "link")
	if err := os.Symlink(directory, linked); err != nil {
		t.Skipf("this box does not make symlinks: %v", err)
	}
	if _, err := NewFileHighWater(filepath.Join(linked, "generation.state"), "ek-ra8d2"); !errors.Is(err, ErrUnsafeState) {
		t.Errorf("a directory reached through a symlink: err = %v, want ErrUnsafeState", err)
	}
}

// A relative path is bound to its absolute form, so two stores named
// differently for the same file are the same file and cannot regress each
// other's generation.
func TestNewFileHighWaterBindsAPathToItsAbsoluteForm(t *testing.T) {
	directory := t.TempDir()
	if err := os.Chmod(directory, 0o700); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(directory, "generation.state")
	direct, err := NewFileHighWater(path, "ek-ra8d2")
	if err != nil {
		t.Fatal(err)
	}
	roundabout, err := NewFileHighWater(filepath.Join(directory, "..", filepath.Base(directory), "generation.state"), "ek-ra8d2")
	if err != nil {
		t.Fatal(err)
	}
	if direct.path != roundabout.path {
		t.Fatalf("the same file was bound twice: %q and %q", direct.path, roundabout.path)
	}
	if err := direct.Advance(7); err != nil {
		t.Fatal(err)
	}
	generation, err := roundabout.Load()
	if err != nil || generation != 7 {
		t.Fatalf("generation = %d, err = %v, want 7", generation, err)
	}
}
