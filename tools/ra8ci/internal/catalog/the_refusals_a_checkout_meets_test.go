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
)

// The canonicalizer decides whether a manifest is even a document before its
// digest is weighed, and VerifyCheckout decides whether the checkout in hand
// is the reviewed one. Both refuse ahead of anything expensive, and these are
// the refusals a broken checkout actually meets.

func TestADigestIsRefusedUnlessItIsSixtyFourLowercaseHex(t *testing.T) {
	sound := strings.Repeat("ab", 32)
	if digest, err := parseDigest("  " + sound + "\n"); err != nil || digest != sound {
		t.Fatalf("a padded sound digest was refused: %q %v", digest, err)
	}
	for name, value := range map[string]string{
		"empty":            "",
		"too short":        strings.Repeat("ab", 31),
		"too long":         strings.Repeat("ab", 33),
		"uppercase hex":    strings.ToUpper(sound),
		"not hex at all":   strings.Repeat("zz", 32),
		"hex with a space": strings.Repeat("ab", 31) + " c",
	} {
		if _, err := parseDigest(value); !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("a %s digest was not refused: %v", name, err)
		}
	}
}

// Uppercase is refused even though it decodes as hex, because the digest is
// compared as text against a lowercase encoding.
func TestAnUppercaseDigestDecodesAsHexAndIsStillRefused(t *testing.T) {
	upper := strings.ToUpper(strings.Repeat("ab", 32))
	if _, err := parseDigest(upper); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("an uppercase digest was accepted: %v", err)
	}
}

func TestADocumentWithAnythingAfterItIsRefused(t *testing.T) {
	for name, raw := range map[string]string{
		"a second object":  `{"a":1} {"b":2}`,
		"a trailing list":  `{"a":1} []`,
		"a trailing word":  `{"a":1} true`,
		"a trailing brace": `{"a":1} }`,
	} {
		if _, err := CanonicalJSON([]byte(raw)); !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("%s after the document was not refused: %v", name, err)
		}
	}
}

func TestADocumentThatIsNotOneValueIsRefused(t *testing.T) {
	for name, raw := range map[string]string{
		"empty":            ``,
		"only whitespace":  "  \n\t",
		"an unclosed list": `[1, 2`,
		"a bare closer":    `}`,
		"a duplicate key":  `{"a":1,"a":2}`,
	} {
		if _, err := CanonicalJSON([]byte(raw)); !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("%s was not refused: %v", name, err)
		}
	}
}

// A duplicate key is refused wherever it sits, because a decoder that keeps
// the last one would canonicalize two different documents to the same bytes.
func TestADuplicateKeyIsRefusedAtEveryDepth(t *testing.T) {
	for name, raw := range map[string]string{
		"at the top":     `{"a":1,"a":2}`,
		"one level down": `{"outer":{"a":1,"a":2}}`,
		"inside a list":  `{"items":[{"a":1,"a":2}]}`,
	} {
		if _, err := CanonicalJSON([]byte(raw)); !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("a duplicate key %s was not refused: %v", name, err)
		}
	}
}

func TestCanonicalJSONSortsKeysAndDropsWhitespace(t *testing.T) {
	canonical, err := CanonicalJSON([]byte("{\n  \"b\": 2,\n  \"a\": [1, {\"d\": 4, \"c\": 3}]\n}"))
	if err != nil {
		t.Fatalf("a sound document was refused: %v", err)
	}
	if got := string(canonical); got != `{"a":[1,{"c":3,"d":4}],"b":2}` {
		t.Fatalf("canonical form is %s", got)
	}
}

// expectEOF is the guard behind both of those: a decoder already at the end
// says so, and one holding another value refuses by name.
func TestADecoderHoldingAnotherValueIsRefusedByName(t *testing.T) {
	spent := json.NewDecoder(strings.NewReader(`{"a":1}`))
	var first any
	if err := spent.Decode(&first); err != nil {
		t.Fatalf("the first value would not decode: %v", err)
	}
	if err := expectEOF(spent); err != nil {
		t.Fatalf("a spent decoder was refused: %v", err)
	}
	holding := json.NewDecoder(strings.NewReader(`{"a":1} 7`))
	if err := holding.Decode(&first); err != nil {
		t.Fatalf("the first value would not decode: %v", err)
	}
	err := expectEOF(holding)
	if !errors.Is(err, ErrInvalidCatalog) || !strings.Contains(err.Error(), "trailing JSON value") {
		t.Fatalf("a decoder holding another value was not refused by name: %v", err)
	}
}

