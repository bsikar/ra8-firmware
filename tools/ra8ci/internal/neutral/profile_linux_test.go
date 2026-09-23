// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package neutral

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

type testHardwareGate struct {
	locked bool
}

func (g *testHardwareGate) Lock(ctx context.Context) (func(), error) {
	select {
	case <-ctx.Done():
		return nil, ctx.Err()
	default:
	}
	if g.locked {
		return nil, errors.New("test gate already held")
	}
	g.locked = true
	return func() { g.locked = false }, nil
}

type testActivityInspector struct{ err error }

func (i testActivityInspector) CheckIdle(context.Context, []string, []string) error { return i.err }

type testSignalReader map[string]string

func (r testSignalReader) ReadSignal(_ context.Context, target string) (string, error) {
	value, ok := r[target]
	if !ok {
		return "", os.ErrNotExist
	}
	return value, nil
}

func observerFixture(t *testing.T) (*LinuxObserver, *testHardwareGate, time.Time, store.NeutralChallenge) {
	t.Helper()
	profile := validProfileFixture()
	now := time.Date(2026, 9, 23, 12, 0, 0, 0, time.UTC)
	gate := &testHardwareGate{}
	readers := map[string]SignalReader{
		"sysfs": testSignalReader{
			"bus/usb/001/serial":           "RA8D2-001\n",
			"bus/usb/002/serial":           "JLINK-001\n",
			"class/gpio/board_power/value": "1\n",
			"class/gpio/reset/value":       "1\n",
			"class/hwmon/hwmon0/in0_input": "12\n",
		},
		"tapo": testSignalReader{"board.power_state": "off"},
	}
	observer, err := newLinuxObserver(LinuxObserverConfig{
		Profile: profile, ProfileSHA256: strings.Repeat("a", 64),
		Gate: gate, Inspector: testActivityInspector{}, Readers: readers,
		Now: func() time.Time { return now },
	})
	if err != nil {
		t.Fatal(err)
	}
	challenge := store.NeutralChallenge{
		ID: "01996f90-3415-7cfe-8ff1-600058131afd", Nonce: strings.Repeat("b", 64),
		BoardID: profile.BoardID, Purpose: "release",
		LeaseID: "01996f90-3415-7cfe-8ff1-600058131afe", Generation: 4,
		AgentHighWater: 4, FixtureRevision: profile.FixtureRevision,
		ProfileSHA256: strings.Repeat("a", 64), RestorePolicy: profile.RestorePolicy,
		IssuedAt: now.Add(-time.Second), ExpiresAt: now.Add(20 * time.Second),
	}
	return observer, gate, now, challenge
}

func TestLinuxObserverRequiresMatchingSignalsUnderSharedGate(t *testing.T) {
	observer, gate, now, challenge := observerFixture(t)
	observation, err := observer.ObserveNeutral(context.Background(), challenge)
	if err != nil {
		t.Fatalf("neutral observation rejected: %v", err)
	}
	if !observation.Neutral || observation.ObservedAt != now || observation.ProfileSHA256 != challenge.ProfileSHA256 ||
		observation.ChallengeID != challenge.ID || len(observation.Evidence) == 0 || gate.locked {
		t.Fatalf("incomplete observation or leaked gate: %+v gate=%v", observation, gate.locked)
	}
	observer.readers["tapo"] = testSignalReader{"board.power_state": "on"}
	if _, err := observer.ObserveNeutral(context.Background(), challenge); !errors.Is(err, ErrSignalMismatch) {
		t.Fatalf("wrong physical power state accepted: %v", err)
	}
	if gate.locked {
		t.Fatal("gate remained locked after mismatched signal")
	}
}

func TestLinuxObserverRejectsWrongProfileBusyBoardAndExpiredContext(t *testing.T) {
	observer, _, _, challenge := observerFixture(t)
	challenge.FixtureRevision = "another-fixture"
	if _, err := observer.ObserveNeutral(context.Background(), challenge); !errors.Is(err, ErrProfileMismatch) {
		t.Fatalf("wrong fixture accepted: %v", err)
	}
	observer, _, _, challenge = observerFixture(t)
	observer.idle = testActivityInspector{err: ErrHardwareBusy}
	if _, err := observer.ObserveNeutral(context.Background(), challenge); !errors.Is(err, ErrHardwareBusy) {
		t.Fatalf("busy board accepted: %v", err)
	}
	observer, _, _, challenge = observerFixture(t)
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := observer.ObserveNeutral(ctx, challenge); !errors.Is(err, context.Canceled) {
		t.Fatalf("cancelled observation proceeded: %v", err)
	}
}

func TestSysfsReaderRejectsTraversalAndSymlinkEscape(t *testing.T) {
	root := t.TempDir()
	outside := t.TempDir()
	if err := os.WriteFile(filepath.Join(root, "value"), []byte("neutral\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(outside, "secret"), []byte("not neutral"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(filepath.Join(outside, "secret"), filepath.Join(root, "escape")); err != nil {
		t.Fatal(err)
	}
	reader := SysfsReader{Root: root}
	value, err := reader.ReadSignal(context.Background(), "value")
	if err != nil || strings.TrimSpace(value) != "neutral" {
		t.Fatalf("valid sysfs value=%q err=%v", value, err)
	}
	if _, err := reader.ReadSignal(context.Background(), "../secret"); !errors.Is(err, ErrInvalidProfile) {
		t.Fatalf("path traversal accepted: %v", err)
	}
	if _, err := reader.ReadSignal(context.Background(), "escape"); !errors.Is(err, ErrInvalidProfile) {
		t.Fatalf("symlink escape accepted: %v", err)
	}
}

func TestLinuxActivityInspectorDetectsProtectedProcessAndDeviceOwner(t *testing.T) {
	procRoot := t.TempDir()
	devRoot := "/dev"
	process := filepath.Join(procRoot, "12345")
	if err := os.MkdirAll(filepath.Join(process, "fd"), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(process, "comm"), []byte("rfp-cli\n"), 0600); err != nil {
		t.Fatal(err)
	}
	inspector := LinuxActivityInspector{ProcRoot: procRoot, DevRoot: devRoot}
	if err := inspector.CheckIdle(context.Background(), []string{"rfp-cli"}, nil); !errors.Is(err, ErrHardwareBusy) {
		t.Fatalf("active programming tool accepted: %v", err)
	}
	if err := os.WriteFile(filepath.Join(process, "comm"), []byte("idle-worker\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink("/dev/null", filepath.Join(process, "fd", "3")); err != nil {
		t.Fatal(err)
	}
	if err := inspector.CheckIdle(context.Background(), nil, []string{"/dev/null"}); !errors.Is(err, ErrHardwareBusy) {
		t.Fatalf("open protected device accepted: %v", err)
	}
	if err := os.Remove(filepath.Join(process, "fd", "3")); err != nil {
		t.Fatal(err)
	}
	if err := inspector.CheckIdle(context.Background(), nil, []string{"/dev/null"}); err != nil {
		t.Fatalf("quiescent device rejected: %v", err)
	}
}
