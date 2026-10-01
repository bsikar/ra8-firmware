// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"
)

func leaseHolderFixture() RunnerVM {
	claimed := time.Date(2026, 9, 24, 12, 0, 0, 0, time.UTC)
	_ = claimed
	return RunnerVM{
		ID:                 "11111111-1111-4111-8111-111111111111",
		State:              "registered",
		ExternalRunnerName: "ra8ci-4815162342-1",
		UnclaimedDeadline:  time.Date(2026, 9, 24, 12, 30, 0, 0, time.UTC),
	}
}

func TestUnclaimedLeaseHoldersCarriesBothIdentities(t *testing.T) {
	holders := UnclaimedLeaseHolders(leaseHolderFixture())
	if len(holders) != 2 {
		t.Fatalf("want two holder identities, got %v", holders)
	}
	if holders[0] != "11111111-1111-4111-8111-111111111111" {
		t.Fatalf("reservation id must be asked about first, got %q", holders[0])
	}
	if holders[1] != "ra8ci-4815162342-1" {
		t.Fatalf("runner name must be asked about, got %q", holders[1])
	}
}

func TestUnclaimedLeaseHoldersDropsWhatItDoesNotHave(t *testing.T) {
	vm := leaseHolderFixture()
	vm.ExternalRunnerName = ""
	if holders := UnclaimedLeaseHolders(vm); len(holders) != 1 || holders[0] != vm.ID {
		t.Fatalf("an unregistered reservation is asked about by id alone, got %v", holders)
	}
	vm.ID = ""
	if holders := UnclaimedLeaseHolders(vm); len(holders) != 0 {
		t.Fatalf("no identity must produce no lookup, got %v", holders)
	}
}

func TestUnclaimedLeaseHoldersDeduplicates(t *testing.T) {
	vm := leaseHolderFixture()
	vm.ExternalRunnerName = vm.ID
	if holders := UnclaimedLeaseHolders(vm); len(holders) != 1 {
		t.Fatalf("one identity twice is one lookup, got %v", holders)
	}
}

func TestListLiveBoardLeasesByHolderRefusesBadArguments(t *testing.T) {
	var s *Store
	for name, holder := range map[string]string{
		"empty": "",
		"long":  strings.Repeat("h", 513),
	} {
		if _, err := s.ListLiveBoardLeasesByHolder(context.Background(), holder); !errors.Is(err, ErrInvalid) {
			t.Fatalf("%s holder: want ErrInvalid, got %v", name, err)
		}
	}
	if _, err := s.ListLiveBoardLeasesByHolder(context.Background(), "holder"); !errors.Is(err, ErrInvalid) {
		t.Fatalf("storeless lookup: want ErrInvalid, got %v", err)
	}
}

// The live set is the bench's, not this file's: migration 0001 enforces one
// live lease per board over exactly these states, so a change there has to
// break here rather than silently leave the reaper reading a smaller set.
func TestLiveLeaseStatesMatchTheUniqueIndex(t *testing.T) {
	for _, state := range []string{"pending", "active"} {
		if !strings.Contains(liveLeaseStates, "'"+state+"'") {
			t.Fatalf("live lease states must include %q, got %q", state, liveLeaseStates)
		}
	}
	if strings.Contains(liveLeaseStates, "'ended'") {
		t.Fatalf("an ended lease holds no board: %q", liveLeaseStates)
	}
}
