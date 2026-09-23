// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"net/url"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

func TestParseEventQuery(t *testing.T) {
	tests := []struct {
		name  string
		query url.Values
		after int64
		limit int
		valid bool
	}{
		{name: "defaults", query: url.Values{}, limit: store.MaxEventPageSize, valid: true},
		{name: "bounded page", query: url.Values{"after": {"7"}, "limit": {"2"}}, after: 7, limit: 2, valid: true},
		{name: "first page size one", query: url.Values{"limit": {"1"}}, limit: 1, valid: true},
		{name: "negative cursor", query: url.Values{"after": {"-1"}}},
		{name: "cursor overflow", query: url.Values{"after": {"9223372036854775808"}}},
		{name: "zero limit", query: url.Values{"limit": {"0"}}},
		{name: "oversized limit", query: url.Values{"limit": {"51"}}},
		{name: "duplicate cursor", query: url.Values{"after": {"1", "2"}}},
		{name: "unknown parameter", query: url.Values{"run": {"other"}}},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			after, limit, valid := parseEventQuery(test.query)
			if valid != test.valid || (valid && (after != test.after || limit != test.limit)) {
				t.Fatalf("parseEventQuery(%v) = (%d, %d, %t), want (%d, %d, %t)", test.query, after, limit, valid, test.after, test.limit, test.valid)
			}
		})
	}
}
