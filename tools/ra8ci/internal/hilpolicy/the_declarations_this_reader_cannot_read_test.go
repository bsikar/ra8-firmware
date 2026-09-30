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

// sealedDirectory returns a directory this process may not enter, restored
// before the test ends so the temporary tree can be removed.
func sealedDirectory(t *testing.T) string {
	t.Helper()
	if os.Geteuid() == 0 {
		t.Skip("running as root: a sealed directory is still searchable")
	}
	sealed := filepath.Join(t.TempDir(), "sealed")
	if err := os.Mkdir(sealed, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(sealed, 0000); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(sealed, 0700) })
	return sealed
}

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

func TestAnAppDirectoryThisReaderMayNotEnterIsRefused(t *testing.T) {
	sealedDirectory(t)
	root, base := hilBase(t)
	app := filepath.Join(base, "blink")
	writeConfig(t, app, "HIL_TIMEOUT_S=45\n")
	if os.Geteuid() == 0 {
		t.Skip("running as root: a sealed directory is still searchable")
	}
	if err := os.Chmod(app, 0000); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(app, 0700) })

	seconds, found, err := DeclaredTimeout(root, "blink")
	if err == nil {
		t.Fatal("an app directory this reader cannot enter was read as an app declaring nothing")
	}
	if errors.Is(err, os.ErrNotExist) {
		t.Fatalf("a directory that exists must not be refused as missing: %v", err)
	}
	if seconds != 0 || found {
		t.Fatalf("refused read handed back seconds=%d found=%v", seconds, found)
	}
}

func TestADeclarationThisReaderMayNotOpenIsRefused(t *testing.T) {
	if os.Geteuid() == 0 {
		t.Skip("running as root: an unreadable file is still readable")
	}
	root, base := hilBase(t)
	app := filepath.Join(base, "blink")
	writeConfig(t, app, "HIL_TIMEOUT_S=45\n")
	conf := filepath.Join(app, "hil.conf")
	if err := os.Chmod(conf, 0000); err != nil {
		t.Fatal(err)
	}

	// The name resolves: only the content is out of reach, which is exactly
	// the case that would otherwise be reported as an absent declaration.
	if _, found, err := DeclaredTimeout(root, "blink"); err == nil || found {
		t.Fatalf("an unopenable configuration was read as found=%v err=%v", found, err)
	}

	if err := os.Chmod(conf, 0600); err != nil {
		t.Fatal(err)
	}
	seconds, found, err := DeclaredTimeout(root, "blink")
	if err != nil || !found || seconds != 45 {
		t.Fatalf("the same file once readable: seconds=%d found=%v err=%v", seconds, found, err)
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

func TestAnAbsenceThisReaderCannotEstablishIsAnError(t *testing.T) {
	sealed := sealedDirectory(t)

	// The configuration's own name cannot be inspected.
	absent, err := configIsAbsent(sealed, filepath.Join(sealed, "hil.conf"))
	if err == nil {
		t.Fatal("a name that could not be inspected was answered as an absence")
	}
	if absent {
		t.Fatal("a failed inspection must not be reported as absent")
	}

	// The file is plainly missing, and the app directory holding it is the
	// name that cannot be inspected. The directory is asked second, so this
	// is the other half of the same refusal.
	missing := filepath.Join(t.TempDir(), "blink", "hil.conf")
	absent, err = configIsAbsent(filepath.Join(sealed, "blink"), missing)
	if err == nil {
		t.Fatal("an app directory that could not be inspected was answered as an absence")
	}
	if absent {
		t.Fatal("a failed inspection must not be reported as absent")
	}
}

func TestAnUninspectableNameIsRefusedAndNamed(t *testing.T) {
	sealed := sealedDirectory(t)
	name := filepath.Join(sealed, "hil.conf")

	there, err := nameExists(name)
	if err == nil {
		t.Fatal("a name this process may not inspect was answered as an ordinary absence")
	}
	if there {
		t.Fatal("a failed inspection must not report the name as present")
	}
	if !strings.Contains(err.Error(), name) {
		t.Fatalf("refusal should name the path it could not inspect: %v", err)
	}
}
