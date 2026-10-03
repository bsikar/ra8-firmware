// SPDX-License-Identifier: MIT
package hilpolicy

import (
	"bufio"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The reader's ordinary answers are held elsewhere. What is held here is the
// set of refusals it owes an operator when a declaration is present but this
// process cannot read it: an approved root that does not resolve, an app
// directory it may not enter, a configuration it may not open, and a line
// longer than the scanner will carry. Each of those is a declaration whose
// value is unknown, and the cost of reading any of them as "this app declares
// nothing" is a bench run cut off at the 30s default while the app's own
// configuration asked for longer.

func TestAnApprovedRootThatDoesNotResolveIsRefusedRatherThanReadAsAbsent(t *testing.T) {
	seconds, found, err := DeclaredTimeout(t.TempDir(), "blink")
	if err == nil {
		t.Fatal("a checkout with no HIL tree at all was read as an app declaring nothing")
	}
	if !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("refusal should name the absent root: %v", err)
	}
	if seconds != 0 || found {
		t.Fatalf("refused read handed back seconds=%d found=%v", seconds, found)
	}
}

func TestALineLongerThanTheReaderCarriesIsRefusedNotPassedOver(t *testing.T) {
	root, base := hilBase(t)
	// One line past the scanner's token limit, ahead of an ordinary
	// declaration. A scan that gave up quietly would report the file as
	// declaring nothing while the declaration below is still in it.
	writeConfig(t, filepath.Join(base, "blink"),
		"# "+strings.Repeat("x", bufio.MaxScanTokenSize+1)+"\nHIL_TIMEOUT_S=45\n")

	seconds, found, err := DeclaredTimeout(root, "blink")
	if err == nil {
		t.Fatal("a line the scanner could not carry was read as an app declaring nothing")
	}
	if !errors.Is(err, bufio.ErrTooLong) {
		t.Fatalf("refusal should carry the scanner's own reason: %v", err)
	}
	if seconds != 0 || found {
		t.Fatalf("refused read handed back seconds=%d found=%v", seconds, found)
	}
}
