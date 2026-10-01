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
)

// A neutral observation is only as good as what it refused to read. These
// hold the bounds on one sysfs scalar and the ways an activity sweep gives
// up rather than reporting a board idle it could not actually inspect.

func sysfsRoot(t *testing.T, files map[string]string) string {
	t.Helper()
	root := t.TempDir()
	for name, content := range files {
		path := filepath.Join(root, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return root
}

func TestSysfsReaderRefusesARequestItCannotAnswer(t *testing.T) {
	root := sysfsRoot(t, map[string]string{"class/power/state": "1"})
	reader := SysfsReader{Root: root}

	var noContext context.Context
	if _, err := reader.ReadSignal(noContext, "class/power/state"); !errors.Is(err, ErrInvalidProfile) {
		t.Fatalf("no context = %v", err)
	}
	stopped, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := reader.ReadSignal(stopped, "class/power/state"); !errors.Is(err, ErrInvalidProfile) {
		t.Fatalf("a caller that gave up = %v", err)
	}
	if _, err := (SysfsReader{}).ReadSignal(context.Background(), "class/power/state"); !errors.Is(err, ErrInvalidProfile) {
		t.Fatalf("no root = %v", err)
	}
	for _, target := range []string{"", "/class/power/state", "../escape", "class//power", "class/power/", "class/power/state\n"} {
		if _, err := reader.ReadSignal(context.Background(), target); !errors.Is(err, ErrInvalidProfile) {
			t.Fatalf("target %q = %v", target, err)
		}
	}
	if _, err := (SysfsReader{Root: filepath.Join(root, "absent")}).ReadSignal(context.Background(), "class/power/state"); err == nil {
		t.Fatal("a root that is not there was read")
	}
}

// The target must be one ordinary file under the root, so a directory, a
// device node's parent or a missing entry is refused rather than read as
// an empty value.
func TestSysfsReaderRefusesWhatIsNotOneOrdinaryFile(t *testing.T) {
	root := sysfsRoot(t, map[string]string{"class/power/state": "1"})
	reader := SysfsReader{Root: root}

	if _, err := reader.ReadSignal(context.Background(), "class/power"); !errors.Is(err, ErrInvalidProfile) {
		t.Fatalf("a directory = %v", err)
	}
	if _, err := reader.ReadSignal(context.Background(), "class/power/absent"); err == nil {
		t.Fatal("a signal that is not there was read")
	}
}

// The value is bounded, and the bound is exact: a scalar is short by
// nature, so anything past it is a sysfs file that is not what the profile
// named.
func TestSysfsReaderHoldsTheValueBoundExactly(t *testing.T) {
	root := sysfsRoot(t, map[string]string{
		"at/bound":   strings.Repeat("7", 4096),
		"past/bound": strings.Repeat("7", 4097),
		"short":      "1\n",
	})
	reader := SysfsReader{Root: root}

	value, err := reader.ReadSignal(context.Background(), "at/bound")
	if err != nil || len(value) != 4096 {
		t.Fatalf("a value exactly at the bound = %d bytes, %v", len(value), err)
	}
	if _, err := reader.ReadSignal(context.Background(), "past/bound"); !errors.Is(err, ErrInvalidProfile) {
		t.Fatalf("a value one byte past the bound = %v", err)
	}
	if value, err := reader.ReadSignal(context.Background(), "short"); err != nil || value != "1\n" {
		t.Fatalf("an ordinary scalar = %q, %v", value, err)
	}
}

// An inspector that cannot see procfs or the device root reports an absent
// observation rather than an idle board, because the two are not the same
// answer to the caller.
func TestActivityInspectorRefusesToReportOnWhatItCannotSee(t *testing.T) {
	procRoot := t.TempDir()
	devRoot := t.TempDir()

	var noContext context.Context
	if err := (LinuxActivityInspector{ProcRoot: procRoot, DevRoot: devRoot}).CheckIdle(noContext, nil, nil); !errors.Is(err, ErrObservationAbsent) {
		t.Fatalf("no context = %v", err)
	}
	for name, inspector := range map[string]LinuxActivityInspector{
		"no procfs root": {DevRoot: devRoot},
		"no device root": {ProcRoot: procRoot},
		"neither root":   {},
	} {
		if err := inspector.CheckIdle(context.Background(), nil, nil); !errors.Is(err, ErrObservationAbsent) {
			t.Fatalf("%s = %v", name, err)
		}
	}
	absent := LinuxActivityInspector{ProcRoot: filepath.Join(procRoot, "gone"), DevRoot: devRoot}
	if err := absent.CheckIdle(context.Background(), nil, nil); err == nil {
		t.Fatal("a procfs root that is not there was inspected")
	}
}

// A protected device must be a device node under the device root. A path
// outside it is a bad profile; a path inside it that is not a device node
// is an observation the inspector could not make.
func TestActivityInspectorJudgesEachProtectedDevice(t *testing.T) {
	procRoot := t.TempDir()
	devRoot := t.TempDir()
	ordinary := filepath.Join(devRoot, "ttyUSB0")
	if err := os.WriteFile(ordinary, []byte("not a device"), 0o644); err != nil {
		t.Fatal(err)
	}
	inspector := LinuxActivityInspector{ProcRoot: procRoot, DevRoot: devRoot}

	outside := filepath.Join(t.TempDir(), "ttyUSB0")
	if err := inspector.CheckIdle(context.Background(), nil, []string{outside}); !errors.Is(err, ErrInvalidProfile) {
		t.Fatalf("a device outside the device root = %v", err)
	}
	if err := inspector.CheckIdle(context.Background(), nil, []string{filepath.Join(devRoot, "absent")}); !errors.Is(err, ErrObservationAbsent) {
		t.Fatalf("a device that is not there = %v", err)
	}
	if err := inspector.CheckIdle(context.Background(), nil, []string{ordinary}); !errors.Is(err, ErrObservationAbsent) {
		t.Fatalf("an ordinary file named as a device = %v", err)
	}
}

// procfs holds more than processes, so only numeric directories are
// inspected: a named directory, and a numeric entry that is a file rather
// than a directory, are both passed over instead of being read as a
// process.
func TestActivityInspectorOnlyInspectsNumericProcessDirectories(t *testing.T) {
	procRoot := t.TempDir()
	for _, name := range []string{"self", "sys", "1a", "bus"} {
		if err := os.MkdirAll(filepath.Join(procRoot, name), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(procRoot, name, "comm"), []byte("openocd\n"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.WriteFile(filepath.Join(procRoot, "412"), []byte("openocd\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(procRoot, "77"), 0o755); err != nil {
		t.Fatal(err)
	}

	inspector := LinuxActivityInspector{ProcRoot: procRoot, DevRoot: t.TempDir()}
	if err := inspector.CheckIdle(context.Background(), []string{"openocd"}, nil); err != nil {
		t.Fatalf("a procfs holding no numeric process directory = %v", err)
	}

	// The same name under a numeric directory is the process this sweep
	// exists to find.
	if err := os.WriteFile(filepath.Join(procRoot, "77", "comm"), []byte("openocd\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := inspector.CheckIdle(context.Background(), []string{"openocd"}, nil); !errors.Is(err, ErrHardwareBusy) {
		t.Fatalf("a protected process under a numeric directory = %v", err)
	}
}
