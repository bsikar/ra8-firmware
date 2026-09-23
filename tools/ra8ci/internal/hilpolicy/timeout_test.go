package hilpolicy

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestChooseFallbackAndDynamicBound(t *testing.T) {
	decision, err := Choose(0, false, nil)
	if err != nil || decision.Seconds != 30 || decision.Source != "default" {
		t.Fatalf("default=%+v err=%v", decision, err)
	}
	decision, err = Choose(240, true, []Observation{{Duration: time.Second}})
	if err != nil || decision.Seconds != 240 || decision.Source != "hil.conf" {
		t.Fatalf("declared=%+v err=%v", decision, err)
	}
	samples := []Observation{{10 * time.Second, false}, {12 * time.Second, false}, {14 * time.Second, false}, {20 * time.Second, false}, {24 * time.Second, false}}
	decision, err = Choose(0, false, samples)
	if err != nil || decision.Source != "observed" || decision.Seconds != 32 || decision.Samples != 5 {
		t.Fatalf("dynamic=%+v err=%v", decision, err)
	}
	samples[4].TimedOut = true
	decision, err = Choose(0, false, samples)
	if err != nil || decision.Seconds < 36 || decision.Censored != 1 {
		t.Fatalf("censored=%+v err=%v", decision, err)
	}
	for i := range samples {
		samples[i] = Observation{Duration: 3600 * time.Second}
	}
	decision, err = Choose(0, false, samples)
	if err != nil || decision.Seconds != MaximumSeconds {
		t.Fatalf("cap=%+v err=%v", decision, err)
	}
}

func TestChooseRejectsInvalidEvidence(t *testing.T) {
	if _, err := Choose(0, true, nil); err == nil {
		t.Fatal("invalid declared timeout accepted")
	}
	if _, err := Choose(0, false, []Observation{{Duration: 0}}); err == nil {
		t.Fatal("zero observation accepted")
	}
	if _, err := Choose(0, false, []Observation{{Duration: 3601 * time.Second}}); err == nil {
		t.Fatal("unbounded observation accepted")
	}
}

func TestDeclaredTimeoutParsesExistingConfigurationWithoutShellEvaluation(t *testing.T) {
	root := t.TempDir()
	base := filepath.Join(root, "examples", "ek_ra8d2", "hw_validated", "hil")
	app := filepath.Join(base, "uart_hello")
	if err := os.MkdirAll(app, 0700); err != nil {
		t.Fatal(err)
	}
	config := filepath.Join(app, "hil.conf")
	if err := os.WriteFile(config, []byte("# comment\nHIL_MODE=uart_scrape\nHIL_TIMEOUT_S=10\n"), 0600); err != nil {
		t.Fatal(err)
	}
	seconds, found, err := DeclaredTimeout(root, "uart_hello")
	if err != nil || !found || seconds != 10 {
		t.Fatalf("seconds=%d found=%v err=%v", seconds, found, err)
	}
	if _, _, err := DeclaredTimeout(root, "../escape"); err == nil {
		t.Fatal("path traversal accepted")
	}
	if _, found, err := DeclaredTimeout(root, "absent"); err != nil || found {
		t.Fatalf("absent found=%v err=%v", found, err)
	}
	if err := os.WriteFile(config, []byte("HIL_TIMEOUT_S=10\nHIL_TIMEOUT_S=20\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, _, err := DeclaredTimeout(root, "uart_hello"); err == nil {
		t.Fatal("duplicate setting accepted")
	}
	if err := os.WriteFile(config, []byte("HIL_TIMEOUT_S=$(evil)\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, _, err := DeclaredTimeout(root, "uart_hello"); err == nil {
		t.Fatal("shell expression accepted")
	}
	if err := os.WriteFile(config, []byte("HIL_TIMEOUT_S=3601\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, _, err := DeclaredTimeout(root, "uart_hello"); err == nil {
		t.Fatal("oversized setting accepted")
	}
	outside := filepath.Join(t.TempDir(), "hil.conf")
	if err := os.WriteFile(outside, []byte("HIL_TIMEOUT_S=10\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.Remove(config); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(outside, config); err == nil {
		if _, _, err := DeclaredTimeout(root, "uart_hello"); err == nil || !strings.Contains(err.Error(), "escapes") {
			t.Fatalf("symlink escape err=%v", err)
		}
	}
}
