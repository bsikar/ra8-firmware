// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"bytes"
	"strings"
	"testing"
)

// The required-contexts reader bounds its document at 64 KiB and then
// measures what it read. The measured refusal is a different answer from the
// one a document built past the bound gets: past the bound the limit reader
// ends the document mid-flight and it is refused as unreadable, which says
// nothing about how big it was. Only a document that DECODES and still lands
// over the bound reaches the measurement, and that is the answer telling an
// operator their contexts list is the problem rather than their pipe.
func TestTheRequiredCheckReaderRefusesADocumentItMeasuredOverTheBound(t *testing.T) {
	shadowCompareEnv(t, "build")
	document := paddedToExactly(t, `{"required":["ci/one"]`, `}`, maxRequiredCheckBytes+1)

	var out bytes.Buffer
	err := githubRequiredChecks(strings.NewReader(document), &out)
	if err == nil {
		t.Fatal("a document over the bound was accepted")
	}
	if !strings.Contains(err.Error(), "larger than") {
		t.Fatalf("err = %v; want the size named", err)
	}
	if !strings.Contains(err.Error(), "read the required contexts") {
		t.Fatalf("err = %v; want the read named", err)
	}
	if out.Len() != 0 {
		t.Fatalf("a refused document wrote a plan: %q", out.String())
	}
}

// Exactly at the bound is read and planned, so the refusal above is the bound
// doing its work rather than a dislike of long documents. A repository that
// really does declare that many contexts must not be turned away for being
// close to the limit.
func TestTheRequiredCheckReaderReadsADocumentThatFillsTheBound(t *testing.T) {
	shadowCompareEnv(t, "build")
	document := paddedToExactly(t, `{"required":["ci/one"]`, `}`, maxRequiredCheckBytes)

	var out bytes.Buffer
	if err := githubRequiredChecks(strings.NewReader(document), &out); err != nil {
		t.Fatalf("a document exactly at the bound was refused: %v", err)
	}
	if !strings.Contains(out.String(), "catalog_digest") {
		t.Fatalf("an accepted document wrote no plan: %q", out.String())
	}
}

// A document built past the bound keeps the unreadable wording. Both refusals
// open with the same read name, so the tail of the message is the only place
// the two are distinguishable, and it is worth keeping them apart.
func TestTheRequiredCheckReaderRefusesATruncatedDocumentAsUnreadable(t *testing.T) {
	shadowCompareEnv(t, "build")
	var document strings.Builder
	document.WriteString(`{"required":["ci/one"`)
	for document.Len() <= maxRequiredCheckBytes {
		document.WriteString(`,"ci/one"`)
	}
	document.WriteString(`]}`)

	var out bytes.Buffer
	err := githubRequiredChecks(strings.NewReader(document.String()), &out)
	if err == nil {
		t.Fatal("a truncated document was accepted")
	}
	if !strings.Contains(err.Error(), "read the required contexts") {
		t.Fatalf("err = %v; want the read named", err)
	}
	if strings.Contains(err.Error(), "larger than") {
		t.Fatalf("err = %v; want the unreadable wording, not the measured one", err)
	}
	if out.Len() != 0 {
		t.Fatalf("a refused document wrote a plan: %q", out.String())
	}
}

// Trailing content after the document is its own refusal, ahead of any
// measurement. Two documents in one pipe is an operator mistake that a reader
// silently taking the first one would hide.
func TestTheRequiredCheckReaderRefusesTrailingContent(t *testing.T) {
	shadowCompareEnv(t, "build")

	var out bytes.Buffer
	err := githubRequiredChecks(strings.NewReader(`{"required":["ci/one"]} {"required":[]}`), &out)
	if err == nil {
		t.Fatal("a second document in the same pipe was accepted")
	}
	if !strings.Contains(err.Error(), "trailing content") {
		t.Fatalf("err = %v; want the trailing content named", err)
	}
	if out.Len() != 0 {
		t.Fatalf("a refused document wrote a plan: %q", out.String())
	}
}
