// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	embedded "github.com/bsikar/ra8-firmware/tools/ra8ci/catalog"
)

// checkoutForTest writes a tree carrying the embedded catalog and returns its
// root and the catalog directory inside it.
func checkoutForTest(t *testing.T) (string, string) {
	t.Helper()
	root := t.TempDir()
	if err := os.Mkdir(filepath.Join(root, ".git"), 0700); err != nil {
		t.Fatal(err)
	}
	base := filepath.Join(root, "tools", "ra8ci", "catalog")
	if err := os.MkdirAll(base, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(base, "tasks.json"), embedded.Manifest(), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(base, "sha256.txt"), embedded.Digest(), 0600); err != nil {
		t.Fatal(err)
	}
	return root, base
}

func TestACheckoutCarryingTheReviewedCatalogIsStillVerified(t *testing.T) {
	root, _ := checkoutForTest(t)
	verified, err := VerifyCheckout(root)
	if err != nil || verified != root {
		t.Fatalf("VerifyCheckout = %q, %v", verified, err)
	}
}

func TestAManifestNamingAnotherFileIsRefused(t *testing.T) {
	root, base := checkoutForTest(t)
	elsewhere := filepath.Join(t.TempDir(), "tasks.json")
	if err := os.WriteFile(elsewhere, embedded.Manifest(), 0600); err != nil {
		t.Fatal(err)
	}
	manifest := filepath.Join(base, "tasks.json")
	if err := os.Remove(manifest); err != nil {
		t.Fatal(err)
	}
	symlinkTest(t, elsewhere, manifest)
	if _, err := VerifyCheckout(root); !errors.Is(err, ErrInvalidCheckout) {
		t.Fatalf("linked manifest error = %v", err)
	}
}

func TestADigestNamingAnotherFileIsRefused(t *testing.T) {
	root, base := checkoutForTest(t)
	elsewhere := filepath.Join(t.TempDir(), "sha256.txt")
	if err := os.WriteFile(elsewhere, embedded.Digest(), 0600); err != nil {
		t.Fatal(err)
	}
	digest := filepath.Join(base, "sha256.txt")
	if err := os.Remove(digest); err != nil {
		t.Fatal(err)
	}
	symlinkTest(t, elsewhere, digest)
	if _, err := VerifyCheckout(root); !errors.Is(err, ErrInvalidCheckout) {
		t.Fatalf("linked digest error = %v", err)
	}
}

func TestAManifestThatIsADirectoryIsRefused(t *testing.T) {
	root, base := checkoutForTest(t)
	manifest := filepath.Join(base, "tasks.json")
	if err := os.Remove(manifest); err != nil {
		t.Fatal(err)
	}
	if err := os.Mkdir(manifest, 0700); err != nil {
		t.Fatal(err)
	}
	if _, err := VerifyCheckout(root); !errors.Is(err, ErrInvalidCheckout) {
		t.Fatalf("directory manifest error = %v", err)
	}
}

// A manifest past the bound is refused for its size, not walked three times and
// then refused for a digest that could never have matched.
func TestAManifestPastTheBoundIsRefusedBeforeTheDigest(t *testing.T) {
	root, base := checkoutForTest(t)
	padded := append([]byte(`{"schema_version":1,"pad":"`), []byte(strings.Repeat("a", maxReadableManifestBytes))...)
	padded = append(padded, []byte(`","tasks":[]}`)...)
	if err := os.WriteFile(filepath.Join(base, "tasks.json"), padded, 0600); err != nil {
		t.Fatal(err)
	}
	_, err := VerifyCheckout(root)
	if !errors.Is(err, ErrInvalidCheckout) {
		t.Fatalf("oversized manifest error = %v", err)
	}
	if errors.Is(err, ErrDigestMismatch) || errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("oversized manifest was read before it was refused: %v", err)
	}
}

func TestADigestFilePastTheBoundIsRefused(t *testing.T) {
	root, base := checkoutForTest(t)
	padded := append(embedded.Digest(), []byte(strings.Repeat("\n", maxReadableDigestBytes))...)
	if err := os.WriteFile(filepath.Join(base, "sha256.txt"), padded, 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := VerifyCheckout(root); !errors.Is(err, ErrInvalidCheckout) {
		t.Fatalf("oversized digest error = %v", err)
	}
}

// A manifest sitting well inside the bound is read whole, trailing newline and
// all: the bound is a ceiling on an untrusted read, not a shape rule.
func TestAManifestInsideTheBoundIsReadWhole(t *testing.T) {
	_, base := checkoutForTest(t)
	raw, err := readCheckoutFile(filepath.Join(base, "tasks.json"), maxReadableManifestBytes)
	if err != nil {
		t.Fatalf("readCheckoutFile: %v", err)
	}
	if len(raw) != len(embedded.Manifest()) {
		t.Fatalf("read %d bytes, manifest is %d", len(raw), len(embedded.Manifest()))
	}
}

// A missing file is still reported as missing, not as a file of the wrong kind.
func TestAMissingManifestIsStillReportedAsMissing(t *testing.T) {
	root, base := checkoutForTest(t)
	if err := os.Remove(filepath.Join(base, "tasks.json")); err != nil {
		t.Fatal(err)
	}
	_, err := VerifyCheckout(root)
	if !errors.Is(err, ErrInvalidCheckout) {
		t.Fatalf("missing manifest error = %v", err)
	}
	if strings.Contains(err.Error(), "not a regular file") {
		t.Fatalf("missing manifest reported as the wrong kind of file: %v", err)
	}
}
