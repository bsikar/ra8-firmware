// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	embedded "github.com/bsikar/ra8-firmware/tools/ra8ci/catalog"
)

// Two disagreements a manifest can carry that every check ahead of them is
// happy with, because both are internally consistent: a manifest that names
// one task twice, and a checkout whose catalog is valid, correctly digested,
// and simply not the catalog this binary was built with.

// rewrittenManifest hands back the embedded manifest with its task list
// replaced, digested to match, so the only thing under test is the list.
func rewrittenManifest(t *testing.T, rewrite func([]json.RawMessage) []json.RawMessage) []byte {
	t.Helper()
	var document map[string]json.RawMessage
	if err := json.Unmarshal(embedded.Manifest(), &document); err != nil {
		t.Fatal(err)
	}
	var tasks []json.RawMessage
	if err := json.Unmarshal(document["tasks"], &tasks); err != nil {
		t.Fatal(err)
	}
	replaced, err := json.Marshal(rewrite(tasks))
	if err != nil {
		t.Fatal(err)
	}
	document["tasks"] = replaced
	raw, err := json.Marshal(document)
	if err != nil {
		t.Fatal(err)
	}
	return raw
}

// A manifest may not name one task twice. Both copies are valid on their own
// and the document is valid JSON, so nothing before the task table catches
// it: the refusal has to come from the table itself, and it has to name the
// task rather than silently keeping whichever copy was read last.
func TestParseRefusesAManifestThatNamesOneTaskTwice(t *testing.T) {
	raw := rewrittenManifest(t, func(tasks []json.RawMessage) []json.RawMessage {
		return append(tasks, tasks[0])
	})

	_, err := Parse(raw, digestOf(t, raw))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a manifest naming one task twice answered %v, want an invalid catalog", err)
	}
	if !strings.Contains(err.Error(), "duplicate task") {
		t.Fatalf("the refusal does not say what is wrong: %v", err)
	}
}

// A checkout whose catalog is internally sound but is not the catalog this
// binary carries is refused as a digest mismatch. This is the disagreement
// that matters in practice: a working tree a step behind the running control
// plane reads perfectly well on its own terms, and running it would dispatch
// definitions the server never agreed to.
func TestVerifyCheckoutRefusesACatalogThatIsNotTheEmbeddedOne(t *testing.T) {
	shortened := rewrittenManifest(t, func(tasks []json.RawMessage) []json.RawMessage {
		return tasks[:len(tasks)-1]
	})
	root := t.TempDir()
	base := filepath.Join(root, "tools", "ra8ci", "catalog")
	if err := os.MkdirAll(base, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.Mkdir(filepath.Join(root, ".git"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(base, "tasks.json"), shortened, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(base, "sha256.txt"), []byte(digestOf(t, shortened)+"\n"), 0o644); err != nil {
		t.Fatal(err)
	}

	// The checkout's own manifest and digest agree with each other, so the
	// refusal is about the two catalogs disagreeing and nothing else.
	if _, err := Parse(shortened, digestOf(t, shortened)); err != nil {
		t.Fatalf("the planted catalog is not readable on its own terms: %v", err)
	}
	if _, err := VerifyCheckout(root); !errors.Is(err, ErrDigestMismatch) {
		t.Fatalf("a checkout carrying another catalog answered %v, want a digest mismatch", err)
	}
}
