package hilpolicy

import (
	"errors"
	"os"
	"path/filepath"
	"testing"
)

// hilBase builds the approved root DeclaredTimeout reads under and returns
// both the checkout root and the hil directory inside it.
func hilBase(t *testing.T) (root, base string) {
	t.Helper()
	root = t.TempDir()
	base = filepath.Join(root, "examples", "ek_ra8d2", "hw_validated", "hil")
	if err := os.MkdirAll(base, 0700); err != nil {
		t.Fatal(err)
	}
	return root, base
}

func writeConfig(t *testing.T, directory, body string) {
	t.Helper()
	if err := os.MkdirAll(directory, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(directory, "hil.conf"), []byte(body), 0600); err != nil {
		t.Fatal(err)
	}
}

func TestAnAppThatDeclaresNothingIsStillAbsent(t *testing.T) {
	root, base := hilBase(t)
	if err := os.MkdirAll(filepath.Join(base, "uart_hello"), 0700); err != nil {
		t.Fatal(err)
	}
	seconds, found, err := DeclaredTimeout(root, "uart_hello")
	if err != nil || found || seconds != 0 {
		t.Fatalf("seconds=%d found=%v err=%v", seconds, found, err)
	}
}

func TestAnAppWithNoDirectoryAtAllIsStillAbsent(t *testing.T) {
	root, _ := hilBase(t)
	seconds, found, err := DeclaredTimeout(root, "never_built")
	if err != nil || found || seconds != 0 {
		t.Fatalf("seconds=%d found=%v err=%v", seconds, found, err)
	}
}

func TestADanglingConfigLinkIsRefusedRatherThanReadAsAbsent(t *testing.T) {
	root, base := hilBase(t)
	app := filepath.Join(base, "uart_hello")
	if err := os.MkdirAll(app, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(filepath.Join(app, "hil.conf.real"), filepath.Join(app, "hil.conf")); err != nil {
		t.Skipf("symlinks unavailable: %v", err)
	}
	_, found, err := DeclaredTimeout(root, "uart_hello")
	if !errors.Is(err, ErrUnresolvableConfig) || found {
		t.Fatalf("found=%v err=%v", found, err)
	}
}

func TestADanglingAppDirectoryLinkIsRefusedRatherThanReadAsAbsent(t *testing.T) {
	root, base := hilBase(t)
	if err := os.Symlink(filepath.Join(base, "gone"), filepath.Join(base, "uart_hello")); err != nil {
		t.Skipf("symlinks unavailable: %v", err)
	}
	_, found, err := DeclaredTimeout(root, "uart_hello")
	if !errors.Is(err, ErrUnresolvableConfig) || found {
		t.Fatalf("found=%v err=%v", found, err)
	}
}

func TestAnAppDirectoryLinkThatResolvesLeavesAMissingConfigAbsent(t *testing.T) {
	root, base := hilBase(t)
	real := filepath.Join(base, "uart_hello_v2")
	if err := os.MkdirAll(real, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(real, filepath.Join(base, "uart_hello")); err != nil {
		t.Skipf("symlinks unavailable: %v", err)
	}
	seconds, found, err := DeclaredTimeout(root, "uart_hello")
	if err != nil || found || seconds != 0 {
		t.Fatalf("seconds=%d found=%v err=%v", seconds, found, err)
	}
}

func TestADeclarationBehindResolvableLinksIsStillRead(t *testing.T) {
	root, base := hilBase(t)
	real := filepath.Join(base, "uart_hello_v2")
	writeConfig(t, real, "HIL_TIMEOUT_S=90\n")
	if err := os.Symlink(real, filepath.Join(base, "uart_hello")); err != nil {
		t.Skipf("symlinks unavailable: %v", err)
	}
	seconds, found, err := DeclaredTimeout(root, "uart_hello")
	if err != nil || !found || seconds != 90 {
		t.Fatalf("seconds=%d found=%v err=%v", seconds, found, err)
	}
}

func TestAnOrdinaryDeclarationIsUnaffected(t *testing.T) {
	root, base := hilBase(t)
	writeConfig(t, filepath.Join(base, "uart_hello"), "# comment\nHIL_MODE=uart_scrape\nHIL_TIMEOUT_S=45\n")
	seconds, found, err := DeclaredTimeout(root, "uart_hello")
	if err != nil || !found || seconds != 45 {
		t.Fatalf("seconds=%d found=%v err=%v", seconds, found, err)
	}
}

func TestAnExistingNameIsNotAnAbsence(t *testing.T) {
	_, base := hilBase(t)
	app := filepath.Join(base, "uart_hello")
	writeConfig(t, app, "HIL_TIMEOUT_S=45\n")
	absent, err := configIsAbsent(app, filepath.Join(app, "hil.conf"))
	if err != nil || absent {
		t.Fatalf("absent=%v err=%v", absent, err)
	}
	absent, err = configIsAbsent(app, filepath.Join(app, "nothing.conf"))
	if err != nil || !absent {
		t.Fatalf("absent=%v err=%v", absent, err)
	}
}
