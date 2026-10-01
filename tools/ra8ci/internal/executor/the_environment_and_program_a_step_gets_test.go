// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"errors"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// A step runs with the environment this package hands it and the program this
// package resolved, and nothing else. These hold the refusals on both: a
// borrowed variable that points somewhere it should not, and a program name
// that resolves to something that cannot be run. Each refusal names what is
// wrong, because an operator reading a CI failure has no other way to tell a
// misconfigured box from a task that escaped its checkout.

// aCheckout is a directory to judge environment values against. It carries no
// catalog, because nothing here reaches the catalog.
func aCheckout(t *testing.T) string {
	t.Helper()
	root, err := filepath.EvalSymlinks(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	return root
}

// A borrowed path variable is taken only when it is absolute. A relative one is
// refused by name rather than resolved against whatever directory the runner
// happened to start in.
func TestABorrowedPathValueMustBeAbsolute(t *testing.T) {
	root := aCheckout(t)
	t.Setenv("GOCACHE", filepath.Join("relative", "cache"))

	env, err := cleanEnvironment(root)
	if !errors.Is(err, ErrUnsafeEnvironment) {
		t.Fatalf("error = %v, want an unsafe environment", err)
	}
	if !strings.Contains(err.Error(), "GOCACHE") {
		t.Fatalf("the refusal did not name the variable: %v", err)
	}
	if env != nil {
		t.Fatalf("a refused environment was still handed back: %v", env)
	}
}

// The same rule, from the other side: an absolute value outside the checkout is
// taken, and the environment that comes back is the allowlist plus the pinned
// toolchain, never the caller's whole environment.
func TestAnAbsoluteValueOutsideTheCheckoutIsTaken(t *testing.T) {
	root := aCheckout(t)
	outside := aCheckout(t)
	t.Setenv("GOCACHE", outside)
	t.Setenv("RA8CI_SECRET_TOKEN", "hunter2")

	env, err := cleanEnvironment(root)
	if err != nil {
		t.Fatalf("a value outside the checkout was refused: %v", err)
	}
	var sawCache, sawToolchain bool
	for _, item := range env {
		if item == "GOCACHE="+outside {
			sawCache = true
		}
		if item == "GOTOOLCHAIN=local" {
			sawToolchain = true
		}
		if strings.HasPrefix(item, "RA8CI_SECRET_TOKEN=") {
			t.Fatal("a variable outside the allowlist was carried into the step")
		}
	}
	if !sawCache || !sawToolchain {
		t.Fatalf("environment = %v, want the borrowed cache and the pinned toolchain", env)
	}
}

// PATH is judged entry by entry. A relative entry is refused before any of it
// is used, and so is an entry that leads back into the checkout under test.
func TestEveryPathEntryIsJudgedOnItsOwn(t *testing.T) {
	root := aCheckout(t)
	outside := aCheckout(t)

	t.Run("a relative entry", func(t *testing.T) {
		t.Setenv("PATH", strings.Join([]string{outside, "usr/local/bin"}, string(os.PathListSeparator)))
		if _, err := cleanEnvironment(root); !errors.Is(err, ErrUnsafeEnvironment) || !strings.Contains(err.Error(), "non-absolute") {
			t.Fatalf("error = %v, want the non-absolute entry refused", err)
		}
	})

	t.Run("an entry inside the checkout", func(t *testing.T) {
		t.Setenv("PATH", strings.Join([]string{outside, filepath.Join(root, "bin")}, string(os.PathListSeparator)))
		if _, err := cleanEnvironment(root); !errors.Is(err, ErrUnsafeEnvironment) || !strings.Contains(err.Error(), "checkout") {
			t.Fatalf("error = %v, want the checkout entry refused", err)
		}
	})

	t.Run("entries outside it", func(t *testing.T) {
		t.Setenv("PATH", strings.Join([]string{outside, filepath.Join(outside, "bin")}, string(os.PathListSeparator)))
		if _, err := cleanEnvironment(root); err != nil {
			t.Fatalf("a PATH entirely outside the checkout was refused: %v", err)
		}
	})
}

// A name that resolves to something other than a runnable file is refused as
// unreviewed, naming the program, rather than handed to the operating system to
// fail with whatever it happens to say. Every name here carries a slash: a bare
// word is looked up on PATH instead of inside the checkout, which is a
// different decision entirely.
func TestAProgramThatCannotBeRunIsRefusedByName(t *testing.T) {
	root := aCheckout(t)
	if err := os.MkdirAll(filepath.Join(root, "tools", "stage"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, "tools", "notes.txt"), []byte("not a program\n"), 0o644); err != nil {
		t.Fatal(err)
	}

	t.Run("a directory", func(t *testing.T) {
		_, err := resolveTaskProgram(root, "tools/stage")
		if !errors.Is(err, ErrUnreviewedTask) || !strings.Contains(err.Error(), "regular file") {
			t.Fatalf("error = %v, want a directory refused as not a regular file", err)
		}
	})

	t.Run("a file nobody may execute", func(t *testing.T) {
		if runtime.GOOS == "windows" {
			t.Skip("the execute bit is not the question on this platform")
		}
		_, err := resolveTaskProgram(root, "tools/notes.txt")
		if !errors.Is(err, ErrUnreviewedTask) || !strings.Contains(err.Error(), "not executable") {
			t.Fatalf("error = %v, want the missing execute bit named", err)
		}
	})

	t.Run("a name that is not there at all", func(t *testing.T) {
		_, err := resolveTaskProgram(root, "tools/absent")
		if !errors.Is(err, ErrToolMissing) {
			t.Fatalf("error = %v, want the tool reported missing", err)
		}
	})

	t.Run("a runnable program", func(t *testing.T) {
		program := filepath.Join(root, "tools", "run.sh")
		if err := os.WriteFile(program, []byte("#!/bin/sh\nexit 0\n"), 0o755); err != nil {
			t.Fatal(err)
		}
		resolved, err := resolveTaskProgram(root, "tools/run.sh")
		if err != nil {
			t.Fatalf("a runnable program inside the checkout was refused: %v", err)
		}
		if resolved != program {
			t.Fatalf("resolved = %q, want %q", resolved, program)
		}
	})
}

// shortWriter accepts less than it was given and says nothing about it, which
// is the quiet way a step log loses output.
type shortWriter struct{ taken int }

func (w *shortWriter) Write(data []byte) (int, error) {
	if len(data) == 0 {
		return 0, nil
	}
	w.taken += len(data) - 1
	return len(data) - 1, nil
}

// A log that silently dropped bytes is a failure, not a clean step: the digest
// would otherwise be published over output nobody ever received.
func TestALogThatSwallowedBytesIsAFailure(t *testing.T) {
	short := &shortWriter{}
	writer := newDigestWriter(short)

	n, err := writer.Write([]byte("hello"))
	if !errors.Is(err, io.ErrShortWrite) {
		t.Fatalf("error = %v, want a short write", err)
	}
	if n != 4 {
		t.Fatalf("wrote %d bytes, want the 4 the writer actually took", n)
	}
	if writer.err == nil {
		t.Fatal("the short write was not remembered for the step to report")
	}
	if _, bytes := writer.digest(); bytes != 4 {
		t.Fatalf("digest covered %d bytes, want only the 4 that landed", bytes)
	}
}
