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

// The idle check walks /proc and reads three things from every process it
// finds: its comm, its command line, and its open descriptors. Each of
// those reads can fail for two very different reasons, and the check
// treats them differently on purpose.
//
// A process that exited mid-walk is ordinary. Linux answers that with a
// not-exist error and the check passes over the process, because a board
// is not busy on account of something that is no longer running.
//
// Anything else means the check could not see what it was sent to see, and
// it says so rather than reporting an idle board. That is the safety
// property worth pinning: an unreadable /proc must never be mistaken for a
// quiet one.
//
// Each case below wedges one of those reads with a shape the kernel
// refuses: a directory where a file belongs, a file where a directory
// belongs, a plain file where a symlink belongs. None of them depend on
// file permissions, which say nothing on a box running as root.

// procTree builds a proc root holding one process directory, with comm,
// cmdline and an fd directory already in place, and hands back the process
// directory so a case can wedge one of them.
func procTree(t *testing.T, comm string) (LinuxActivityInspector, string) {
	t.Helper()
	procRoot := t.TempDir()
	process := filepath.Join(procRoot, "4242")
	if err := os.MkdirAll(filepath.Join(process, "fd"), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(process, "comm"), []byte(comm+"\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(process, "cmdline"), []byte("idle-worker\x00"), 0600); err != nil {
		t.Fatal(err)
	}
	return LinuxActivityInspector{ProcRoot: procRoot, DevRoot: t.TempDir()}, process
}

func replaceWithDirectory(t *testing.T, path string) {
	t.Helper()
	if err := os.Remove(path); err != nil {
		t.Fatal(err)
	}
	if err := os.Mkdir(path, 0700); err != nil {
		t.Fatal(err)
	}
}

// A read that fails for any reason other than the process having exited is
// handed back naming the read that failed, so an operator learns which of
// the three it was.
func TestAProcessThatCannotBeReadIsNotReportedIdle(t *testing.T) {
	for name, wedge := range map[string]func(*testing.T, string){
		"comm is a directory": func(t *testing.T, process string) {
			replaceWithDirectory(t, filepath.Join(process, "comm"))
		},
		"the command line is a directory": func(t *testing.T, process string) {
			replaceWithDirectory(t, filepath.Join(process, "cmdline"))
		},
		"the descriptor directory is a file": func(t *testing.T, process string) {
			if err := os.RemoveAll(filepath.Join(process, "fd")); err != nil {
				t.Fatal(err)
			}
			if err := os.WriteFile(filepath.Join(process, "fd"), []byte("not a directory"), 0600); err != nil {
				t.Fatal(err)
			}
		},
		"a descriptor is not a link": func(t *testing.T, process string) {
			if err := os.WriteFile(filepath.Join(process, "fd", "3"), []byte("not a link"), 0600); err != nil {
				t.Fatal(err)
			}
		},
	} {
		inspector, process := procTree(t, "idle-worker")
		wedge(t, process)

		err := inspector.CheckIdle(context.Background(), []string{"openocd"}, nil)
		if err == nil {
			t.Fatalf("%s: the board was reported idle over a /proc the check could not read", name)
		}
		if errors.Is(err, ErrHardwareBusy) {
			t.Fatalf("%s: an unreadable process was reported busy, which is a different claim", name)
		}
		if !strings.Contains(err.Error(), "inspect process") {
			t.Fatalf("%s: err = %v, want it to name the read that failed", name, err)
		}
	}
}

// A process that exits while the check is walking is passed over. Linux
// answers those reads with a not-exist error, which is the one failure that
// means nothing is holding the board.
func TestAProcessThatExitedMidWalkIsPassedOver(t *testing.T) {
	inspector, process := procTree(t, "idle-worker")
	for _, name := range []string{"cmdline", "fd"} {
		if err := os.RemoveAll(filepath.Join(process, name)); err != nil {
			t.Fatal(err)
		}
	}
	if err := inspector.CheckIdle(context.Background(), []string{"openocd"}, nil); err != nil {
		t.Fatalf("a process that went away was treated as a failure: %v", err)
	}

	empty := LinuxActivityInspector{ProcRoot: t.TempDir(), DevRoot: t.TempDir()}
	if err := empty.CheckIdle(context.Background(), []string{"openocd"}, nil); err != nil {
		t.Fatalf("an empty proc root was refused: %v", err)
	}
}

// The command line is searched argument by argument, on the base name, so
// a protected tool is found however it was invoked. This is the check that
// catches a debugger whose comm was renamed but whose argv still names it.
func TestAProtectedToolIsFoundInTheCommandLineHoweverItWasInvoked(t *testing.T) {
	for name, cmdline := range map[string]string{
		"an absolute path":       "/usr/bin/openocd\x00-f\x00board.cfg\x00",
		"a relative path":        "./tools/OpenOCD\x00",
		"a later argument":       "sudo\x00-E\x00openocd\x00",
		"a single argument":      "openocd\x00",
		"a path with no NUL end": "/opt/bin/openocd",
	} {
		inspector, process := procTree(t, "idle-worker")
		if err := os.WriteFile(filepath.Join(process, "cmdline"), []byte(cmdline), 0600); err != nil {
			t.Fatal(err)
		}
		if err := inspector.CheckIdle(context.Background(), []string{"openocd"}, nil); !errors.Is(err, ErrHardwareBusy) {
			t.Fatalf("%s: err = %v, want ErrHardwareBusy", name, err)
		}
	}

	inspector, process := procTree(t, "idle-worker")
	if err := os.WriteFile(filepath.Join(process, "cmdline"), []byte("/usr/bin/openocd-wrapper\x00--openocdish\x00"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := inspector.CheckIdle(context.Background(), []string{"openocd"}, nil); err != nil {
		t.Fatalf("a name that merely contains the protected one was reported busy: %v", err)
	}
}

// An enormous command line is refused rather than searched. A process can
// hand /proc megabytes of argv, and the check will not spend the board's
// observation window on it.
func TestAnEnormousCommandLineIsRefusedRatherThanSearched(t *testing.T) {
	inspector, process := procTree(t, "idle-worker")
	huge := strings.Repeat("a", (128<<10)+1)
	if err := os.WriteFile(filepath.Join(process, "cmdline"), []byte(huge), 0600); err != nil {
		t.Fatal(err)
	}
	if err := inspector.CheckIdle(context.Background(), []string{"openocd"}, nil); !errors.Is(err, ErrObservationAbsent) {
		t.Fatalf("err = %v, want ErrObservationAbsent", err)
	}

	atTheBound := strings.Repeat("a", 128<<10)
	if err := os.WriteFile(filepath.Join(process, "cmdline"), []byte(atTheBound), 0600); err != nil {
		t.Fatal(err)
	}
	if err := inspector.CheckIdle(context.Background(), []string{"openocd"}, nil); err != nil {
		t.Fatalf("a command line exactly at the bound was refused: %v", err)
	}
}

// A proc root that is not a directory cannot be walked, and the check says
// so instead of reporting the board idle over a tree it never read.
func TestAProcRootThatCannotBeWalkedIsNotAnIdleBoard(t *testing.T) {
	file := filepath.Join(t.TempDir(), "proc")
	if err := os.WriteFile(file, []byte("not a directory"), 0600); err != nil {
		t.Fatal(err)
	}
	inspector := LinuxActivityInspector{ProcRoot: file, DevRoot: t.TempDir()}
	if err := inspector.CheckIdle(context.Background(), []string{"openocd"}, nil); err == nil {
		t.Fatal("a proc root that is not a directory was reported idle")
	}

	absent := LinuxActivityInspector{ProcRoot: filepath.Join(t.TempDir(), "absent"), DevRoot: t.TempDir()}
	if err := absent.CheckIdle(context.Background(), []string{"openocd"}, nil); err == nil {
		t.Fatal("an absent proc root was reported idle")
	}
	missingDev := LinuxActivityInspector{ProcRoot: t.TempDir(), DevRoot: filepath.Join(t.TempDir(), "absent")}
	if err := missingDev.CheckIdle(context.Background(), []string{"openocd"}, nil); err == nil {
		t.Fatal("an absent device root was reported idle")
	}
}

// The walk gives up the moment its context is over, both between processes
// and between one process's descriptors, so a cancelled observation does
// not keep reading the tree.
func TestACancelledWalkStopsWhereItIs(t *testing.T) {
	inspector, process := procTree(t, "idle-worker")
	if err := os.Symlink(filepath.Join(t.TempDir(), "held"), filepath.Join(process, "fd", "3")); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if err := inspector.CheckIdle(ctx, []string{"openocd"}, nil); !errors.Is(err, context.Canceled) {
		t.Fatalf("err = %v, want context.Canceled", err)
	}
}