// plantCheckout writes the shape VerifyCheckout reads: a .git marker and the
// catalog pair under tools/ra8ci/catalog.
func plantCheckout(t *testing.T, manifest, digest string) string {
	t.Helper()
	root := t.TempDir()
	if err := os.Mkdir(filepath.Join(root, ".git"), 0o755); err != nil {
		t.Fatalf("plant .git: %v", err)
	}
	base := filepath.Join(root, "tools", "ra8ci", "catalog")
	if err := os.MkdirAll(base, 0o755); err != nil {
		t.Fatalf("plant catalog directory: %v", err)
	}
	if manifest != "" {
		if err := os.WriteFile(filepath.Join(base, "tasks.json"), []byte(manifest), 0o644); err != nil {
			t.Fatalf("plant manifest: %v", err)
		}
	}
	if digest != "" {
		if err := os.WriteFile(filepath.Join(base, "sha256.txt"), []byte(digest), 0o644); err != nil {
			t.Fatalf("plant digest: %v", err)
		}
	}
	return root
}

func TestACheckoutPathThatDoesNotResolveIsRefused(t *testing.T) {
	absent := filepath.Join(t.TempDir(), "no-such-checkout")
	if _, err := VerifyCheckout(absent); !errors.Is(err, ErrInvalidCheckout) {
		t.Fatalf("a checkout path that does not exist was not refused: %v", err)
	}
	if _, err := VerifyCheckout(""); !errors.Is(err, ErrInvalidCheckout) {
		t.Fatalf("an empty checkout root was not refused: %v", err)
	}
}

// A checkout can hold a manifest that is sound, matches its own digest, and is
// still not the catalog this binary was built against. That is the case the
// digest comparison exists for, and it is the one a stale checkout hits.
func TestASoundButDifferentCatalogIsADigestMismatch(t *testing.T) {
	embedded, err := Load()
	if err != nil {
		t.Fatalf("the embedded catalog would not load: %v", err)
	}
	raw, err := os.ReadFile(filepath.Join("..", "..", "catalog", "tasks.json"))
	if err != nil {
		t.Skipf("the checkout manifest is not readable from here: %v", err)
	}
	var document map[string]json.RawMessage
	if err := json.Unmarshal(raw, &document); err != nil {
		t.Fatalf("the checkout manifest would not decode: %v", err)
	}
	// One key nothing reads, which changes the canonical bytes and so the
	// digest, while leaving every reviewed task exactly as it was.
	document["schema_version"] = json.RawMessage("1")
	altered, err := json.Marshal(document)
	if err != nil {
		t.Fatalf("the altered manifest would not encode: %v", err)
	}
	canonical, err := CanonicalJSON(altered)
	if err != nil {
		t.Fatalf("the altered manifest is not canonicalizable: %v", err)
	}
	if string(canonical) == string(mustCanonical(t, raw)) {
		t.Skip("the alteration did not change the canonical bytes")
	}
	root := plantCheckout(t, string(altered), digestOf(t, altered))
	_, err = VerifyCheckout(root)
	if err == nil {
		t.Fatalf("a checkout carrying a different catalog was accepted as %s", embedded.Digest())
	}
	if !errors.Is(err, ErrDigestMismatch) && !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("the refusal is neither a digest mismatch nor an invalid catalog: %v", err)
	}
}

func TestAManifestThatDoesNotMatchItsOwnDigestIsRefused(t *testing.T) {
	root := plantCheckout(t, `{"schema_version":1,"tasks":[]}`, strings.Repeat("ab", 32))
	if _, err := VerifyCheckout(root); !errors.Is(err, ErrDigestMismatch) {
		t.Fatalf("a manifest that does not match its stated digest was not refused: %v", err)
	}
}

func TestACheckoutMissingEitherHalfOfThePairIsRefused(t *testing.T) {
	sound := `{"schema_version":1,"tasks":[]}`
	for name, root := range map[string]string{
		"no manifest": plantCheckout(t, "", strings.Repeat("ab", 32)),
		"no digest":   plantCheckout(t, sound, ""),
	} {
		if _, err := VerifyCheckout(root); !errors.Is(err, ErrInvalidCheckout) {
			t.Fatalf("a checkout with %s was not refused: %v", name, err)
		}
	}
}

func mustCanonical(t *testing.T, raw []byte) []byte {
	t.Helper()
	canonical, err := CanonicalJSON(raw)
	if err != nil {
		t.Fatalf("canonicalize: %v", err)
	}
	return canonical
}
