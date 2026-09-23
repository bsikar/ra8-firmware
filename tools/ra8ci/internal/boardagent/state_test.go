// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"errors"
	"os"
	"path/filepath"
	"sync"
	"testing"
)

func newTestHighWater(t *testing.T) (*FileHighWater, string) {
	t.Helper()
	directory := t.TempDir()
	if err := os.Chmod(directory, 0o700); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(directory, "generation.state")
	state, err := NewFileHighWater(path, "ek-ra8d2")
	if err != nil {
		t.Fatal(err)
	}
	return state, path
}

func TestFileHighWaterIsDurableMonotonicAndBoardBound(t *testing.T) {
	state, path := newTestHighWater(t)
	if value, err := state.Load(); err != nil || value != 0 {
		t.Fatalf("new high-water=%d err=%v", value, err)
	}
	if err := state.Advance(4); err != nil {
		t.Fatal(err)
	}
	if err := state.Advance(4); err != nil {
		t.Fatalf("idempotent generation write failed: %v", err)
	}
	if err := state.Advance(3); !errors.Is(err, ErrGenerationRollback) {
		t.Fatalf("generation rollback accepted: %v", err)
	}
	reopened, err := NewFileHighWater(path, "ek-ra8d2")
	if err != nil {
		t.Fatal(err)
	}
	if value, err := reopened.Load(); err != nil || value != 4 {
		t.Fatalf("reopened high-water=%d err=%v", value, err)
	}
	info, err := os.Stat(path)
	if err != nil || info.Mode().Perm() != 0o600 {
		t.Fatalf("state permissions=%v err=%v", info.Mode().Perm(), err)
	}
	if _, err := NewFileHighWater(path, "another-board"); err != nil {
		t.Fatal(err)
	} else if value, err := func() (uint64, error) {
		other, _ := NewFileHighWater(path, "another-board")
		return other.Load()
	}(); !errors.Is(err, ErrUnsafeState) || value != 0 {
		t.Fatalf("state accepted under another board identity: %d %v", value, err)
	}
}

func TestFileHighWaterRejectsSymlinkWritableAndMalformedState(t *testing.T) {
	state, path := newTestHighWater(t)
	if err := state.Advance(2); err != nil {
		t.Fatal(err)
	}
	link := path + ".link"
	if err := os.Symlink(path, link); err != nil {
		t.Fatal(err)
	}
	if _, err := NewFileHighWater(link, "ek-ra8d2"); !errors.Is(err, ErrUnsafeState) {
		t.Fatalf("symlink state accepted: %v", err)
	}
	if err := os.Chmod(path, 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := state.Load(); !errors.Is(err, ErrUnsafeState) {
		t.Fatalf("world-readable state accepted: %v", err)
	}
	if err := os.Chmod(path, 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte("schema_version=1\nboard_id=ek-ra8d2\nboard_id=ek-ra8d2\nhigh_water=2\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := state.Load(); !errors.Is(err, ErrUnsafeState) {
		t.Fatalf("duplicate state field accepted: %v", err)
	}
}

func TestNewFileHighWaterRequiresPrivateDirectory(t *testing.T) {
	directory := t.TempDir()
	if err := os.Chmod(directory, 0o755); err != nil {
		t.Fatal(err)
	}
	if _, err := NewFileHighWater(filepath.Join(directory, "generation.state"), "ek-ra8d2"); !errors.Is(err, ErrUnsafeState) {
		t.Fatalf("public state directory accepted: %v", err)
	}
}

func TestIndependentStoresCannotRegressGeneration(t *testing.T) {
	first, path := newTestHighWater(t)
	second, err := NewFileHighWater(path, "ek-ra8d2")
	if err != nil {
		t.Fatal(err)
	}
	var wait sync.WaitGroup
	for generation := uint64(1); generation <= 50; generation++ {
		generation := generation
		wait.Add(1)
		go func() {
			defer wait.Done()
			store := first
			if generation%2 == 0 {
				store = second
			}
			if err := store.Advance(generation); err != nil && !errors.Is(err, ErrGenerationRollback) {
				t.Errorf("advance generation %d: %v", generation, err)
			}
		}()
	}
	wait.Wait()
	value, err := first.Load()
	if err != nil || value != 50 {
		t.Fatalf("concurrent stores regressed to %d: %v", value, err)
	}
}
