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

// commOf writes a proc tree holding one live process whose comm is exactly
// what the kernel would have written, and returns an inspector over it.
func commOf(t *testing.T, name string) LinuxActivityInspector {
	t.Helper()
	procRoot := t.TempDir()
	process := filepath.Join(procRoot, "7331")
	if err := os.MkdirAll(process, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(process, "comm"), []byte(asKernelComm(name)+"\n"), 0600); err != nil {
		t.Fatal(err)
	}
	return LinuxActivityInspector{ProcRoot: procRoot, DevRoot: "/dev"}
}

// asKernelComm cuts a name the way TASK_COMM_LEN does.
func asKernelComm(name string) string {
	if len(name) > commLimit {
		return name[:commLimit]
	}
	return name
}

func TestCommLimitIsTheKernelsOwnBuffer(t *testing.T) {
	if commLimit != 15 {
		t.Fatalf("comm limit is %d, not the 15 bytes TASK_COMM_LEN leaves for a name", commLimit)
	}
}

func TestTheBuiltInListCarriesNamesCommCannotHold(t *testing.T) {
	var long []string
	for _, name := range defaultProtectedProcesses {
		if len(name) > commLimit {
			long = append(long, name)
		}
	}
	if len(long) == 0 {
		t.Fatal("no built-in protected name is longer than comm, so this rule guards nothing")
	}
	prefixes := truncatedProtectedNames(defaultProtectedProcesses)
	for _, name := range long {
		if !prefixes[strings.ToLower(asKernelComm(name))] {
			t.Fatalf("protected name %q has no comm-length prefix on record", name)
		}
	}
}

func TestATruncatedProtectedNameStillAnswersBusy(t *testing.T) {
	for _, name := range []string{"ra8-hil-privileged", "ra8-hil-privileged.py"} {
		inspector := commOf(t, name)
		err := inspector.CheckIdle(context.Background(), defaultProtectedProcesses, nil)
		if !errors.Is(err, ErrHardwareBusy) {
			t.Fatalf("%s truncated to comm cleared the pass: %v", name, err)
		}
	}
}

func TestAProfileNameTooLongForCommStillAnswersBusy(t *testing.T) {
	inspector := commOf(t, "fixture-power-cycler.sh")
	err := inspector.CheckIdle(context.Background(), []string{"fixture-power-cycler.sh"}, nil)
	if !errors.Is(err, ErrHardwareBusy) {
		t.Fatalf("long profile-supplied name cleared the pass: %v", err)
	}
}

func TestAShortProtectedNameIsStillMatchedWhole(t *testing.T) {
	inspector := commOf(t, "rfp-cli")
	err := inspector.CheckIdle(context.Background(), defaultProtectedProcesses, nil)
	if !errors.Is(err, ErrHardwareBusy) {
		t.Fatalf("short protected name stopped matching: %v", err)
	}
}

func TestAnOrdinaryProcessStillClearsThePass(t *testing.T) {
	for _, name := range []string{"idle-worker", "systemd-journald-and-more", "ra8-hil", "bash"} {
		inspector := commOf(t, name)
		if err := inspector.CheckIdle(context.Background(), defaultProtectedProcesses, nil); err != nil {
			t.Fatalf("quiescent process %q refused: %v", name, err)
		}
	}
}

func TestOnlyACommLengthNameIsPrefixMatched(t *testing.T) {
	protected := map[string]bool{"ra8-hil-privileged": true}
	truncated := truncatedProtectedNames([]string{"ra8-hil-privileged"})
	cases := []struct {
		comm  string
		match bool
	}{
		{"ra8-hil-privile", true},
		{"ra8-hil-privil", false},
		{"ra8-hil-privilex", false},
		{"ra8-hil-privileged", true},
		{"RA8-HIL-PRIVILE", true},
		{"  ra8-hil-privile\n", true},
		{"", false},
		{"   ", false},
	}
	for _, c := range cases {
		if got := commNamesAProtectedProcess(c.comm, protected, truncated); got != c.match {
			t.Fatalf("comm %q judged %v, want %v", c.comm, got, c.match)
		}
	}
}

func TestNoPrefixIsRecordedForANameCommCanHold(t *testing.T) {
	prefixes := truncatedProtectedNames([]string{"rfp-cli", "openocd", "tapo_control.py", "JLinkGDBServer"})
	if len(prefixes) != 0 {
		t.Fatalf("names that fit in comm were recorded as truncated: %v", prefixes)
	}
}

// A name of exactly commLimit bytes is written back whole, so it must keep
// matching through the protected set rather than through the prefix set.
func TestANameExactlyAtTheLimitIsNotTruncated(t *testing.T) {
	const name = "tapo_control.py"
	if len(name) != commLimit {
		t.Fatalf("fixture name is %d bytes, not the boundary", len(name))
	}
	inspector := commOf(t, name)
	if err := inspector.CheckIdle(context.Background(), defaultProtectedProcesses, nil); !errors.Is(err, ErrHardwareBusy) {
		t.Fatalf("boundary-length protected name cleared the pass: %v", err)
	}
}

// The cmdline arm is not a second chance: a zombie or a process that rewrote
// its argv has an empty cmdline, and the comm arm is all that is left.
func TestATruncatedNameIsCaughtWithAnEmptyCommandLine(t *testing.T) {
	procRoot := t.TempDir()
	process := filepath.Join(procRoot, "9001")
	if err := os.MkdirAll(process, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(process, "comm"), []byte("ra8-hil-privile\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(process, "cmdline"), nil, 0600); err != nil {
		t.Fatal(err)
	}
	inspector := LinuxActivityInspector{ProcRoot: procRoot, DevRoot: "/dev"}
	err := inspector.CheckIdle(context.Background(), defaultProtectedProcesses, nil)
	if !errors.Is(err, ErrHardwareBusy) {
		t.Fatalf("truncated name with no command line cleared the pass: %v", err)
	}
}

// The whole observer, not just the inspector arm, must refuse the board while
// the privileged helper is live under a truncated name.
func TestTheObserverRefusesABoardHoldingATruncatedTool(t *testing.T) {
	procRoot := t.TempDir()
	process := filepath.Join(procRoot, "4410")
	if err := os.MkdirAll(process, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(process, "comm"), []byte("ra8-hil-privile\n"), 0600); err != nil {
		t.Fatal(err)
	}
	observer, _, _, challenge := observerFixture(t)
	observer.idle = LinuxActivityInspector{ProcRoot: procRoot, DevRoot: "/dev"}
	if _, err := observer.ObserveNeutral(context.Background(), challenge); !errors.Is(err, ErrHardwareBusy) {
		t.Fatalf("observer signed over a board holding the privileged helper: %v", err)
	}
}
