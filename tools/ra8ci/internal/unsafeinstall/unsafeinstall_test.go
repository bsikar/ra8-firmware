// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package unsafeinstall

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

func TestScanTextReportsEveryLine(t *testing.T) {
	text := "safe\n" + forbidden + "\n" + forbidden
	got := scanText(text)
	if len(got) != 2 || got[0] != 2 || got[1] != 3 {
		t.Fatalf("scanText() = %v, want [2 3]", got)
	}
}
