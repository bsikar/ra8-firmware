// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package hilpolicy

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestAFlatFileStaysAtDepthZero(t *testing.T) {
	for _, line := range []string{
		"HIL_TIMEOUT_S=180",
		`HIL_EXPECT="boot ok"`,
		"HIL_MODE=uart_scrape",
		"",
	} {
		if depth := blockDepthAfter(line, 0); depth != 0 {
			t.Fatalf("blockDepthAfter(%q, 0) = %d, want 0", line, depth)
		}
	}
}

func TestEachBlockWordMovesTheDepth(t *testing.T) {
	for _, run := range []struct {
		open  string
		close string
	}{
		{`if [ "$HIL_MODE" = uart_scrape ]; then`, "fi"},
		{"for app in a b c; do", "done"},
		{"while read -r line; do", "done"},
		{"until boot_ok; do", "done"},
		{`case "$HIL_MODE" in`, "esac"},
		{"hil_defaults() {", "}"},
	} {
		opened := blockDepthAfter(run.open, 0)
		if opened != 1 {
			t.Fatalf("blockDepthAfter(%q, 0) = %d, want 1", run.open, opened)
		}
		if closed := blockDepthAfter(run.close, opened); closed != 0 {
			t.Fatalf("blockDepthAfter(%q, 1) = %d, want 0", run.close, closed)
		}
	}
}

func TestABlockWordInsideAValueIsNotStructure(t *testing.T) {
	for _, line := range []string{
		`HIL_EXPECT="if the board boots, done"`,
		`HIL_EXPECT='case closed'`,
		"HIL_TIMEOUT_S=180 # if it needs longer, done",
	} {
		if depth := blockDepthAfter(line, 0); depth != 0 {
			t.Fatalf("blockDepthAfter(%q, 0) = %d, want 0", line, depth)
		}
	}
}

func TestABlockWordOutOfCommandPositionIsNotStructure(t *testing.T) {
	for _, line := range []string{
		"echo done",
		"printf fi",
		"HIL_EMU_ARGS=--device esac",
	} {
		if depth := blockDepthAfter(line, 0); depth != 0 {
			t.Fatalf("blockDepthAfter(%q, 0) = %d, want 0", line, depth)
		}
	}
}

func TestABlockOpenedAndClosedOnOneLineNets(t *testing.T) {
	line := `if [ -n "$HIL_MODE" ]; then :; fi`
	if depth := blockDepthAfter(line, 0); depth != 0 {
		t.Fatalf("blockDepthAfter(%q, 0) = %d, want 0", line, depth)
	}
}

func TestAWordAfterTheBodyMarkerIsStillCommandPosition(t *testing.T) {
	line := "for app in a b; do if boot_ok; then"
	if depth := blockDepthAfter(line, 0); depth != 2 {
		t.Fatalf("blockDepthAfter(%q, 0) = %d, want 2", line, depth)
	}
}

func TestACloserWithNothingOpenIsUnaccountable(t *testing.T) {
	depth := blockDepthAfter("fi", 0)
	if depth != -1 {
		t.Fatalf("blockDepthAfter(fi, 0) = %d, want -1", depth)
	}
	if next := blockDepthAfter("HIL_TIMEOUT_S=180", depth); next != -1 {
		t.Fatalf("an unaccountable depth recovered: %d", next)
	}
}

// writeBlockConfig writes one hil.conf carrying the given lines and returns
// the root DeclaredTimeout reads from.
func writeBlockConfig(t *testing.T, lines ...string) string {
	t.Helper()
	root := t.TempDir()
	directory := filepath.Join(root, "examples", "ek_ra8d2", "hw_validated", "hil", "blocked")
	if err := os.MkdirAll(directory, 0o755); err != nil {
		t.Fatalf("make config directory: %v", err)
	}
	body := strings.Join(lines, "\n") + "\n"
	if err := os.WriteFile(filepath.Join(directory, "hil.conf"), []byte(body), 0o644); err != nil {
		t.Fatalf("write hil.conf: %v", err)
	}
	return root
}

