// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"encoding/json"
	"strings"
	"testing"
)

// A manifest that stops mid-value, or closes with the wrong bracket, is
// refused rather than canonicalized on the part that arrived. Without the
// closing read a half-written manifest would canonicalize and digest as
// though it were whole, which is the one failure a digest cannot catch
// afterwards: both sides would agree on the truncated bytes.

// A value that simply runs out is refused where it runs out, inside the
// walk over its members rather than at its close.
func TestAValueThatStopsIsRefusedWhereItStops(t *testing.T) {
	for _, raw := range []string{"[1,2", `{"a":1`, "[[1", `{"a":{`} {
		decoder := json.NewDecoder(strings.NewReader(raw))
		err := inspectJSONValue(decoder)
		if err == nil {
			t.Fatalf("%s was walked to the end", raw)
		}
		if !strings.Contains(err.Error(), "unexpected end of JSON input") {
			t.Fatalf("%s was refused, but not for running out: %v", raw, err)
		}
	}

	// The same truncation through the door a checkout comes in by, so the
	// refusal is not only reachable from inside the package.
	if _, err := CanonicalJSON([]byte(`{"schema_version":1,"tasks":[`)); err == nil {
		t.Fatal("a manifest cut mid-array was canonicalized")
	}
}

// A container whose members parse cleanly and whose close is the wrong
// bracket is refused when that close is read. This is the branch that only
// runs once the members are behind it, so a well-formed body cannot carry a
// mismatched tail past the walk.
func TestAValueClosedWithTheWrongBracketIsRefused(t *testing.T) {
	for _, raw := range []string{"[1,2}", `{"a":1]`} {
		decoder := json.NewDecoder(strings.NewReader(raw))
		err := inspectJSONValue(decoder)
		if err == nil {
			t.Fatalf("%s was accepted", raw)
		}
		if !strings.Contains(err.Error(), "invalid character") {
			t.Fatalf("%s was refused, but not for its close: %v", raw, err)
		}
	}
}
