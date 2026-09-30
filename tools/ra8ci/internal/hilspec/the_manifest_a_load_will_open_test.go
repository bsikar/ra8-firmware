// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package hilspec

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// Load decides which file it is even allowed to open before it reads a byte,
// and the grammar behind it refuses anything that could reach a shell. These
// are the refusals a hand-edited hil.conf actually meets.

func TestAManifestPathOutsideTheExamplesTreeIsRefusedBeforeAnyRead(t *testing.T) {
	root := t.TempDir()
	for name, relative := range map[string]string{
		"an absolute path":  "/etc/hil.conf",
		"another file name": "examples/board.conf",
		"a backslash":       "examples\\board\\hil.conf",
		"the tree itself":   ".",
		"a parent":          "..",
		"a climb out":       "../hil.conf",
		"outside examples":  "vendor/hil.conf",
		"a climb through":   "examples/../../hil.conf",
	} {
		if _, err := Load(root, relative); !errors.Is(err, ErrUnsafePath) {
			t.Fatalf("%s was not refused as unsafe: %v", name, err)
		}
	}
}

// The path rules pass, and the tree simply is not there. That is a different
// answer from an unsafe path: the caller named something legitimate that does
// not exist, and the error says so rather than claiming an escape.
func TestAnAbsentExamplesTreeAndAnAbsentManifestAreReportedAsAbsent(t *testing.T) {
	bare := t.TempDir()
	_, err := Load(bare, "examples/board/hil.conf")
	if err == nil || errors.Is(err, ErrUnsafePath) {
		t.Fatalf("an absent examples tree was reported as an escape: %v", err)
	}
	withTree := t.TempDir()
	if err := os.MkdirAll(filepath.Join(withTree, "examples", "board"), 0o755); err != nil {
		t.Fatalf("plant the examples tree: %v", err)
	}
	_, err = Load(withTree, "examples/board/hil.conf")
	if err == nil || errors.Is(err, ErrUnsafePath) {
		t.Fatalf("an absent manifest was reported as an escape: %v", err)
	}
}

// A symlink is resolved and then judged against the resolved examples tree, so
// a manifest pointing out of the tree is refused even though its own path
// never leaves it.
func TestAManifestSymlinkedOutOfTheTreeIsRefused(t *testing.T) {
	root := t.TempDir()
	board := filepath.Join(root, "examples", "board")
	if err := os.MkdirAll(board, 0o755); err != nil {
		t.Fatalf("plant the examples tree: %v", err)
	}
	outside := filepath.Join(root, "elsewhere.conf")
	if err := os.WriteFile(outside, []byte("HIL_MODE=alive\n"), 0o644); err != nil {
		t.Fatalf("plant the outside manifest: %v", err)
	}
	if err := os.Symlink(outside, filepath.Join(board, "hil.conf")); err != nil {
		t.Skipf("symlinks are not available here: %v", err)
	}
	if _, err := Load(root, "examples/board/hil.conf"); !errors.Is(err, ErrUnsafePath) {
		t.Fatalf("a manifest symlinked out of the tree was not refused: %v", err)
	}
}

func plantManifest(t *testing.T, body string, mode os.FileMode) (string, string) {
	t.Helper()
	root := t.TempDir()
	board := filepath.Join(root, "examples", "board")
	if err := os.MkdirAll(board, 0o755); err != nil {
		t.Fatalf("plant the examples tree: %v", err)
	}
	if err := os.WriteFile(filepath.Join(board, "hil.conf"), []byte(body), mode); err != nil {
		t.Fatalf("plant the manifest: %v", err)
	}
	return root, "examples/board/hil.conf"
}

// A file the reader cannot open is reported as the open failure, not as an
// invalid manifest: nothing was parsed, so nothing can be called invalid.
func TestAManifestThatWillNotOpenIsRefused(t *testing.T) {
	if os.Geteuid() == 0 {
		t.Skip("running as root: a sealed file still opens")
	}
	root, relative := plantManifest(t, "HIL_MODE=alive\n", 0o000)
	if _, err := Load(root, relative); err == nil {
		t.Fatal("a sealed manifest was read")
	}
}

// A directory sitting where hil.conf belongs passes every path rule and opens,
// so the regular-file check is the only thing standing between it and the
// parser.
func TestADirectoryNamedHilConfIsRefusedAsAnInvalidManifest(t *testing.T) {
	root := t.TempDir()
	if err := os.MkdirAll(filepath.Join(root, "examples", "board", "hil.conf"), 0o755); err != nil {
		t.Fatalf("plant a directory named hil.conf: %v", err)
	}
	if _, err := Load(root, "examples/board/hil.conf"); !errors.Is(err, ErrInvalidManifest) {
		t.Fatalf("a directory named hil.conf was not refused: %v", err)
	}
}

