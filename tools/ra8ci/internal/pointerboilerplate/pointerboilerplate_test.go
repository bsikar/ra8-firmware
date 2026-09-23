// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package pointerboilerplate

import "testing"

func TestBannedCommentForms(t *testing.T) {
	if !banned.MatchString("/* See the internal header for the documented contract. */") {
		t.Fatal("accepted generated pointer-only definition comment")
	}
	for _, line := range []string{
		"/* see header for full description */",
		"const char* text = \"see header for the documented contract.\";",
	} {
		if banned.MatchString(line) {
			t.Errorf("flagged allowed text %q", line)
		}
	}
}
