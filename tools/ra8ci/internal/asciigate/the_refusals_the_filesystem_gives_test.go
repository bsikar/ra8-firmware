// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package asciigate

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The gate's own refusals are covered next door. These are the ones the
// filesystem hands it: a checkout it cannot open, a directory it cannot walk
// into, a file it can see but not read, a file it can read but not write.
// Each one has to end the run, because the alternative is a gate that reports
// nothing to fix about a file it never managed to look at, and CI reads that
// as clean.

// A root that is not a directory is refused at the door, naming the checkout
// rather than the target. The distinction matters to whoever reads CI: the
// target they passed was fine and the checkout they passed was not.
func TestACheckoutThatCannotBeOpenedIsRefused(t *testing.T) {
	home := t.TempDir()
	notADirectory := filepath.Join(home, "checkout")
	if err := os.WriteFile(notADirectory, []byte("not a checkout\n"), 0o600); err != nil {
		t.Fatalf("plant a file where a checkout should be: %v", err)
	}
	for name, root := range map[string]string{
		"a file standing in for the checkout": notADirectory,
		"a checkout that is not there":        filepath.Join(home, "absent"),
	} {
		t.Run(name, func(t *testing.T) {
			code, out, errs := ranGate(t, root, "--checkout", "page.md")
			if code != 2 {
				t.Fatalf("code = %d, want 2 (stdout %q, stderr %q)", code, out, errs)
			}
			if !strings.Contains(errs, "open checkout:") {
				t.Fatalf("stderr = %q, want the checkout named", errs)
			}
			if out != "" {
				t.Fatalf("stdout carried %q, want nothing reported about a checkout never opened", out)
			}
		})
	}
}

// The scan opens the checkout again rather than carrying a handle from the
// scope derivation, so it refuses a checkout that has gone away between the
// two. Reached directly because a single run cannot have the checkout both
// sound and unsound.
func TestTheScanRefusesACheckoutThatCanNoLongerBeOpened(t *testing.T) {
	home := t.TempDir()
	if _, err := processCheckout(filepath.Join(home, "absent"), "page.md", false); err == nil {
		t.Fatal("a checkout that is not there was scanned")
	}
}
