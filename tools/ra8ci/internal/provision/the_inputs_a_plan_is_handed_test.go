// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"crypto/sha256"
	"encoding/hex"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/privatefile"
)

// The three pure inputs a Terraform run is handed before anything is planned:
// the state identity the reservation is filed under, the private variables
// file the plan reads, and the digest of the source tree the plan is taken
// against. Nothing here needs a terraform binary or a lab, and all three
// decide what an apply is allowed to be before any of that is reachable.

const (
	tfOperationID      = "0192f3a4-b5c6-7d8e-9f01-23456789abcd"
	tfOtherOperationID = "0192f3a4-b5c6-7d8e-9f01-23456789abce"
)

// tfTree writes a source tree from relative path to contents and answers its
// root. Directories are created as needed, so a key may name a nested file.
func tfTree(t *testing.T, files map[string]string) string {
	t.Helper()
	root := t.TempDir()
	for name, body := range files {
		file := filepath.Join(root, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(file), 0o700); err != nil {
			t.Fatalf("create tree directory for %s: %v", name, err)
		}
		if err := os.WriteFile(file, []byte(body), 0o600); err != nil {
			t.Fatalf("write tree file %s: %v", name, err)
		}
	}
	return root
}

func tfDigest(t *testing.T, root string) string {
	t.Helper()
	digest, err := digestTerraformTree(root)
	if err != nil {
		t.Fatalf("digest tree %s: %v", root, err)
	}
	if len(digest) != 64 {
		t.Fatalf("a tree digest must be a sha256 in hex, got %q", digest)
	}
	return digest
}

func TestRunnerStateIdentityIsTheDomainSeparatedDigestOfTheReservation(t *testing.T) {
	identity := terraformRunnerStateIdentity(tfOperationID)
	expected := sha256.Sum256([]byte("ra8ci-runner-terraform-state:" + tfOperationID))
	if identity != hex.EncodeToString(expected[:]) {
		t.Fatalf("state identity must be the prefixed digest, got %q", identity)
	}
	// The prefix is what keeps this digest out of every other sha256 the
	// package takes of the same reservation ID: a bare digest of the ID
	// would collide with any of them.
	bare := sha256.Sum256([]byte(tfOperationID))
	if identity == hex.EncodeToString(bare[:]) {
		t.Fatal("state identity must not be the bare digest of the reservation ID")
	}
}

func TestEachReservationKeepsItsOwnStateIdentity(t *testing.T) {
	if terraformRunnerStateIdentity(tfOperationID) == terraformRunnerStateIdentity(tfOtherOperationID) {
		t.Fatal("two reservations must not share one Terraform state identity")
	}
	first := terraformRunnerStateIdentity(tfOperationID)
	if first != terraformRunnerStateIdentity(tfOperationID) {
		t.Fatal("a reservation's state identity must be stable across calls")
	}
	// The identity is a naming function, not an admission check: it answers
	// for an empty reservation ID too, and the callers are the ones holding
	// the ID to a shape.
	if len(terraformRunnerStateIdentity("")) != 64 {
		t.Fatal("state identity must answer a digest for any input")
	}
}

func TestVariablesAreWrittenPrivatelyUnderTheirOwnOperation(t *testing.T) {
	workspace := t.TempDir()
	body := []byte(`{"runner":{"vm_id":9001}}`)

	file, err := writeTerraformVariables(workspace, tfOperationID, body)
	if err != nil {
		t.Fatalf("write variables: %v", err)
	}
	if want := filepath.Join(workspace, tfOperationID, "runner.tfvars.json"); file != want {
		t.Fatalf("variables must live under their operation, got %q want %q", file, want)
	}

	written, err := os.ReadFile(file)
	if err != nil {
		t.Fatalf("read variables back: %v", err)
	}
	if string(written) != string(body) {
		t.Fatalf("variables must be written byte for byte, got %q", written)
	}

	info, err := os.Lstat(file)
	if err != nil {
		t.Fatalf("stat variables: %v", err)
	}
	if err := privatefile.Check(file); err != nil {
		t.Fatalf("variables are not private: %v", err)
	}
	if runtime.GOOS != "windows" && info.Mode().Perm() != 0o600 {
		t.Fatalf("variables carry credentials and must be private, got %v", info.Mode().Perm())
	}
	directory, err := os.Lstat(filepath.Dir(file))
	if err != nil {
		t.Fatalf("stat variables directory: %v", err)
	}
	if err := privatefile.CheckDirectory(filepath.Dir(file)); err != nil {
		t.Fatalf("operation directory is not private: %v", err)
	}
	if runtime.GOOS != "windows" && directory.Mode().Perm() != 0o700 {
		t.Fatalf("the operation directory must be private, got %v", directory.Mode().Perm())
	}
}

