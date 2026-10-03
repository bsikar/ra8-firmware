// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"context"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

// The shape this closes: an absolute program path that lands in the tree under
// test. The same bytes reached through a relative name are refused by the
// relative branch only when they escape the checkout, which is the opposite
// direction, so this is the one door that had no question at all.
func TestAnAbsoluteProgramInsideTheCheckoutIsRefused(t *testing.T) {
	root := t.TempDir()
	program := writeProgram(t, filepath.Join(root, "scripts", "smuggled"), "exit 0")
	_, err := resolveTaskProgram(root, program)
	if !errors.Is(err, ErrUnsafeEnvironment) {
		t.Fatalf("resolveTaskProgram error = %v, want ErrUnsafeEnvironment", err)
	}
	if !strings.Contains(err.Error(), program) {
		t.Fatalf("refusal %q does not name the program it refused", err)
	}
}

// The checkout root itself, named as the program, never resolves, and the
// answer stays the one the lookup gives: a directory is not a program, which
// exec.LookPath decides before containment is ever in question. Pinned so the
// new rule is understood as running AFTER a successful lookup rather than in
// place of one.
func TestTheCheckoutRootIsNotAProgram(t *testing.T) {
	root := t.TempDir()
	if _, err := resolveTaskProgram(root, root); !errors.Is(err, ErrToolMissing) {
		t.Fatalf("resolveTaskProgram error = %v, want ErrToolMissing", err)
	}
}

// A genuine system tool is what the absolute branch is for, and it still
// resolves. The program here sits in a sibling temp directory, so it is
// absolute, outside the checkout, and nothing about it changed.
func TestAnAbsoluteProgramOutsideTheCheckoutStillResolves(t *testing.T) {
	root := t.TempDir()
	elsewhere := t.TempDir()
	program := writeProgram(t, filepath.Join(elsewhere, "tool"), "exit 0")
	resolved, err := resolveTaskProgram(root, program)
	if err != nil {
		t.Fatalf("resolveTaskProgram error = %v, want nil", err)
	}
	if resolved != program {
		t.Fatalf("resolved = %q, want %q", resolved, program)
	}
}

// The second way in: a PATH directory that is itself outside the checkout,
// holding a symlink whose target is not. cleanEnvironment resolves the
// DIRECTORY and finds it clean, so the entry inside it is the part nothing
// looked at until now.
func TestAPathSymlinkIntoTheCheckoutIsRefused(t *testing.T) {
	root := t.TempDir()
	binDir := t.TempDir()
	target := writeProgram(t, filepath.Join(root, "tools", "gate"), "exit 0")
	link := filepath.Join(binDir, "gate-probe")
	symlinkTest(t, target, link)
	t.Setenv("PATH", binDir)
	if _, err := resolveTaskProgram(root, "gate-probe"); !errors.Is(err, ErrUnsafeEnvironment) {
		t.Fatalf("resolveTaskProgram error = %v, want ErrUnsafeEnvironment", err)
	}
}

// The same lookup with the same PATH, resolving to a real file outside the
// checkout, is untouched. Without this the test above would pass for a rule
// that refused every PATH lookup.
func TestAPathLookupOutsideTheCheckoutStillResolves(t *testing.T) {
	root := t.TempDir()
	binDir := t.TempDir()
	program := writeProgram(t, filepath.Join(binDir, "gate-probe"), "exit 0")
	t.Setenv("PATH", binDir)
	resolved, err := resolveTaskProgram(root, "gate-probe")
	if err != nil {
		t.Fatalf("resolveTaskProgram error = %v, want nil", err)
	}
	if resolved != program {
		t.Fatalf("resolved = %q, want %q", resolved, program)
	}
}

// Order matters between the two refusals. A name nothing on PATH provides is a
// missing tool, and it has to stay one: reading it as an unsafe environment
// would send an operator looking at the checkout for a binary the image was
// supposed to carry.
func TestAMissingToolIsStillAMissingTool(t *testing.T) {
	root := t.TempDir()
	t.Setenv("PATH", t.TempDir())
	if _, err := resolveTaskProgram(root, "no-such-tool-anywhere"); !errors.Is(err, ErrToolMissing) {
		t.Fatalf("resolveTaskProgram error = %v, want ErrToolMissing", err)
	}
}

// The mirror rule, pinned from the other side: a relative program IS checkout
// content and still resolves from the checkout. The new rule is about the
// branch that claims the program comes from the image, so this one may not
// move.
func TestARelativeProgramInsideTheCheckoutStillResolves(t *testing.T) {
	root := t.TempDir()
	program := writeProgram(t, filepath.Join(root, "scripts", "probe"+programFixtureExtension), "exit 0")
	resolved, err := resolveTaskProgram(root, filepath.Join("scripts", "probe"+programFixtureExtension))
	if err != nil {
		t.Fatalf("resolveTaskProgram error = %v, want nil", err)
	}
	if resolved != program {
		t.Fatalf("resolved = %q, want %q", resolved, program)
	}
}

// Both doors answer with one error, which is what lets an operator read the
// two refusals as the same fact about the checkout rather than two unrelated
// failures. cleanEnvironment owns the PATH half.
func TestTheEnvironmentDoorRefusesTheCheckoutOnPathToo(t *testing.T) {
	root := t.TempDir()
	t.Setenv("PATH", filepath.Join(root, "bin"))
	if _, err := cleanEnvironment(root); !errors.Is(err, ErrUnsafeEnvironment) {
		t.Fatalf("cleanEnvironment error = %v, want ErrUnsafeEnvironment", err)
	}
}

// At step level the refusal has to happen before anything runs, so the proof
// is a program that would leave a mark and does not get the chance. The step
// also keeps noChildExit, because nothing exited.
func TestAStepNamingACheckoutProgramRunsNothing(t *testing.T) {
	root := t.TempDir()
	witness := filepath.Join(t.TempDir(), "ran")
	program := writeProgram(t, filepath.Join(root, "scripts", "smuggled"), "touch "+witness)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	step := catalog.Step{Name: "smuggled", Program: program}
	result, err := runStep(ctx, root, []string{"PATH=/usr/bin:/bin"}, step, io.Discard, io.Discard, time.Second)
	if !errors.Is(err, ErrUnsafeEnvironment) {
		t.Fatalf("runStep error = %v, want ErrUnsafeEnvironment", err)
	}
	if result.ExitCode != noChildExit {
		t.Fatalf("step exit code = %d, want %d", result.ExitCode, noChildExit)
	}
	if _, statErr := os.Stat(witness); statErr == nil {
		t.Fatal("the refused program ran")
	}
}

// The reviewed catalog cannot produce the shape this refuses, and that is
// worth holding: the dispatch seam admits bash and the ra8ci: tools only, so
// every reviewed step reaches a program name with no separator in it.
func TestNoReviewedStepCanNameAnAbsoluteProgram(t *testing.T) {
	definitions, err := catalog.Load()
	if err != nil {
		t.Fatalf("load catalog: %v", err)
	}
	names := definitions.Names()
	if len(names) == 0 {
		t.Fatal("the catalog reviews no task at all, so this test proves nothing")
	}
	for _, name := range names {
		task, found := definitions.Task(name)
		if !found {
			t.Fatalf("catalog names task %q and does not hold it", name)
		}
		for _, step := range task.Steps {
			if filepath.IsAbs(step.Program) || strings.ContainsAny(step.Program, "/\\") {
				t.Fatalf("task %q step %q names the program path %q", task.Name, step.Name, step.Program)
			}
		}
	}
}
