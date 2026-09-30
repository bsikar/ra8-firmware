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

// sealed takes away every permission on a path for the duration of one test
// and gives them back afterwards, without which the temporary directory
// cannot be removed and the failure lands on an unrelated test.
func sealed(t *testing.T, path string) {
	t.Helper()
	info, err := os.Lstat(path)
	if err != nil {
		t.Fatalf("stat before sealing %s: %v", path, err)
	}
	if err := os.Chmod(path, 0); err != nil {
		t.Fatalf("seal %s: %v", path, err)
	}
	t.Cleanup(func() {
		if err := os.Chmod(path, info.Mode().Perm()); err != nil {
			t.Errorf("unseal %s: %v", path, err)
		}
	})
}

// readOnly leaves a path readable and takes away the write bit, which is the
// state a checked-out file has under a build that was never meant to rewrite
// it.
func readOnly(t *testing.T, path string) {
	t.Helper()
	if err := os.Chmod(path, 0o400); err != nil {
		t.Fatalf("make %s read-only: %v", path, err)
	}
	t.Cleanup(func() {
		if err := os.Chmod(path, 0o600); err != nil {
			t.Errorf("restore %s: %v", path, err)
		}
	})
}

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

// A directory on the way to the target that cannot be entered is refused. It
// stats as an ordinary directory, so the scope check passes it and only the
// open fails, which is the one path here where the two disagree.
func TestADirectoryOnTheWayToTheTargetThatCannotBeEnteredIsRefused(t *testing.T) {
	root := t.TempDir()
	section := filepath.Join(root, "section")
	if err := os.Mkdir(section, 0o700); err != nil {
		t.Fatalf("plant section: %v", err)
	}
	if err := os.WriteFile(filepath.Join(section, "page.md"), []byte("ascii\n"), 0o600); err != nil {
		t.Fatalf("plant page: %v", err)
	}
	sealed(t, section)

	code, out, errs := ranGate(t, root, "--checkout", filepath.Join("section", "page.md"))
	if code != 2 {
		t.Fatalf("code = %d, want 2 (stdout %q, stderr %q)", code, out, errs)
	}
	if !strings.Contains(errs, "FATAL") {
		t.Fatalf("stderr = %q, want the run ended", errs)
	}
	if out != "" {
		t.Fatalf("stdout carried %q, want nothing reported about a file never read", out)
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

// A file the gate can see but not read is refused rather than counted as
// having nothing to fix. This is the failure that matters most of the four:
// it is silent, and a gate that reports zero findings on an unreadable file
// tells CI the rule holds over a file nobody checked.
func TestATargetThatCannotBeReadIsRefused(t *testing.T) {
	root := t.TempDir()
	page := filepath.Join(root, "page.md")
	if err := os.WriteFile(page, []byte("ascii\n"), 0o600); err != nil {
		t.Fatalf("plant page: %v", err)
	}
	sealed(t, page)

	code, out, errs := ranGate(t, root, "--check", "--checkout", "page.md")
	if code != 2 {
		t.Fatalf("code = %d, want 2 (stdout %q, stderr %q)", code, out, errs)
	}
	if !strings.Contains(errs, "FATAL") {
		t.Fatalf("stderr = %q, want the run ended", errs)
	}
	if strings.Contains(out, "page.md") {
		t.Fatalf("stdout carried %q, want no verdict on a file never read", out)
	}
}

// A rewrite that cannot be written is refused, and it is refused in both
// modes the gate rewrites through. A count reported here without the write
// landing would tell CI the file was fixed while the characters are still in
// it, which is worse than either a clean pass or an honest failure.
func TestARewriteThatCannotBeWrittenIsRefused(t *testing.T) {
	for name, asCheckout := range map[string]bool{
		"through the checkout": true,
		"through a walk":       false,
	} {
		t.Run(name, func(t *testing.T) {
			root := t.TempDir()
			page := filepath.Join(root, "page.md")
			if err := os.WriteFile(page, []byte("an em dash \u2014 here\n"), 0o600); err != nil {
				t.Fatalf("plant page: %v", err)
			}
			readOnly(t, page)

			args := []string{page}
			if asCheckout {
				args = []string{"--checkout", "page.md"}
			}
			code, out, errs := ranGate(t, root, args...)
			if code != 2 {
				t.Fatalf("code = %d, want 2 (stdout %q, stderr %q)", code, out, errs)
			}
			if !strings.Contains(errs, "FATAL") {
				t.Fatalf("stderr = %q, want the run ended", errs)
			}
			if strings.Contains(out, "[FIXED]") {
				t.Fatalf("stdout carried %q, want no fix claimed for a write that failed", out)
			}
			raw, err := os.ReadFile(page)
			if err != nil {
				t.Fatalf("read the page back: %v", err)
			}
			if !strings.Contains(string(raw), "\u2014") {
				t.Fatalf("the page now reads %q, want it left exactly as it was", raw)
			}
		})
	}
}
