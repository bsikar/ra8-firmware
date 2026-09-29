// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"bytes"
	"encoding/json"
	"regexp"
	"testing"
)

// This package hands the reviewed task manifest and its digest to every
// caller that needs to prove which catalog a run was judged against. Both
// accessors copy on the way out, and nothing here asserted that until now:
// returning the embedded slice itself would let any caller edit the catalog
// in place, after the digest that vouches for it had already been read.

// A caller that writes into what it was handed must not reach the embedded
// bytes the next caller will be given.
func TestManifestHandsBackACopyRatherThanTheEmbeddedBytes(t *testing.T) {
	first := Manifest()
	if len(first) == 0 {
		t.Fatal("the embedded manifest is empty")
	}
	original := append([]byte(nil), first...)
	for i := range first {
		first[i] ^= 0xFF
	}
	if second := Manifest(); !bytes.Equal(second, original) {
		t.Fatal("editing a returned manifest changed what the next caller is handed")
	}
}

func TestDigestHandsBackACopyRatherThanTheEmbeddedBytes(t *testing.T) {
	first := Digest()
	if len(first) == 0 {
		t.Fatal("the embedded digest is empty")
	}
	original := append([]byte(nil), first...)
	for i := range first {
		first[i] ^= 0xFF
	}
	if second := Digest(); !bytes.Equal(second, original) {
		t.Fatal("editing a returned digest changed what the next caller is handed")
	}
}

// The digest is read as the expected SHA-256 of the catalog's canonical JSON,
// so the file has to carry exactly one lowercase hex sum and nothing else. A
// second line, an uppercase sum, or a stray comment would be refused at load
// time, on a binary that had already shipped.
func TestTheReviewedDigestFileCarriesOneLowercaseSHA256(t *testing.T) {
	raw := string(Digest())
	trimmed := raw
	if n := len(trimmed); n > 0 && trimmed[n-1] == '\n' {
		trimmed = trimmed[:n-1]
	}
	if !regexp.MustCompile(`^[0-9a-f]{64}$`).MatchString(trimmed) {
		t.Fatalf("digest file is not one lowercase SHA-256: %q", raw)
	}
}

// The manifest is decoded with unknown fields refused, so it has to be a JSON
// object rather than an array or a bare value.
func TestTheEmbeddedManifestIsAJSONObject(t *testing.T) {
	var document map[string]json.RawMessage
	if err := json.Unmarshal(Manifest(), &document); err != nil {
		t.Fatalf("the embedded manifest is not a JSON object: %v", err)
	}
	if len(document) == 0 {
		t.Fatal("the embedded manifest object is empty")
	}
}
