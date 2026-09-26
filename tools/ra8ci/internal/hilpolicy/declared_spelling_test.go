// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package hilpolicy

import (
	"errors"
	"path/filepath"
	"testing"
)

// readDeclaration writes one hil.conf body and asks DeclaredTimeout about it.
func readDeclaration(t *testing.T, body string) (int, bool, error) {
	t.Helper()
	root, base := hilBase(t)
	writeConfig(t, filepath.Join(base, "uart_hello"), body)
	return DeclaredTimeout(root, "uart_hello")
}

func TestAnUnreadSpellingOfTheTimeoutIsRefusedNotSkipped(t *testing.T) {
	for _, body := range []string{
		"export HIL_TIMEOUT_S=180\n",
		"HIL_TIMEOUT_S+=60\n",
		"declare -i HIL_TIMEOUT_S=180\n",
		"readonly HIL_TIMEOUT_S=180\n",
		"HIL_MODE=uart_scrape\nexport HIL_TIMEOUT_S=180\n",
	} {
		t.Run(body, func(t *testing.T) {
			seconds, found, err := readDeclaration(t, body)
			if !errors.Is(err, ErrUnreadableDeclaration) {
				t.Fatalf("err = %v, want ErrUnreadableDeclaration", err)
			}
			if found || seconds != 0 {
				t.Fatalf("seconds=%d found=%v; an unreadable declaration is not an absence", seconds, found)
			}
		})
	}
}

func TestADifferentVariableIsStillSkippedInSilence(t *testing.T) {
	for _, body := range []string{
		"RA8_HIL_TIMEOUT_S=180\n",
		"HIL_TIMEOUT_SECONDS=180\n",
		"HIL_TIMEOUT_SEC=180\n",
		"MY_HIL_TIMEOUT_SX=180\n",
		"HIL_MODE=uart_scrape\n",
	} {
		t.Run(body, func(t *testing.T) {
			seconds, found, err := readDeclaration(t, body)
			if err != nil || found || seconds != 0 {
				t.Fatalf("seconds=%d found=%v err=%v; another variable is not this one", seconds, found, err)
			}
		})
	}
}

func TestTheOrdinaryDeclarationStillReadsTheSame(t *testing.T) {
	for _, body := range []string{
		"HIL_TIMEOUT_S=90\n",
		"  HIL_TIMEOUT_S =90\n",
		"# export HIL_TIMEOUT_S=180\nHIL_TIMEOUT_S=90\n",
	} {
		t.Run(body, func(t *testing.T) {
			seconds, found, err := readDeclaration(t, body)
			if err != nil || !found || seconds != 90 {
				t.Fatalf("seconds=%d found=%v err=%v", seconds, found, err)
			}
		})
	}
}

// A comment is dropped before the key is looked at, so a commented-out shell
// spelling must not be mistaken for a declaration the reader cannot read.
func TestACommentedOutSpellingIsNotADeclaration(t *testing.T) {
	seconds, found, err := readDeclaration(t, "# export HIL_TIMEOUT_S=180\n#HIL_TIMEOUT_S+=5\n")
	if err != nil || found || seconds != 0 {
		t.Fatalf("seconds=%d found=%v err=%v", seconds, found, err)
	}
}

func TestKeyNamesTheTimeoutStandsTheTokenAlone(t *testing.T) {
	tests := []struct {
		key  string
		want bool
	}{
		{"HIL_TIMEOUT_S", true},
		{"export HIL_TIMEOUT_S", true},
		{"declare -i HIL_TIMEOUT_S", true},
		{"HIL_TIMEOUT_S+", true},
		{"local HIL_TIMEOUT_S", true},
		{"RA8_HIL_TIMEOUT_S", false},
		{"HIL_TIMEOUT_SEC", false},
		{"HIL_TIMEOUT_S2", false},
		{"xHIL_TIMEOUT_S", false},
		{"HIL_MODE", false},
		{"", false},
		{"HIL_TIMEOUT", false},
	}
	for _, test := range tests {
		t.Run(test.key, func(t *testing.T) {
			if got := keyNamesTheTimeout(test.key); got != test.want {
				t.Fatalf("keyNamesTheTimeout(%q) = %t, want %t", test.key, got, test.want)
			}
		})
	}
}

// The two sentinels answer different questions and a caller distinguishes
// them, so neither may stand in for the other.
func TestTheTwoUnreadableAnswersAreToldApart(t *testing.T) {
	_, _, err := readDeclaration(t, "export HIL_TIMEOUT_S=180\n")
	if errors.Is(err, ErrUnresolvableConfig) {
		t.Fatalf("err = %v, want a spelling refusal rather than a resolution refusal", err)
	}
	if !errors.Is(err, ErrUnreadableDeclaration) {
		t.Fatalf("err = %v, want ErrUnreadableDeclaration", err)
	}
}

// The refusal has to name the line, because the operator's next act is to go
// and look at it.
func TestTheRefusalNamesWhatItCouldNotRead(t *testing.T) {
	_, _, err := readDeclaration(t, "export HIL_TIMEOUT_S=180\n")
	if err == nil {
		t.Fatal("want a refusal")
	}
	if want := `"export HIL_TIMEOUT_S"`; !contains(err.Error(), want) {
		t.Fatalf("error %q does not name the key it could not read", err)
	}
	if !contains(err.Error(), "hil.conf") {
		t.Fatalf("error %q does not name the file", err)
	}
}

func contains(haystack, needle string) bool {
	for index := 0; index+len(needle) <= len(haystack); index++ {
		if haystack[index:index+len(needle)] == needle {
			return true
		}
	}
	return false
}
