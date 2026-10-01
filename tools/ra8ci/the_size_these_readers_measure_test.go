// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"bytes"
	"strings"
	"testing"
)

// paddedToExactly returns head + padding + tail sized to exactly size bytes.
//
// The padding is JSON whitespace placed INSIDE the object, before its closing
// brace, so the document stays valid however long it grows. That is what lets
// a test hand a reader a document it can actually finish decoding while still
// being one byte over the bound, which is the only way the measured refusal
// is reached at all: a document built past the limit is truncated by the
// limit reader and fails as unreadable long before anything is measured.
func paddedToExactly(t *testing.T, head, tail string, size int) string {
	t.Helper()
	padding := size - len(head) - len(tail)
	if padding < 0 {
		t.Fatalf("a %d-byte document cannot hold %d bytes of JSON", size, len(head)+len(tail))
	}
	document := head + strings.Repeat(" ", padding) + tail
	if len(document) != size {
		t.Fatalf("document is %d bytes; want %d", len(document), size)
	}
	return document
}

// A document the reader could decode, but only by reading one byte more than
// it agreed to read, is refused on its SIZE and says so.
//
// This is a different refusal from the one an over-long document gets. Past
// the limit the reader never sees the end of the document at all and refuses
// it as unreadable, which is correct but says nothing about how big it was.
// The measured refusal is the one that tells an operator piping in a real
// corpus that the corpus is the problem rather than the pipe, so it is worth
// having a document that actually reaches it.
func TestTheGateCommandsRefuseADocumentTheyMeasuredOverTheBound(t *testing.T) {
	t.Run("evidence gate", func(t *testing.T) {
		commit := agreeingCommit(t)
		document := paddedToExactly(t,
			`{"threshold":1,"commits":[`+commit+`],"required":[]`, `}`,
			maxShadowEvidenceBytes+1)

		var out bytes.Buffer
		err := githubEvidenceGate(strings.NewReader(document), &out)
		if err == nil {
			t.Fatal("a document over the bound was accepted")
		}
		if !strings.Contains(err.Error(), "larger than") {
			t.Fatalf("err = %v; want the size named", err)
		}
		if !strings.Contains(err.Error(), "read evidence gate document") {
			t.Fatalf("err = %v; want the read named", err)
		}
		if out.Len() != 0 {
			t.Fatalf("a refused document wrote a plan: %q", out.String())
		}
	})

	t.Run("shadow evidence", func(t *testing.T) {
		commit := agreeingCommit(t)
		document := paddedToExactly(t,
			`{"threshold":1,"commits":[`+commit+`]`, `}`,
			maxShadowEvidenceBytes+1)

		if _, err := readShadowEvidence(strings.NewReader(document)); err == nil {
			t.Fatal("a document over the bound was accepted")
		} else if !strings.Contains(err.Error(), "larger than") {
			t.Fatalf("err = %v; want the size named", err)
		} else if !strings.Contains(err.Error(), "read shadow evidence") {
			t.Fatalf("err = %v; want the read named", err)
		}
	})
}

// Exactly at the bound is accepted, so the refusal above is the bound doing
// its work rather than the readers simply disliking long documents. A corpus
// that fits must not be turned away for being close.
func TestTheGateCommandsReadADocumentThatFillsTheBound(t *testing.T) {
	t.Run("evidence gate", func(t *testing.T) {
		commit := agreeingCommit(t)
		document := paddedToExactly(t,
			`{"threshold":1,"commits":[`+commit+`],"required":[]`, `}`,
			maxShadowEvidenceBytes)

		var out bytes.Buffer
		if err := githubEvidenceGate(strings.NewReader(document), &out); err != nil {
			t.Fatalf("a document exactly at the bound was refused: %v", err)
		}
		if out.Len() == 0 {
			t.Fatal("an accepted document wrote no plan")
		}
	})

	t.Run("shadow evidence", func(t *testing.T) {
		commit := agreeingCommit(t)
		document := paddedToExactly(t,
			`{"threshold":1,"commits":[`+commit+`]`, `}`,
			maxShadowEvidenceBytes)

		answer, err := readShadowEvidence(strings.NewReader(document))
		if err != nil {
			t.Fatalf("a document exactly at the bound was refused: %v", err)
		}
		if answer.CatalogDigest == "" {
			t.Fatal("an accepted document answered without a catalog digest")
		}
	})
}

// A document built past the bound is refused as unreadable, not as oversized,
// because the limit reader ends it mid-document and the decoder never reaches
// the closing brace. Both wordings name the read, so a caller reading only
// the prefix cannot tell them apart; the tail of the message is where the
// difference lives, and it is worth keeping distinct.
func TestTheGateCommandsRefuseATruncatedDocumentAsUnreadable(t *testing.T) {
	commit := agreeingCommit(t)
	var document strings.Builder
	document.WriteString(`{"threshold":1,"commits":[` + commit)
	for document.Len() <= maxShadowEvidenceBytes {
		document.WriteString("," + commit)
	}
	document.WriteString(`],"required":[]}`)

	var out bytes.Buffer
	err := githubEvidenceGate(strings.NewReader(document.String()), &out)
	if err == nil {
		t.Fatal("a truncated document was accepted")
	}
	if !strings.Contains(err.Error(), "read evidence gate document") {
		t.Fatalf("err = %v; want the read named", err)
	}
	if strings.Contains(err.Error(), "larger than") {
		t.Fatalf("err = %v; want the unreadable wording, not the measured one", err)
	}
	if out.Len() != 0 {
		t.Fatalf("a refused document wrote a plan: %q", out.String())
	}
}