func TestVariablesRefuseAnOperationThatIsNotAnIdentifier(t *testing.T) {
	for _, operationID := range []string{
		"",
		"not-an-identifier",
		"0192f3a4-b5c6-4d8e-9f01-23456789abcd", // version 4, not 7
		"0192f3a4-b5c6-7d8e-cf01-23456789abcd", // variant outside 89ab
		"../escape",
	} {
		workspace := t.TempDir()
		if _, err := writeTerraformVariables(workspace, operationID, []byte("{}")); err == nil {
			t.Fatalf("operation %q must be refused", operationID)
		}
		// The identifier is judged before anything is created, so a refused
		// operation leaves no directory to be mistaken for a prepared run,
		// and a traversing name never reaches the filesystem at all.
		entries, err := os.ReadDir(workspace)
		if err != nil {
			t.Fatalf("read workspace: %v", err)
		}
		if len(entries) != 0 {
			t.Fatalf("a refused operation must write nothing, found %d entries", len(entries))
		}
	}
}

func TestVariablesAreNeverOverwrittenForTheSameOperation(t *testing.T) {
	workspace := t.TempDir()
	file, err := writeTerraformVariables(workspace, tfOperationID, []byte("first"))
	if err != nil {
		t.Fatalf("write variables: %v", err)
	}

	// A second write under one operation ID is a retry that has lost track of
	// the first, and the inputs a plan was approved against must not change
	// underneath it. The refusal leaves the original standing.
	if _, err := writeTerraformVariables(workspace, tfOperationID, []byte("second")); err == nil {
		t.Fatal("a second write for one operation must be refused")
	}
	standing, err := os.ReadFile(file)
	if err != nil {
		t.Fatalf("read variables back: %v", err)
	}
	if string(standing) != "first" {
		t.Fatalf("the first inputs must stand, got %q", standing)
	}

	// A different operation in the same workspace is a different run and is
	// written normally.
	if _, err := writeTerraformVariables(workspace, tfOtherOperationID, []byte("other")); err != nil {
		t.Fatalf("a distinct operation must be accepted: %v", err)
	}
}

func TestVariablesRequireAnAbsoluteWorkspace(t *testing.T) {
	_, err := writeTerraformVariables(filepath.Join("relative", "workspace"), tfOperationID, []byte("{}"))
	if err == nil {
		t.Fatal("a relative workspace must be refused")
	}
	if !strings.Contains(err.Error(), "absolute") {
		t.Fatalf("the refusal must name the absolute path requirement, got %v", err)
	}
}

func TestTreeDigestIsStableAndIndependentOfWriteOrder(t *testing.T) {
	first := tfTree(t, map[string]string{
		"main.tf":            "resource \"one\" {}\n",
		"variables.tf":       "variable \"two\" {}\n",
		"modules/runner.tf":  "module \"three\" {}\n",
		"modules/outputs.tf": "output \"four\" {}\n",
	})
	second := tfTree(t, map[string]string{
		"modules/outputs.tf": "output \"four\" {}\n",
		"modules/runner.tf":  "module \"three\" {}\n",
		"variables.tf":       "variable \"two\" {}\n",
		"main.tf":            "resource \"one\" {}\n",
	})

	digest := tfDigest(t, first)
	if digest != tfDigest(t, first) {
		t.Fatal("one tree must digest the same twice")
	}
	// The digest is over sorted paths, so two trees holding the same sources
	// answer the same however the filesystem hands them back.
	if digest != tfDigest(t, second) {
		t.Fatal("two identical trees must share one digest")
	}
	// And the root the tree was reached by is not part of the answer, which
	// is what lets a digest taken in one checkout be compared with one taken
	// in another.
	if digest != tfDigest(t, first+string(filepath.Separator)+".") {
		t.Fatal("the digest must not depend on how the root was spelled")
	}
}

