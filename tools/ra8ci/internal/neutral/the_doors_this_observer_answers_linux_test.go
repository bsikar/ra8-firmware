//go:build linux

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package neutral

import (
	"context"
	"errors"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// A gate that runs a hook while it holds the lock, so a test can make the
// world change underneath an observation that has already been admitted.
type gateRunning struct {
	hook func()
}

func (g gateRunning) Lock(context.Context) (func(), error) {
	if g.hook != nil {
		g.hook()
	}
	return func() {}, nil
}

// An observer handed no activity inspector builds its own from the roots it
// was given, rather than standing up with none. That matters because a nil
// inspector is one of the four things ObserveNeutral refuses outright, so an
// observer that quietly kept nil would refuse every observation with the gate
// reported unavailable, which reads as a broken fixture rather than a missing
// argument.
func TestAnObserverHandedNoInspectorBuildsItsOwn(t *testing.T) {
	profile := validProfileFixture()
	now := time.Date(2026, 9, 23, 12, 0, 0, 0, time.UTC)
	observer, err := newLinuxObserver(LinuxObserverConfig{
		Profile: profile, ProfileSHA256: strings.Repeat("a", 64),
		Gate:     &testHardwareGate{},
		ProcRoot: t.TempDir(), DevRoot: t.TempDir(),
		Readers: map[string]SignalReader{
			"sysfs": testSignalReader(soundReadings()),
			"tapo":  testSignalReader{"board.power_state": "off"},
		},
		Now: func() time.Time { return now },
	})
	if err != nil {
		t.Fatalf("an observer with no inspector was refused: %v", err)
	}

	_, err = observer.ObserveNeutral(context.Background(), challengeFor(profile, now))
	if errors.Is(err, ErrGateUnavailable) {
		t.Fatal("the observer stood up with no inspector and refused its own gate")
	}
}

// The context is read again after the gate is held. A caller that gives up
// during the wait for the hardware must not have a physical reading taken on
// its behalf, so the cancellation is handed back as itself rather than as a
// mismatch or an absent observation.
func TestACancellationWhileTheGateIsHeldStopsTheReading(t *testing.T) {
	profile := validProfileFixture()
	now := time.Date(2026, 9, 23, 12, 0, 0, 0, time.UTC)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	observer, err := newLinuxObserver(LinuxObserverConfig{
		Profile: profile, ProfileSHA256: strings.Repeat("a", 64),
		Gate: gateRunning{hook: cancel}, Inspector: testActivityInspector{},
		Readers: map[string]SignalReader{
			"sysfs": testSignalReader(soundReadings()),
			"tapo":  testSignalReader{"board.power_state": "off"},
		},
		Now: func() time.Time { return now },
	})
	if err != nil {
		t.Fatal(err)
	}

	observation, err := observer.ObserveNeutral(ctx, challengeFor(profile, now))
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("a reading was taken for a caller that had given up: %v", err)
	}
	if observation.Neutral {
		t.Fatal("a cancelled observation still swore the board was neutral")
	}
}

// A signal file the filesystem will describe but not open is handed back as
// the permission failure it is, not as an invalid profile. The distinction is
// the whole value: a profile naming a signal that is not there is the author's
// mistake to fix, while a signal that is there and sealed is the fixture
// host's, and the two are repaired in different places.
func TestASignalFileThatWillNotOpenIsNotAProfileFault(t *testing.T) {
	root := t.TempDir()
	path := filepath.Join(root, "board", "serial")
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte("RA8-0001\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(path, 0o000); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(path, 0o644) })
	if info, err := os.Stat(path); err != nil || !info.Mode().IsRegular() {
		t.Fatalf("the sealed signal file stopped being a regular file: %v", err)
	}

	value, err := SysfsReader{Root: root}.ReadSignal(context.Background(), "board/serial")
	if err == nil {
		t.Fatalf("a sealed signal file answered %q", value)
	}
	if !errors.Is(err, fs.ErrPermission) {
		t.Fatalf("the refusal does not name the permission that stopped it: %v", err)
	}
	if errors.Is(err, ErrInvalidProfile) {
		t.Fatal("a sealed signal file was blamed on the profile")
	}
	if value != "" {
		t.Fatalf("a refused read still answered %q", value)
	}
}

func challengeFor(profile Profile, now time.Time) store.NeutralChallenge {
	return store.NeutralChallenge{
		ID: "01996f90-3415-7cfe-8ff1-600058131afd", Nonce: strings.Repeat("b", 64),
		BoardID: profile.BoardID, Purpose: "release",
		LeaseID: "01996f90-3415-7cfe-8ff1-600058131afe", Generation: 4,
		AgentHighWater: 4, FixtureRevision: profile.FixtureRevision,
		ProfileSHA256: strings.Repeat("a", 64), RestorePolicy: profile.RestorePolicy,
		IssuedAt: now.Add(-time.Second), ExpiresAt: now.Add(20 * time.Second),
	}
}
