// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package legacymake

import (
	"bytes"
	"context"
	"testing"
)

func TestSelfTest(t *testing.T) {
	var stdout, stderr bytes.Buffer
	if code := Run(context.Background(), t.TempDir(), []string{"--selftest"}, &stdout, &stderr); code != 0 {
		t.Fatalf("Run selftest = %d, stdout=%q stderr=%q", code, stdout.String(), stderr.String())
	}
}

func TestInvocationDistinguishesCommandsFromMentions(t *testing.T) {
	tests := []struct {
		line   string
		active bool
		want   bool
	}{
		{"make -C apps/blink build", true, true},
		{"cmd=(gmake ci)", true, true},
		{"run: make ci", true, true},
		{"RUN make ci", true, true},
		{"# make ci", true, true},
		{"Please run make ci", false, true},
		{"command -v make || missing=build-essential", true, false},
		{"for tool in curl cmake make tar; do", true, false},
		{"these controls make an empty scan fail", false, false},
		{"# make the detector fail", false, false},
	}
	for _, test := range tests {
		got := invocation(test.line, test.active) != ""
		if got != test.want {
			t.Errorf("invocation(%q, %t) = %t, want %t", test.line, test.active, got, test.want)
		}
	}
}