func TestTreeDigestMovesWithContentAndWithPath(t *testing.T) {
	root := tfTree(t, map[string]string{"main.tf": "resource \"one\" {}\n"})
	before := tfDigest(t, root)

	if err := os.WriteFile(filepath.Join(root, "main.tf"), []byte("resource \"one\" {}"), 0o600); err != nil {
		t.Fatalf("edit source: %v", err)
	}
	if tfDigest(t, root) == before {
		t.Fatal("dropping one byte of a source must change the digest")
	}

	// Paths are hashed alongside contents, so moving a file without editing
	// it is a change: a module reached by a different name is a different
	// module to Terraform.
	moved := tfTree(t, map[string]string{"main.tf": "resource \"one\" {}\n"})
	renamed := tfTree(t, map[string]string{"other.tf": "resource \"one\" {}\n"})
	if tfDigest(t, moved) == tfDigest(t, renamed) {
		t.Fatal("the same bytes under a different name must digest differently")
	}

	// The separator between a path and its contents is what stops the two
	// running together: these two trees hold the same byte stream and must
	// not collide.
	left := tfTree(t, map[string]string{"a.tf": "bc"})
	right := tfTree(t, map[string]string{"a.tfb": "c"})
	if tfDigest(t, left) == tfDigest(t, right) {
		t.Fatal("a path and its contents must not run together in the digest")
	}
}

func TestTreeDigestIsTheSortedRelativePathAndContentStream(t *testing.T) {
	root := tfTree(t, map[string]string{
		"a.tf":     "alpha\n",
		"sub/b.tf": "beta\n",
	})
	expected := sha256.Sum256([]byte("a.tf\x00alpha\n" + filepath.Join("sub", "b.tf") + "\x00beta\n"))
	if got := tfDigest(t, root); got != hex.EncodeToString(expected[:]) {
		t.Fatalf("digest must be the sorted relative path and content stream, got %q", got)
	}
}

func TestAnEmptyOrMissingTreeIsRefusedRatherThanDigested(t *testing.T) {
	// An empty tree is the dangerous case: a fixed digest over nothing would
	// let an apply be approved against sources that were never there.
	if _, err := digestTerraformTree(t.TempDir()); err == nil {
		t.Fatal("an empty tree must be refused")
	}
	if _, err := digestTerraformTree(filepath.Join(t.TempDir(), "absent")); err == nil {
		t.Fatal("a missing tree must be refused")
	}
	// A directory holding only directories is empty for this purpose too.
	root := t.TempDir()
	if err := os.MkdirAll(filepath.Join(root, "modules", "runner"), 0o700); err != nil {
		t.Fatalf("create empty directories: %v", err)
	}
	if _, err := digestTerraformTree(root); err == nil {
		t.Fatal("a tree of empty directories must be refused")
	}
}

func TestASymlinkInTheTreeIsRefusedRatherThanFollowed(t *testing.T) {
	root := tfTree(t, map[string]string{"main.tf": "resource \"one\" {}\n"})
	outside := tfTree(t, map[string]string{"secret.tf": "resource \"elsewhere\" {}\n"})
	symlinkTest(t, filepath.Join(outside, "secret.tf"), filepath.Join(root, "linked.tf"))

	// A followed symlink would digest sources that are not in the tree, and
	// could be repointed between the digest and the apply.
	if _, err := digestTerraformTree(root); err == nil {
		t.Fatal("a symlink in the source tree must be refused")
	}

	// A symlinked directory is refused on the same reading.
	other := tfTree(t, map[string]string{"main.tf": "resource \"one\" {}\n"})
	symlinkTest(t, outside, filepath.Join(other, "vendored"))
	if _, err := digestTerraformTree(other); err == nil {
		t.Fatal("a symlinked directory in the source tree must be refused")
	}
}

