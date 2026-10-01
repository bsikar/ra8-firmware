// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"
)

// The bounds and refusals digestTerraformTree meets once a source tree is not
// the ordinary tree of small .tf files. The digest is what an apply is
// approved against, so every one of these has to end in a refusal rather than
// a digest taken over part of the tree: a digest that quietly skipped an
// unreadable file, or stopped at a bound, would still look like an approval
// of the whole tree to everything downstream of it.

// sparseTree writes a tree of files of the given apparent sizes without
// spending the disk. The size bound is read off os.Stat, so a hole answers the
// question exactly as a file of zeros would.
func sparseTree(t *testing.T, sizes map[string]int64) string {
	t.Helper()
	root := t.TempDir()
	for name, size := range sizes {
		path := filepath.Join(root, name)
		if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
			t.Fatalf("create directory for %s: %v", name, err)
		}
		handle, err := os.OpenFile(path, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0o600)
		if err != nil {
			t.Fatalf("create %s: %v", name, err)
		}
		if err := handle.Truncate(size); err != nil {
			t.Fatalf("size %s: %v", name, err)
		}
		if err := handle.Close(); err != nil {
			t.Fatalf("close %s: %v", name, err)
		}
	}
	return root
}

func refusedTree(t *testing.T, root, reason string) {
	t.Helper()
	digest, err := digestTerraformTree(root)
	if err == nil {
		t.Fatalf("tree must be refused, got digest %q", digest)
	}
	if digest != "" {
		t.Fatalf("a refused tree must answer no digest, got %q", digest)
	}
	if !strings.Contains(err.Error(), reason) {
		t.Fatalf("refusal must name %q, got %q", reason, err)
	}
}

func TestTheSourceTreeSizeBoundIsExactAndIsJudgedBeforeTheContentIsRead(t *testing.T) {
	const bound = 64 << 20

	// Exactly at the bound is still a source tree we will vouch for.
	if digest := tfDigest(t, sparseTree(t, map[string]int64{"main.tf": bound})); digest == "" {
		t.Fatal("a tree of exactly the size bound must be digested")
	}

	// One byte past it is refused, and refused before the file is read: the
	// bound exists to stop an unbounded read, so reaching it and then
	// complaining would be the bound doing nothing.
	refusedTree(t, sparseTree(t, map[string]int64{"main.tf": bound + 1}), "size bound")
}

func TestTheSourceTreeSizeBoundIsOverTheWholeTreeNotOneFile(t *testing.T) {
	const half = 33 << 20

	// Two files, each comfortably inside the bound, that pass it together.
	// A per-file reading of the bound would digest this tree.
	refusedTree(t, sparseTree(t, map[string]int64{
		"a.tf":         half,
		"modules/b.tf": half,
	}), "size bound")

	// The same two files under the bound are digested, so the refusal above
	// is the total and nothing else about the shape of the tree.
	if digest := tfDigest(t, sparseTree(t, map[string]int64{
		"a.tf":         half,
		"modules/b.tf": 1 << 20,
	})); digest == "" {
		t.Fatal("two files inside the bound must be digested")
	}
}

func TestANonRegularFileIsRefusedRatherThanDigested(t *testing.T) {
	// A named pipe is the honest version of this: it stats, it walks, and a
	// reader hangs on it. Digesting a tree with one in it would either block
	// the control plane or hash whatever a writer chose to send.
	root := tfTree(t, map[string]string{"main.tf": "resource \"one\" {}\n"})
	if err := syscall.Mkfifo(filepath.Join(root, "pipe.tf"), 0o600); err != nil {
		t.Skipf("named pipes unavailable here: %v", err)
	}
	// Every refusal the walk itself raises is folded into one tree-level
	// reason, so the refusal reads as the tree being unavailable rather than
	// naming the pipe. That is deliberate and worth pinning: the walk's own
	// wording never reaches a caller, so nothing downstream may key on it.
	refusedTree(t, root, "unavailable or unbounded")

	// Depth does not soften it: the walk refuses wherever it meets one.
	nested := tfTree(t, map[string]string{"modules/runner/main.tf": "resource \"one\" {}\n"})
	if err := syscall.Mkfifo(filepath.Join(nested, "modules", "runner", "pipe.tf"), 0o600); err != nil {
		t.Skipf("named pipes unavailable here: %v", err)
	}
	refusedTree(t, nested, "unavailable or unbounded")
}

func TestAnUnreadableSourceFileIsRefusedRatherThanSkipped(t *testing.T) {
	root := tfTree(t, map[string]string{
		"main.tf":   "resource \"one\" {}\n",
		"sealed.tf": "resource \"two\" {}\n",
	})
	sealed := filepath.Join(root, "sealed.tf")
	if err := os.Chmod(sealed, 0o000); err != nil {
		t.Fatalf("seal source file: %v", err)
	}
	t.Cleanup(func() { _ = os.Chmod(sealed, 0o600) })

	// The file is a regular file of a known size, so every check in front of
	// the read passes and only the open refuses. Skipping it would answer the
	// digest of a tree that is missing a file an apply would still run.
	refusedTree(t, root, "cannot be opened")

	// Readable again, the same tree digests, so the refusal was the mode.
	if err := os.Chmod(sealed, 0o600); err != nil {
		t.Fatalf("unseal source file: %v", err)
	}
	if digest := tfDigest(t, root); digest == "" {
		t.Fatal("a readable tree must be digested")
	}
}

func TestTheSourceTreeFileCountBoundIsExact(t *testing.T) {
	const bound = 4096

	atBound := make(map[string]string, bound)
	for i := 0; i < bound; i++ {
		atBound["f"+strconv.Itoa(i)+".tf"] = "\n"
	}
	root := tfTree(t, atBound)
	if digest := tfDigest(t, root); digest == "" {
		t.Fatal("a tree of exactly the file bound must be digested")
	}

	// One more file and the tree is refused as unbounded rather than
	// digested down to the first 4096 entries.
	if err := os.WriteFile(filepath.Join(root, "one-too-many.tf"), []byte("\n"), 0o600); err != nil {
		t.Fatalf("write the file past the bound: %v", err)
	}
	refusedTree(t, root, "unavailable or unbounded")
}

func TestTheModuleDigestCarriesEveryTreeRefusalThrough(t *testing.T) {
	good := tfTree(t, map[string]string{"main.tf": "resource \"one\" {}\n"})

	sealedRoot := tfTree(t, map[string]string{"main.tf": "resource \"two\" {}\n"})
	sealed := filepath.Join(sealedRoot, "main.tf")
	if err := os.Chmod(sealed, 0o000); err != nil {
		t.Fatalf("seal source file: %v", err)
	}
	t.Cleanup(func() { _ = os.Chmod(sealed, 0o600) })

	oversized := sparseTree(t, map[string]int64{"main.tf": (64 << 20) + 1})

	// Either side refusing refuses the pair. A module digest taken with one
	// side skipped would be a stable digest of half the sources.
	for _, pair := range []struct {
		name                string
		environment, module string
	}{
		{"unreadable environment", sealedRoot, good},
		{"unreadable module", good, sealedRoot},
		{"oversized environment", oversized, good},
		{"oversized module", good, oversized},
	} {
		t.Run(pair.name, func(t *testing.T) {
			digest, err := terraformModuleDigest(pair.environment, pair.module)
			if err == nil {
				t.Fatalf("module digest must be refused, got %q", digest)
			}
			if digest != "" {
				t.Fatalf("a refused module digest must be empty, got %q", digest)
			}
		})
	}
}