func TestAManifestPastTheSizeBoundIsRefused(t *testing.T) {
	body := "# " + strings.Repeat("padding ", (maxManifestBytes/8)+16) + "\nHIL_MODE=alive\n"
	if len(body) <= maxManifestBytes {
		t.Fatalf("the fixture is %d bytes, inside the %d bound", len(body), maxManifestBytes)
	}
	root, relative := plantManifest(t, body, 0o644)
	if _, err := Load(root, relative); !errors.Is(err, ErrInvalidManifest) {
		t.Fatalf("an oversized manifest was not refused: %v", err)
	}
	if _, err := Parse(strings.NewReader(body), "examples/board/hil.conf"); !errors.Is(err, ErrInvalidManifest) {
		t.Fatalf("the parser accepted an oversized manifest: %v", err)
	}
}

// A manifest that survives every path rule reaches the parser, which is the
// pair the refusals above are only meaningful against.
func TestAManifestInsideTheTreeIsRead(t *testing.T) {
	root, relative := plantManifest(t, "# a board\nHIL_MODE=alive\n", 0o644)
	spec, err := Load(root, relative)
	if err != nil {
		t.Fatalf("a sound manifest was refused: %v", err)
	}
	if spec.Mode != ModeAlive {
		t.Fatalf("the mode read as %q", spec.Mode)
	}
	if spec.Path != relative {
		t.Fatalf("the spec names %q rather than the relative path", spec.Path)
	}
}

// validKey is consulted only for a key the schema already knows, so its
// refusals are a second line rather than the first: they hold the table itself
// to the HIL_ naming rule.
func TestAKeyIsValidOnlyWhenItIsShoutedAndPrefixed(t *testing.T) {
	if !validKey("HIL_MODE") || !validKey("HIL_PROBE_BOOT_S") || !validKey("HIL_9") {
		t.Fatal("a shouted HIL_ key was refused")
	}
	for name, key := range map[string]string{
		"no prefix":     "MODE",
		"a near prefix": "HILMODE",
		"lowercase":     "HIL_mode",
		"a dash":        "HIL-MODE",
		"a dot":         "HIL.MODE",
		"a space":       "HIL MODE",
		"a non-ASCII":   "HIL_MODÉ",
	} {
		if validKey(key) {
			t.Fatalf("%s was accepted as a key", name)
		}
	}
	if !validKey("HIL_") {
		t.Log("the bare prefix is accepted; the schema is what keeps it out")
	}
}

// The literal grammar is deliberately not Bash. Anything that could expand,
// substitute, or continue a command is refused rather than quoted away.
func TestALiteralThatCouldReachAShellIsRefused(t *testing.T) {
	for name, raw := range map[string]string{
		"empty":                  "",
		"an expansion":           "$HOME",
		"a substitution":         "`id`",
		"a carriage return":      "one\rtwo",
		"a newline":              "one\ntwo",
		"a NUL":                  "one\x00two",
		"an unterminated quote":  `"open`,
		"a lone quote":           `"`,
		"an unescaped quote":     `"a"b"`,
		"unquoted whitespace":    "two words",
		"an unquoted semicolon":  "alive;id",
		"an unquoted pipe":       "alive|id",
		"an unquoted subshell":   "alive(id)",
		"an unquoted redirect":   "alive>out",
		"an unquoted brace":      "alive{id}",
		"an unquoted apostrophe": "it's",
	} {
		if value, err := parseLiteral(raw); err == nil {
			t.Fatalf("%s was accepted as the literal %q", name, value)
		}
	}
	for raw, want := range map[string]string{
		`alive`:       "alive",
		`"two words"`: "two words",
		`"a\"b"`:      `a\"b`,
		`""`:          "",
		`10`:          "10",
	} {
		value, err := parseLiteral(raw)
		if err != nil {
			t.Fatalf("the literal %s was refused: %v", raw, err)
		}
		if value != want {
			t.Fatalf("the literal %s read as %q, wanted %q", raw, value, want)
		}
	}
}

// A probe symbol becomes a linker query, so it is held to C identifier shape
// rather than to the literal grammar alone.
func TestAProbeSymbolIsHeldToIdentifierShape(t *testing.T) {
	for name, symbol := range map[string]string{
		"empty":            "",
		"a leading digit":  "9probe",
		"a leading dash":   "-probe",
		"a dot":            "probe.count",
		"a dash inside":    "probe-count",
		"a non-ASCII rune": "probé",
	} {
		if validSymbol(symbol) {
			t.Fatalf("%s was accepted as a probe symbol", name)
		}
		err := validateTypedValue("HIL_PROBE_SYMBOL", Value{Kind: TextValue, Text: symbol})
		if err == nil {
			t.Fatalf("%s reached the board as a probe symbol", name)
		}
		if !strings.Contains(err.Error(), "probe symbol") {
			t.Fatalf("the refusal of %s does not name the symbol: %v", name, err)
		}
	}
	for _, symbol := range []string{"probe", "_probe", "Probe9", "g_ra8_probe_count"} {
		if !validSymbol(symbol) {
			t.Fatalf("the identifier %q was refused as a probe symbol", symbol)
		}
		if err := validateTypedValue("HIL_RTT_BUF_SYMBOL", Value{Kind: TextValue, Text: symbol}); err != nil {
			t.Fatalf("the identifier %q was refused for the RTT buffer: %v", symbol, err)
		}
	}
}
