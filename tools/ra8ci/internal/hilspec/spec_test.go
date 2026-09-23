package hilspec

import (
	"errors"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func repositoryRoot(t *testing.T) string {
	t.Helper()
	root, err := filepath.Abs(filepath.Join("..", "..", "..", ".."))
	if err != nil {
		t.Fatal(err)
	}
	return root
}

func TestParsesEveryCurrentHILManifestAsData(t *testing.T) {
	root := repositoryRoot(t)
	count := 0
	err := filepath.WalkDir(filepath.Join(root, "examples"), func(path string, entry fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if entry.IsDir() || entry.Name() != "hil.conf" {
			return nil
		}
		relative, err := filepath.Rel(root, path)
		if err != nil {
			return err
		}
		spec, err := Load(root, relative)
		if err != nil {
			t.Errorf("%s: %v", relative, err)
			return nil
		}
		if spec.Path != relative || spec.Mode == "" || len(spec.Values) == 0 {
			t.Errorf("%s: incomplete parsed spec %+v", relative, spec)
		}
		count++
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	if count < 200 {
		t.Fatalf("only %d HIL manifests parsed", count)
	}
}

func TestRepresentativeTypedMetadata(t *testing.T) {
	root := repositoryRoot(t)
	for _, tc := range []struct {
		path    string
		mode    Mode
		expect  string
		timeout int
	}{
		{"examples/ek_ra8d2/hw_validated/hil/uart_hello/hil.conf", ModeUARTScrape, "hello, ra8d2!", 10},
		{"examples/ra8p1_foundation/npu_infer/hil.conf", ModeUARTScrape, "verdict=PASS", 30},
		{"examples/ek_ra8d2/hw_validated/hil/dfu_copy_to_run/hil.conf", ModeJLinkMemprobe, "", 0},
		{"examples/ek_ra8d2/hw_validated/hil/threadx_netx_tcp_echo/hil.conf", ModeEthernetTCP, "[netx] echoed 23 bytes from 192.168.1.1", 0},
	} {
		spec, err := Load(root, tc.path)
		if err != nil {
			t.Fatal(err)
		}
		if spec.Mode != tc.mode || spec.Expect != tc.expect || spec.TimeoutSeconds != tc.timeout ||
			spec.TimeoutDeclared != (tc.timeout != 0) {
			t.Errorf("%s: wrong typed metadata %+v", tc.path, spec)
		}
		if tc.mode == ModeEthernetTCP && (spec.BootTimeoutSeconds != 25 || spec.ProbeTimeoutSeconds != 15 || spec.Values["HIL_PORT"].Number != 7) {
			t.Errorf("Ethernet sub-step deadlines were lost: %+v", spec)
		}
	}
}

func TestParserRejectsShellSyntaxUnknownsAndInvalidTypes(t *testing.T) {
	cases := []string{
		"HIL_MODE=uart_scrape\nsource /tmp/evil\n",
		"HIL_MODE=uart_scrape\nEVIL=1\n",
		"HIL_MODE=uart_scrape\nHIL_TIMEOUT_S=$(id)\n",
		"HIL_MODE=uart_scrape\nHIL_EXPECT=\"$(id)\"\n",
		"HIL_MODE=uart_scrape\nHIL_EXPECT=\"`id`\"\n",
		"HIL_MODE=uart_scrape\nHIL_TIMEOUT_S=10;id\n",
		"HIL_MODE=uart_scrape\nHIL_TIMEOUT_S=10\nHIL_TIMEOUT_S=11\n",
		"HIL_MODE=uart_scrape\nHIL_TIMEOUT_S=0\n",
		"HIL_MODE=uart_scrape\nHIL_TIMEOUT_S=3601\n",
		"HIL_MODE=uart_scrape\nHIL_FAULT_EXPECTED=true\n",
		"HIL_MODE=uart_scrape\nHIL_PORT=65536\n",
		"HIL_MODE=not_a_mode\n",
		"HIL_MODE=uart_scrape\nHIL_BOARD_IP=host.example\n",
		"HIL_MODE=uart_scrape\nHIL_PROBE_SYMBOL=bad-symbol\n",
		"HIL_MODE=uart_scrape\nHIL_EXPECT=\"unterminated\n",
		"HIL_MODE=uart_scrape\nHIL_MAX_TIMEOUT_S=5\nHIL_TIMEOUT_S=10\n",
		"# only comment\n",
	}
	for _, contents := range cases {
		if _, err := Parse(strings.NewReader(contents), "examples/test/hil.conf"); !errors.Is(err, ErrInvalidManifest) {
			t.Errorf("unsafe/invalid manifest accepted: %q err=%v", contents, err)
		}
	}
	if _, err := Parse(strings.NewReader("HIL_MODE=uart_scrape\n"+strings.Repeat("x", maxManifestBytes)), "examples/test/hil.conf"); !errors.Is(err, ErrInvalidManifest) {
		t.Fatalf("oversized manifest accepted: %v", err)
	}
	if _, err := Parse(strings.NewReader("HIL_MODE=uart_scrape\nHIL_EXPECT=\""+strings.Repeat("x", maxLineBytes)+"\""), "examples/test/hil.conf"); !errors.Is(err, ErrInvalidManifest) {
		t.Fatalf("oversized line accepted: %v", err)
	}
}

func TestParserPreservesQuotedRegexWithoutExecutingIt(t *testing.T) {
	input := "# comment\nHIL_MODE=uart_scrape\nHIL_EXPECT=\"pass; literal [x] \"\nHIL_EXPECT_NEGATIVE=\"\\\\[bad\\\\]|HardFault\"\nHIL_TIMEOUT_S=12\nHIL_MAX_TIMEOUT_S=40\nHIL_FAULT_EXPECTED=1\n"
	spec, err := Parse(strings.NewReader(input), "examples/test/hil.conf")
	if err != nil {
		t.Fatal(err)
	}
	if spec.Expect != "pass; literal [x] " || spec.ExpectNegative != `\\[bad\\]|HardFault` ||
		spec.TimeoutSeconds != 12 || spec.SafetyMaximumSeconds != 40 || !spec.Values["HIL_FAULT_EXPECTED"].Flag {
		t.Fatalf("quoted metadata was changed: %+v", spec)
	}
}

func TestLoadRejectsTraversalAndSymlinkEscape(t *testing.T) {
	root := t.TempDir()
	examples := filepath.Join(root, "examples")
	if err := os.MkdirAll(filepath.Join(examples, "app"), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(examples, "app", "hil.conf"), []byte("HIL_MODE=alive\n"), 0600); err != nil {
		t.Fatal(err)
	}
	for _, path := range []string{"../outside/hil.conf", "/etc/passwd", "tools/hil.conf", "examples/app/../..//outside/hil.conf"} {
		if _, err := Load(root, path); !errors.Is(err, ErrUnsafePath) {
			t.Errorf("unsafe path accepted: %q err=%v", path, err)
		}
	}
	if spec, err := Load(root, "examples/app/hil.conf"); err != nil || spec.Mode != ModeAlive {
		t.Fatalf("safe manifest rejected: %+v err=%v", spec, err)
	}
	outside := filepath.Join(root, "outside.conf")
	if err := os.WriteFile(outside, []byte("HIL_MODE=alive\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(outside, filepath.Join(examples, "app", "escape")); err != nil {
		t.Fatal(err)
	}
	// The symlink itself must be the expected filename to exercise Load.
	if err := os.MkdirAll(filepath.Join(examples, "escape-app"), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(outside, filepath.Join(examples, "escape-app", "hil.conf")); err != nil {
		t.Fatal(err)
	}
	if _, err := Load(root, "examples/escape-app/hil.conf"); !errors.Is(err, ErrUnsafePath) {
		t.Fatalf("symlink escape accepted: %v", err)
	}
}

type brokenReader struct{}

func (brokenReader) Read([]byte) (int, error) { return 0, errors.New("read error") }

func TestParserRejectsUnreadableInput(t *testing.T) {
	if _, err := Parse(brokenReader{}, "examples/test/hil.conf"); !errors.Is(err, ErrInvalidManifest) {
		t.Fatalf("read failure accepted: %v", err)
	}
}
