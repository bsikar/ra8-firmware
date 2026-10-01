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

// fixtureSet builds the protected-device set the inspector builds, from the
// node itself, so no test here ever states a device number literally.
func fixtureSet(t *testing.T, paths ...string) map[uint64]bool {
	t.Helper()
	set := make(map[uint64]bool, len(paths))
	for _, path := range paths {
		info, err := os.Stat(path)
		if err != nil {
			t.Skipf("device %s is not present on this host: %v", path, err)
		}
		number, ok := deviceNumber(info)
		if !ok {
			t.Skipf("device %s carries no device number on this host", path)
		}
		set[number] = true
	}
	return set
}

// The set carries numbers and no paths at all, so a descriptor it refuses is
// refused on the device it opens rather than on the name it opens it under.
// This is the case a second node for one piece of hardware produces, which
// this box cannot construct (creating a device node needs privileges the
// sandbox does not grant), so the set is built the way CheckIdle builds it and
// the descriptor is judged against it directly.
func TestADescriptorIsJudgedOnTheDeviceItOpensNotItsName(t *testing.T) {
	fixtures := fixtureSet(t, "/dev/null")
	if err := checkHeldDescriptorIsNotTheFixture("/dev", "/dev/null", fixtures); !errors.Is(err, ErrHardwareBusy) {
		t.Fatalf("descriptor on the protected device cleared the pass: %v", err)
	}
}

func TestADescriptorOnAnotherDeviceIsStillCleared(t *testing.T) {
	fixtures := fixtureSet(t, "/dev/null")
	if _, err := os.Stat("/dev/zero"); err != nil {
		t.Skipf("/dev/zero is not present on this host: %v", err)
	}
	if err := checkHeldDescriptorIsNotTheFixture("/dev", "/dev/zero", fixtures); err != nil {
		t.Fatalf("descriptor on an unrelated device refused: %v", err)
	}
}

// Every ordinary descriptor on a live host resolves outside the device root,
// and none of them is stat'd: the rule is bounded to the root the fixture
// lives under, exactly as descriptorCouldBeProtectedDevice is.
func TestADescriptorOutsideTheDeviceRootIsNotJudgedAtAll(t *testing.T) {
	fixtures := fixtureSet(t, "/dev/null")
	outside := filepath.Join(t.TempDir(), "gone")
	if err := checkHeldDescriptorIsNotTheFixture("/dev", outside, fixtures); err != nil {
		t.Fatalf("descriptor outside the device root refused: %v", err)
	}
}

// A pass protecting nothing has nothing to compare against and must not start
// stat'ing descriptors it was never asked about.
func TestAPassProtectingNoDeviceJudgesNoDescriptor(t *testing.T) {
	if err := checkHeldDescriptorIsNotTheFixture("/dev", "/dev/null", nil); err != nil {
		t.Fatalf("descriptor refused with no protected device: %v", err)
	}
	if err := checkHeldDescriptorIsNotTheFixture("", "/dev/null", fixtureSet(t, "/dev/null")); err != nil {
		t.Fatalf("descriptor refused with no device root: %v", err)
	}
}

// A regular file under the device root is not a device and carries no number,
// so it is cleared rather than compared.
func TestARegularFileUnderTheDeviceRootIsNotAFixture(t *testing.T) {
	root := t.TempDir()
	plain := filepath.Join(root, "notes")
	if err := os.WriteFile(plain, []byte("not a device\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := checkHeldDescriptorIsNotTheFixture(root, plain, fixtureSet(t, "/dev/null")); err != nil {
		t.Fatalf("regular file under the device root refused: %v", err)
	}
}

// Resolution succeeded a moment ago and the stat did not, which is a race on
// the fixture's own directory. CheckIdle is documented to fail closed on
// anything it cannot inspect, and this arm says the same.
func TestADescriptorUnderTheDeviceRootThatVanishedIsNotCleared(t *testing.T) {
	root := t.TempDir()
	err := checkHeldDescriptorIsNotTheFixture(root, filepath.Join(root, "gone"), fixtureSet(t, "/dev/null"))
	if !errors.Is(err, ErrObservationAbsent) {
		t.Fatalf("uninspectable descriptor under the device root cleared the pass: %v", err)
	}
}

// deviceNumber is the one place the identity is read, and it answers only for
// a device node.
func TestOnlyADeviceNodeCarriesADeviceNumber(t *testing.T) {
	plain := filepath.Join(t.TempDir(), "notes")
	if err := os.WriteFile(plain, []byte("x"), 0600); err != nil {
		t.Fatal(err)
	}
	info, err := os.Stat(plain)
	if err != nil {
		t.Fatal(err)
	}
	if _, ok := deviceNumber(info); ok {
		t.Fatal("a regular file reported a device number")
	}
	if _, ok := deviceNumber(nil); ok {
		t.Fatal("a missing file reported a device number")
	}
	node, err := os.Stat("/dev/null")
	if err != nil {
		t.Skipf("/dev/null is not present on this host: %v", err)
	}
	if _, ok := deviceNumber(node); !ok {
		t.Fatal("a device node reported no device number")
	}
}

// The whole pass still answers as it did: a process holding the protected
// node is busy, and the same process holding nothing is clear.
func TestTheWholePassStillAnswersOnTheProtectedNode(t *testing.T) {
	inspector := inspectorHolding(t, "/dev/null")
	if err := inspector.CheckIdle(context.Background(), nil, []string{"/dev/null"}); !errors.Is(err, ErrHardwareBusy) {
		t.Fatalf("open protected device accepted: %v", err)
	}
	quiet := inspectorHolding(t)
	if err := quiet.CheckIdle(context.Background(), nil, []string{"/dev/null"}); err != nil {
		t.Fatalf("quiescent fixture refused: %v", err)
	}
}
