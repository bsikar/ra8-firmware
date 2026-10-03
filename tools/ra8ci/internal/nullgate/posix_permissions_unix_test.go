//go:build unix

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package nullgate

import (
	os "os"
	filepath "path/filepath"
	strings "strings"
	testing "testing"
)

// A source the gate cannot read yields no violations rather than a refusal:
// the enumeration already decided the file belongs, so an unreadable one is
// passed over the way an undecodable one is. Worth pinning because the quiet
// return is indistinguishable from a clean file at the call site.
func TestASourceTheGateCannotReadYieldsNothing(t *testing.T) {
	root := plantFiles(t, map[string]string{"libs/ra8_hal/src/ra8_gpio.c": "char *p = NULL;\n"})
	sealed := filepath.Join(root, "libs", "ra8_hal", "src", "ra8_gpio.c")
	if err := os.Chmod(sealed, 0o000); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(sealed, 0o600) })
	if _, err := os.ReadFile(sealed); err == nil {
		t.Skip("this process can read a sealed file")
	}

	if found := findViolations(sealed); found != nil {
		t.Fatalf("an unreadable source produced violations: %+v", found)
	}
	code, stdout, stderr := swept(t, root, "libs/ra8_hal/src/ra8_gpio.c")
	if code != 0 || stderr != "" || !strings.Contains(stdout, "0 findings") {
		t.Fatalf("an unreadable source answered %d, stdout=%q stderr=%q", code, stdout, stderr)
	}
}