func TestTheTerraformCacheIsSkippedAndEverythingElseIsNot(t *testing.T) {
	plain := tfTree(t, map[string]string{"main.tf": "resource \"one\" {}\n"})
	cached := tfTree(t, map[string]string{
		"main.tf":                                "resource \"one\" {}\n",
		".terraform/providers/registry/provider": "downloaded bytes\n",
		".terraform/modules/modules.json":        "{}\n",
	})

	// The cache is whatever a previous init downloaded, so counting it would
	// make the digest depend on the machine rather than on the sources.
	if tfDigest(t, plain) != tfDigest(t, cached) {
		t.Fatal("the .terraform cache must not reach the digest")
	}

	// The skip is exact: only that name, and only as a directory.
	named := tfTree(t, map[string]string{
		"main.tf":              "resource \"one\" {}\n",
		".terraform.lock.hcl":  "provider lock\n",
		"terraform/helper.tf":  "module \"helper\" {}\n",
		".terraformrc/main.tf": "resource \"two\" {}\n",
	})
	if tfDigest(t, named) == tfDigest(t, plain) {
		t.Fatal("only a .terraform directory is skipped, not every name near it")
	}
}

func TestModuleDigestBindsTheEnvironmentAndTheModuleInThatOrder(t *testing.T) {
	environment := tfTree(t, map[string]string{"main.tf": "environment\n"})
	module := tfTree(t, map[string]string{"main.tf": "module\n"})

	digest, err := terraformModuleDigest(environment, module)
	if err != nil {
		t.Fatalf("module digest: %v", err)
	}
	expected := sha256.Sum256([]byte(tfDigest(t, environment) + ":" + tfDigest(t, module)))
	if digest != hex.EncodeToString(expected[:]) {
		t.Fatalf("module digest must join the two tree digests, got %q", digest)
	}

	// The two trees are not interchangeable: swapping them is a different
	// apply, and the separator is what keeps a pair from being read as one
	// long digest.
	swapped, err := terraformModuleDigest(module, environment)
	if err != nil {
		t.Fatalf("module digest swapped: %v", err)
	}
	if swapped == digest {
		t.Fatal("the environment and the module must not be interchangeable")
	}

	// A change in either tree moves the answer.
	if err := os.WriteFile(filepath.Join(module, "main.tf"), []byte("module edited\n"), 0o600); err != nil {
		t.Fatalf("edit module: %v", err)
	}
	edited, err := terraformModuleDigest(environment, module)
	if err != nil {
		t.Fatalf("module digest after edit: %v", err)
	}
	if edited == digest {
		t.Fatal("editing the module must change the pair's digest")
	}
}

func TestModuleDigestRefusesWhenEitherTreeIsUnreadable(t *testing.T) {
	good := tfTree(t, map[string]string{"main.tf": "resource \"one\" {}\n"})
	missing := filepath.Join(t.TempDir(), "absent")

	if _, err := terraformModuleDigest(missing, good); err == nil {
		t.Fatal("a missing environment tree must be refused")
	}
	// The environment is digested first, so a run with both trees missing
	// still refuses rather than answering a digest over nothing.
	if _, err := terraformModuleDigest(good, missing); err == nil {
		t.Fatal("a missing module tree must be refused")
	}
	if _, err := terraformModuleDigest(missing, missing); err == nil {
		t.Fatal("two missing trees must be refused")
	}
	if _, err := terraformModuleDigest(t.TempDir(), good); err == nil {
		t.Fatal("an empty environment tree must be refused")
	}
}
