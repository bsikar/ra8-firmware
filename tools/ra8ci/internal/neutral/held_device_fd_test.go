// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package neutral

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"testing"
)

// inspectorHolding builds a proc tree with one live, unprotected process
// holding the given descriptor targets, and returns an inspector over it.
func inspectorHolding(t *testing.T, targets ...string) LinuxActivityInspector {
	t.Helper()
	procRoot := t.TempDir()
	process := filepath.Join(procRoot, "4242")
	if err := os.MkdirAll(filepath.Join(process, "fd"), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(process, "comm"), []byte("idle-worker\n"), 0600); err != nil {
		t.Fatal(err)
	}
	for i, target := range targets {
		link := filepath.Join(process, "fd", string(rune('3'+i)))
		if err := os.Symlink(target, link); err != nil {
			t.Fatal(err)
		}
	}
	return LinuxActivityInspector{ProcRoot: procRoot, DevRoot: "/dev"}
}

func TestADescriptorUnderTheDeviceRootThatCannotBeResolvedIsNotCleared(t *testing.T) {
	inspector := inspectorHolding(t, "/dev/ra8-fixture-that-is-gone")
	err := inspector.CheckIdle(context.Background(), nil, []string{"/dev/null"})
	if !errors.Is(err, ErrObservationAbsent) {
		t.Fatalf("unresolvable fixture descriptor cleared the pass: %v", err)
	}
}

func TestADeletedDeviceDescriptorIsNotCleared(t *testing.T) {
	inspector := inspectorHolding(t, "/dev/null"+deletedDescriptorSuffix)
	err := inspector.CheckIdle(context.Background(), nil, []string{"/dev/null"})
	if !errors.Is(err, ErrObservationAbsent) {
		t.Fatalf("descriptor to an unlinked device cleared the pass: %v", err)
	}
}

// Sockets, pipes and anonymous inodes are the ordinary case on any live host
// and none of them resolves. Failing closed on these would refuse every pass
// on every machine, which is why the rule is bounded to the device root.
func TestDescriptorsThatNameNoPathAreStillCleared(t *testing.T) {
	inspector := inspectorHolding(t, "socket:[12345]", "pipe:[67890]", "anon_inode:[eventfd]")
	if err := inspector.CheckIdle(context.Background(), nil, []string{"/dev/null"}); err != nil {
		t.Fatalf("quiescent process with ordinary descriptors refused: %v", err)
	}
}

func TestAnUnresolvableDescriptorOutsideTheDeviceRootIsStillCleared(t *testing.T) {
	gone := filepath.Join(t.TempDir(), "scratch-file-since-removed")
	inspector := inspectorHolding(t, gone)
	if err := inspector.CheckIdle(context.Background(), nil, []string{"/dev/null"}); err != nil {
		t.Fatalf("descriptor outside the device root refused the pass: %v", err)
	}
}

// The second bound: a pass protecting no device has nothing an unresolvable
// descriptor could be, so it stays out of the way.
func TestAnUnresolvableDescriptorIsOnlyEvidenceWhenAPassProtectsADevice(t *testing.T) {
	inspector := inspectorHolding(t, "/dev/ra8-fixture-that-is-gone")
	if err := inspector.CheckIdle(context.Background(), nil, nil); err != nil {
		t.Fatalf("unresolvable descriptor refused a pass protecting no device: %v", err)
	}
}

// A descriptor that DOES resolve to a protected device keeps its own answer:
// this is the fixture being held, not an observation we could not make.
func TestAResolvedProtectedDeviceStillAnswersBusy(t *testing.T) {
	inspector := inspectorHolding(t, "/dev/null")
	err := inspector.CheckIdle(context.Background(), nil, []string{"/dev/null"})
	if !errors.Is(err, ErrHardwareBusy) || errors.Is(err, ErrObservationAbsent) {
		t.Fatalf("open protected device no longer answers busy: %v", err)
	}
}

func TestDescriptorTargetsAreJudgedAgainstTheDeviceRoot(t *testing.T) {
	for _, testCase := range []struct {
		name       string
		target     string
		protecting bool
		want       bool
	}{
		{"a device path", "/dev/ttyUSB0", true, true},
		{"an unlinked device path", "/dev/ttyUSB0" + deletedDescriptorSuffix, true, true},
		{"a nested device path", "/dev/serial/by-id/ra8-probe", true, true},
		{"an uncleaned device path", "/dev/serial/../ttyUSB0", true, true},
		{"the device root itself", "/dev", true, false},
		{"a path outside the device root", "/var/tmp/scratch", true, false},
		{"a device-root prefix that is not the root", "/devices/ttyUSB0", true, false},
		{"a socket", "socket:[12345]", true, false},
		{"a pipe", "pipe:[67890]", true, false},
		{"an anonymous inode", "anon_inode:[eventfd]", true, false},
		{"a relative target", "dev/ttyUSB0", true, false},
		{"an empty target", "", true, false},
		{"a device path while protecting nothing", "/dev/ttyUSB0", false, false},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			if got := descriptorCouldBeProtectedDevice("/dev", testCase.target, testCase.protecting); got != testCase.want {
				t.Fatalf("descriptorCouldBeProtectedDevice(%q) = %v, want %v", testCase.target, got, testCase.want)
			}
		})
	}
}