func TestDeclaredTimeoutRefusesADeclarationInsideABlock(t *testing.T) {
	for _, config := range [][]string{
		{`if [ "$HIL_MODE" = uart_scrape ]; then`, "HIL_TIMEOUT_S=180", "fi"},
		{"for probe in a b; do", "  HIL_TIMEOUT_S=180", "done"},
		{"hil_defaults() {", "  HIL_TIMEOUT_S=180", "}"},
		{`case "$HIL_MODE" in`, "  HIL_TIMEOUT_S=180", "esac"},
	} {
		root := writeBlockConfig(t, config...)
		_, found, err := DeclaredTimeout(root, "blocked")
		if found {
			t.Fatalf("DeclaredTimeout(%q) reported a declaration", config)
		}
		if !errors.Is(err, ErrUnreadableDeclaration) {
			t.Fatalf("DeclaredTimeout(%q) error = %v, want ErrUnreadableDeclaration", config, err)
		}
	}
}

func TestDeclaredTimeoutRefusesABareDeclarationInsideABlock(t *testing.T) {
	root := writeBlockConfig(t, "if boot_ok; then", "  unset HIL_TIMEOUT_S", "fi")
	_, found, err := DeclaredTimeout(root, "blocked")
	if found || !errors.Is(err, ErrUnreadableDeclaration) {
		t.Fatalf("DeclaredTimeout = %v, %v", found, err)
	}
}

func TestDeclaredTimeoutRefusesAFileLeftOpen(t *testing.T) {
	root := writeBlockConfig(t, "HIL_TIMEOUT_S=180", "if boot_ok; then", "  HIL_MODE=uart_scrape")
	_, found, err := DeclaredTimeout(root, "blocked")
	if found {
		t.Fatalf("DeclaredTimeout reported a declaration from a file left open")
	}
	if !errors.Is(err, ErrUnreadableDeclaration) {
		t.Fatalf("DeclaredTimeout error = %v, want ErrUnreadableDeclaration", err)
	}
}

func TestDeclaredTimeoutReadsADeclarationBesideAClosedBlock(t *testing.T) {
	root := writeBlockConfig(t,
		"if boot_ok; then",
		"  HIL_MODE=uart_scrape",
		"fi",
		"HIL_TIMEOUT_S=180",
	)
	seconds, found, err := DeclaredTimeout(root, "blocked")
	if err != nil || !found || seconds != 180 {
		t.Fatalf("DeclaredTimeout = %d, %v, %v", seconds, found, err)
	}
}

func TestDeclaredTimeoutStillReadsAFlatFile(t *testing.T) {
	root := writeBlockConfig(t,
		"# the bench waits three minutes for this app",
		`HIL_EXPECT="if it boots, done"`,
		"HIL_TIMEOUT_S=180",
	)
	seconds, found, err := DeclaredTimeout(root, "blocked")
	if err != nil || !found || seconds != 180 {
		t.Fatalf("DeclaredTimeout = %d, %v, %v", seconds, found, err)
	}
}

// Every HIL_TIMEOUT_S line under examples/ is a plain top-level assignment
// today, so this rule is expected to change no real manifest. The check that
// it does not is the flat-file case above; this pins the shape those files
// have, so a manifest that grows structure is caught by the rule rather than
// read past.
func TestARealManifestShapeIsFlat(t *testing.T) {
	depth := 0
	for _, line := range []string{
		"# ek_ra8d2 uart scrape",
		"HIL_MODE=uart_scrape",
		`HIL_EXPECT="RA8 BOOT OK"`,
		"HIL_TIMEOUT_S=45",
		"HIL_EMU_ARGS=--device ra8p1",
	} {
		depth = blockDepthAfter(strings.TrimSpace(line), depth)
	}
	if depth != 0 {
		t.Fatalf("a real manifest shape counted as structure: depth %d", depth)
	}
}
