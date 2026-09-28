// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"strings"
	"testing"
)

// The version segment is what tells our own older work apart from a
// stranger's. A value with nothing in that segment resembles our shape
// closely enough to be read as an earlier deployment's, and reading it that
// way would file somebody else's run under our own history as work we once
// did. It is foreign.

// A digest of the right shape, used wherever the digest itself is not the
// thing under test.
const wellFormedExternalIDDigest = "0123456789abcdef0123456789abcdef"

// An identifier carrying our prefix and no version at all is foreign, not an
// earlier version of ours.
func TestAnIdentifierWithNothingInItsVersionSegmentIsForeign(t *testing.T) {
	published := PublishedCheckRun{
		Name:       "ra8ci / build",
		ExternalID: externalIDPrefix + "-" + wellFormedExternalIDDigest,
	}
	standing, err := ExternalIDStandingOf(published, strings.Repeat("a", 40))
	if err != nil {
		t.Fatalf("an empty version segment answered %v", err)
	}
	if standing != ExternalIDForeign {
		t.Fatalf("an empty version segment is %s, want foreign", standing)
	}
	if standing.Ours() {
		t.Fatal("an empty version segment claimed to be ours")
	}

	// The same value with a version in it is our shape at a version this
	// build does not derive, which is the answer the empty one must not
	// borrow.
	versioned := PublishedCheckRun{
		Name:       published.Name,
		ExternalID: externalIDPrefix + "0-" + wellFormedExternalIDDigest,
	}
	standing, err = ExternalIDStandingOf(versioned, strings.Repeat("a", 40))
	if err != nil {
		t.Fatalf("an earlier version answered %v", err)
	}
	if standing != ExternalIDSuperseded {
		t.Fatalf("an earlier version is %s, want superseded", standing)
	}
}

// The version segment is read off the last hyphen, so every shape around that
// hyphen is judged on its own rather than on how much of our prefix it
// carries.
func TestTheVersionSegmentIsReadOffTheLastHyphen(t *testing.T) {
	for _, shape := range []struct {
		name       string
		identifier string
		want       ExternalIDStanding
	}{
		{"our prefix with the digest straight after it", externalIDPrefix + wellFormedExternalIDDigest, ExternalIDForeign},
		{"our prefix and an empty version", externalIDPrefix + "-" + wellFormedExternalIDDigest, ExternalIDForeign},
		{"a version of hyphens alone", externalIDPrefix + "--" + wellFormedExternalIDDigest, ExternalIDSuperseded},
		{"a version carrying its own hyphens", externalIDPrefix + "1-2-" + wellFormedExternalIDDigest, ExternalIDSuperseded},
		{"our current version", externalIDVersion + "-" + wellFormedExternalIDDigest, ExternalIDOtherSubject},
		{"a digest a character short", externalIDVersion + "-" + wellFormedExternalIDDigest[:31], ExternalIDForeign},
		{"a digest a character long", externalIDVersion + "-" + wellFormedExternalIDDigest + "0", ExternalIDForeign},
		{"a digest shouted in upper case", externalIDVersion + "-" + strings.ToUpper(wellFormedExternalIDDigest), ExternalIDForeign},
		{"a digest that is not hexadecimal", externalIDVersion + "-" + strings.Repeat("g", 32), ExternalIDForeign},
		{"nothing after the last hyphen", externalIDVersion + "-" + wellFormedExternalIDDigest + "-", ExternalIDForeign},
		{"somebody else's shape entirely", "gh-actions-" + wellFormedExternalIDDigest, ExternalIDForeign},
	} {
		standing, err := ExternalIDStandingOf(PublishedCheckRun{
			Name: "ra8ci / build", ExternalID: shape.identifier,
		}, strings.Repeat("b", 40))
		if err != nil {
			t.Errorf("%s answered %v", shape.name, err)
			continue
		}
		if standing != shape.want {
			t.Errorf("%s is %s, want %s", shape.name, standing, shape.want)
		}
	}
}
