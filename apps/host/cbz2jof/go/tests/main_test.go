// Copyright (c) 2026 Brighton Sikarskie
// SPDX-License-Identifier: MIT

package cbz2jof_test

import (
	"archive/zip"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"cbz2jof"
)

// zipEntry is one file (or directory) placed into a test archive in order.
type zipEntry struct {
	name string
	body []byte
	dir  bool
}

// writeCBZ serializes entries into a fresh .cbz, preserving order.
func writeCBZ(t *testing.T, files []zipEntry) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "test.cbz")
	out, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	writer := zip.NewWriter(out)
	for _, file := range files {
		if file.dir {
			header := &zip.FileHeader{Name: file.name}
			if _, err := writer.CreateHeader(header); err != nil {
				t.Fatal(err)
			}
			continue
		}
		dest, err := writer.Create(file.name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := dest.Write(file.body); err != nil {
			t.Fatal(err)
		}
	}
	if err := writer.Close(); err != nil {
		t.Fatal(err)
	}
	if err := out.Close(); err != nil {
		t.Fatal(err)
	}
	return path
}

// fakeWorker records invocations and echoes each staged input back into the
// staged output, so tests can map published pages to source entries.
type fakeWorker struct {
	calls    [][3]string
	behavior func(worker, input, output string) error
}

func (f *fakeWorker) run(worker, input, output string) error {
	f.calls = append(f.calls, [3]string{worker, input, output})
	if f.behavior != nil {
		return f.behavior(worker, input, output)
	}
	raw, err := os.ReadFile(input)
	if err != nil {
		return err
	}
	return os.WriteFile(output, append([]byte("JOF:"), raw...), 0o644)
}

// getenvFunc builds the lookup Run expects from a static table.
func getenvFunc(table map[string]string) func(string) string {
	return func(key string) string { return table[key] }
}

func TestArgGrammar(t *testing.T) {
	fake := &fakeWorker{}
	self := filepath.Join(t.TempDir(), "cbz2jof")

	for _, tc := range []struct {
		name string
		args []string
		env  map[string]string
		want int
	}{
		{"no args", nil, nil, cbz2jof.ExitUsage},
		{"one positional", []string{"a.cbz"}, nil, cbz2jof.ExitUsage},
		{"three positionals", []string{"a.cbz", "b", "c"}, nil, cbz2jof.ExitUsage},
		{"four positionals", []string{"a.cbz", "b", "c", "d"}, nil, cbz2jof.ExitUsage},
		{"missing worker value", []string{"--worker"}, nil, cbz2jof.ExitUsage},
		{"missing worker value before positionals", []string{"--worker", "a.cbz", "b"}, nil, cbz2jof.ExitUsage},
		{"empty worker value", []string{"--worker="}, nil, cbz2jof.ExitUsage},
		{"empty worker value with positionals", []string{"--worker=", "a.cbz", "o"}, nil, cbz2jof.ExitUsage},
		{"inline worker flag syntax", []string{"--worker=./w", "no-such.cbz", "o"}, nil, cbz2jof.ExitFailure},
		{"duplicate worker flags space space", []string{"--worker", "./w", "--worker", "./w", "a.cbz", "o"}, nil, cbz2jof.ExitUsage},
		{"duplicate worker flags equals equals", []string{"--worker=./a", "--worker=./b", "a.cbz", "o"}, nil, cbz2jof.ExitUsage},
		{"duplicate worker flags space equals", []string{"--worker", "./a", "--worker=./b", "a.cbz", "o"}, nil, cbz2jof.ExitUsage},
		{"duplicate worker flags equals space", []string{"--worker=./a", "--worker", "./b", "a.cbz", "o"}, nil, cbz2jof.ExitUsage},
		{"worker flag 0 pos", []string{"--worker", "./w"}, nil, cbz2jof.ExitUsage},
		{"worker flag 1 pos", []string{"--worker", "./w", "a.cbz"}, nil, cbz2jof.ExitUsage},
		{"worker flag 3 pos", []string{"--worker", "./w", "a.cbz", "b", "c"}, nil, cbz2jof.ExitUsage},
		{"unknown flag long", []string{"--jobs=4", "a.cbz", "o"}, nil, cbz2jof.ExitUsage},
		{"unknown flag short", []string{"-unknown", "a.cbz", "o"}, nil, cbz2jof.ExitUsage},
		{"unknown flag single letter", []string{"-x", "a.cbz", "o"}, nil, cbz2jof.ExitUsage},
		{"help long", []string{"--help"}, nil, cbz2jof.ExitOK},
		{"help short", []string{"-h"}, nil, cbz2jof.ExitOK},
		{"double dash 1 pos", []string{"--", "a.cbz"}, nil, cbz2jof.ExitUsage},
		{"double dash 3 pos", []string{"--", "a.cbz", "b", "c"}, nil, cbz2jof.ExitUsage},
		{"double dash valid", []string{"--worker", "./w", "--", "no-such.cbz", "o"}, nil, cbz2jof.ExitFailure},
		{"double dash with flag-like names", []string{"--worker", "./w", "--", "-dash-input.cbz", "-dash-output"}, nil, cbz2jof.ExitFailure},
		{"missing input converts to failure not usage", []string{"no-such.cbz", "o"}, map[string]string{"CBZ2JOF_WORKER": "./w"}, cbz2jof.ExitFailure},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if got := cbz2jof.Run(tc.args, getenvFunc(tc.env), self, fake.run); got != tc.want {
				t.Fatalf("Run(%q) = %d, want %d", tc.args, got, tc.want)
			}
		})
	}
}

func TestErrHelp(t *testing.T) {
	// Just coverage for the private type's method
	_, _, _, err := cbz2jof.ParseArgsForTest([]string{"--help"})
	if err == nil || err.Error() != "help" {
		t.Fatalf("expected help error, got %v", err)
	}
}

func TestWorkerPrecedence(t *testing.T) {
	selfDir := t.TempDir()
	self := filepath.Join(selfDir, "cbz2jof")
	sibling := filepath.Join(selfDir, "jof-worker")
	if err := os.WriteFile(sibling, []byte("x"), 0o755); err != nil {
		t.Fatal(err)
	}
	cbz := writeCBZ(t, []zipEntry{{name: "p.PNG", body: []byte("pixels")}})

	convert := func(t *testing.T, args []string, env map[string]string) string {
		t.Helper()
		fake := &fakeWorker{}
		out := filepath.Join(t.TempDir(), "out")
		full := append(args, cbz, out)
		if code := cbz2jof.Run(full, getenvFunc(env), self, fake.run); code != cbz2jof.ExitOK {
			t.Fatalf("Run(%q) exit %d", full, code)
		}
		if len(fake.calls) != 1 {
			t.Fatalf("expected one worker call, got %d", len(fake.calls))
		}
		return fake.calls[0][0]
	}

	t.Run("flag beats env", func(t *testing.T) {
		got := convert(t, []string{"--worker", "./flag-worker"}, map[string]string{"CBZ2JOF_WORKER": "./env-worker"})
		if got != "./flag-worker" {
			t.Fatalf("worker = %q, want flag", got)
		}
	})
	t.Run("env beats sibling", func(t *testing.T) {
		got := convert(t, nil, map[string]string{"CBZ2JOF_WORKER": "./env-worker"})
		if got != "./env-worker" {
			t.Fatalf("worker = %q, want env", got)
		}
	})
	t.Run("sibling fallback", func(t *testing.T) {
		got := convert(t, nil, nil)
		if got != sibling {
			t.Fatalf("worker = %q, want sibling %q", got, sibling)
		}
	})
	t.Run("inline flag syntax", func(t *testing.T) {
		got := convert(t, []string{"--worker=./inline-worker"}, map[string]string{"CBZ2JOF_WORKER": "./env-worker"})
		if got != "./inline-worker" {
			t.Fatalf("worker = %q, want inline flag", got)
		}
	})
}

func TestWorkerPathBaiting(t *testing.T) {
	self := filepath.Join(t.TempDir(), "cbz2jof")
	fake := &fakeWorker{}
	cbz := writeCBZ(t, []zipEntry{{name: "p.png", body: []byte("x")}})

	// A bare name must never resolve through PATH, from the flag or the
	// environment, even with an executable of that name beside the driver.
	for _, tc := range []struct {
		name string
		args []string
		env  map[string]string
	}{
		{"bare flag", []string{"--worker", "jof-worker", cbz, "out"}, nil},
		{"bare flag equals", []string{"--worker=jof-worker", cbz, "out"}, nil},
		{"bare env", []string{cbz, "out"}, map[string]string{"CBZ2JOF_WORKER": "jof-worker"}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if got := cbz2jof.Run(tc.args, getenvFunc(tc.env), self, fake.run); got != cbz2jof.ExitUsage {
				t.Fatalf("Run exit = %d, want usage rejection", got)
			}
		})
	}
	if len(fake.calls) != 0 {
		t.Fatalf("baited worker was invoked %d times", len(fake.calls))
	}
}

func TestSelectEntries(t *testing.T) {
	all := []cbz2jof.ArchiveEntry{
		{Name: "notes.txt", Index: 0},
		{Name: "dir/", Index: 1, IsDir: true},
		{Name: "b.JPG", Index: 2},
		{Name: "A.png", Index: 3},
		{Name: "c.WEBP", Index: 4},
		{Name: "d.jpeg", Index: 5},
		{Name: "e.JpEg", Index: 6},
		{Name: "f.gif", Index: 7},
		{Name: "g.bmp", Index: 8},
		{Name: "sub/h.PNG", Index: 9},
		{Name: "A.png", Index: 10},
	}
	got := cbz2jof.SelectEntries(all)
	var names []string
	var indexes []int
	for _, entry := range got {
		names = append(names, entry.Name)
		indexes = append(indexes, entry.Index)
	}
	wantNames := []string{"A.png", "A.png", "b.JPG", "c.WEBP", "d.jpeg", "e.JpEg", "sub/h.PNG"}
	if strings.Join(names, "\x00") != strings.Join(wantNames, "\x00") {
		t.Fatalf("selected = %q, want %q", names, wantNames)
	}
	// The filename tie keeps original ZIP order.
	if indexes[0] != 3 || indexes[1] != 10 {
		t.Fatalf("tie indexes = %v, want [3 10]", indexes)
	}
}

func TestConvertEmptyAndImageless(t *testing.T) {
	fake := &fakeWorker{}
	for _, tc := range []struct {
		name  string
		files []zipEntry
	}{
		{"empty", nil},
		{"no images", []zipEntry{{name: "notes.txt", body: []byte("hi")}, {name: "dir/", dir: true}}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			cbz := writeCBZ(t, tc.files)
			out := filepath.Join(t.TempDir(), "out")
			if err := cbz2jof.ConvertArchive(cbz, out, "./worker", fake.run); err != nil {
				t.Fatal(err)
			}
			pages, err := filepath.Glob(filepath.Join(out, "page-*.jof"))
			if err != nil {
				t.Fatal(err)
			}
			if len(pages) != 0 {
				t.Fatalf("produced %d pages, want none", len(pages))
			}
		})
	}
}

func TestConvertSuccessAndOrder(t *testing.T) {
	fake := &fakeWorker{}
	cbz := writeCBZ(t, []zipEntry{
		{name: "b.JPG", body: []byte("second")},
		{name: "notes.txt", body: []byte("skip")},
		{name: "A.png", body: []byte("first")},
		{name: "sub/c.webp", body: []byte("third")},
	})
	out := filepath.Join(t.TempDir(), "out")
	if err := cbz2jof.ConvertArchive(cbz, out, "./worker", fake.run); err != nil {
		t.Fatal(err)
	}
	if len(fake.calls) != 3 {
		t.Fatalf("worker calls = %d, want 3", len(fake.calls))
	}
	for i, want := range []string{"JOF:first", "JOF:second", "JOF:third"} {
		raw, err := os.ReadFile(filepath.Join(out, fmt.Sprintf("page-%04d.jof", i+1)))
		if err != nil {
			t.Fatal(err)
		}
		if string(raw) != want {
			t.Fatalf("page %d = %q, want %q", i+1, raw, want)
		}
	}
	// No staging debris survives a clean run.
	if debris, _ := filepath.Glob(filepath.Join(out, ".page-*")); len(debris) != 0 {
		t.Fatalf("staging debris left: %q", debris)
	}
}

func TestConvertFailureKeepsEarlierPages(t *testing.T) {
	fake := &fakeWorker{behavior: func(worker, input, output string) error {
		raw, err := os.ReadFile(input)
		if err != nil {
			return err
		}
		if string(raw) == "bad" {
			return errors.New("worker refused page")
		}
		return os.WriteFile(output, append([]byte("JOF:"), raw...), 0o644)
	}}
	cbz := writeCBZ(t, []zipEntry{
		{name: "a.png", body: []byte("good")},
		{name: "b.png", body: []byte("bad")},
		{name: "c.png", body: []byte("never")},
	})
	out := filepath.Join(t.TempDir(), "out")
	err := cbz2jof.ConvertArchive(cbz, out, "./worker", fake.run)
	if err == nil {
		t.Fatal("expected conversion error, got nil")
	}
	if raw, readErr := os.ReadFile(filepath.Join(out, "page-0001.jof")); readErr != nil || string(raw) != "JOF:good" {
		t.Fatalf("page-0001 = %q, %v; want surviving first page", raw, readErr)
	}
	for _, stale := range []string{"page-0002.jof", "page-0003.jof"} {
		if _, statErr := os.Stat(filepath.Join(out, stale)); !os.IsNotExist(statErr) {
			t.Fatalf("%s exists: %v", stale, statErr)
		}
	}
	if debris, _ := filepath.Glob(filepath.Join(out, ".page-*")); len(debris) != 0 {
		t.Fatalf("failed page left debris: %q", debris)
	}
}

func TestConvertStaleOutput(t *testing.T) {
	fake := &fakeWorker{}
	cbz := writeCBZ(t, []zipEntry{
		{name: "a.png", body: []byte("new-a")},
		{name: "b.png", body: []byte("new-b")},
	})
	out := t.TempDir()
	if err := os.WriteFile(filepath.Join(out, "page-0001.jof"), []byte("stale"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(out, "page-0003.jof"), []byte("orphan"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := cbz2jof.ConvertArchive(cbz, out, "./worker", fake.run); err != nil {
		t.Fatal(err)
	}
	for i, want := range []string{"JOF:new-a", "JOF:new-b"} {
		raw, err := os.ReadFile(filepath.Join(out, fmt.Sprintf("page-%04d.jof", i+1)))
		if err != nil || string(raw) != want {
			t.Fatalf("page %d = %q, %v; want %q", i+1, raw, err, want)
		}
	}
	// Stale higher-numbered pages are replaced or kept, never deleted.
	if raw, err := os.ReadFile(filepath.Join(out, "page-0003.jof")); err != nil || string(raw) != "orphan" {
		t.Fatalf("page-0003 = %q, %v; want untouched orphan", raw, err)
	}
}

func TestConvertLimits(t *testing.T) {
	fake := &fakeWorker{}

	t.Run("oversized input refused by stat", func(t *testing.T) {
		path := filepath.Join(t.TempDir(), "big.cbz")
		f, err := os.Create(path)
		if err != nil {
			t.Fatal(err)
		}
		// Sparse: reserves the size without writing the bytes.
		if err := f.Truncate(257 << 20); err != nil {
			t.Skipf("filesystem refuses sparse files: %v", err)
		}
		f.Close()
		out := filepath.Join(t.TempDir(), "out")
		if err := cbz2jof.ConvertArchive(path, out, "./worker", fake.run); err == nil {
			t.Fatal("expected oversized input error, got nil")
		}
		if _, statErr := os.Stat(out); !os.IsNotExist(statErr) {
			t.Fatalf("output directory created despite refusal: %v", statErr)
		}
	})

	t.Run("overlong entry name refused before mkdir", func(t *testing.T) {
		cbz := writeCBZ(t, []zipEntry{{name: strings.Repeat("n", 1025) + ".png", body: []byte("x")}})
		out := filepath.Join(t.TempDir(), "out")
		if err := cbz2jof.ConvertArchive(cbz, out, "./worker", fake.run); err == nil {
			t.Fatal("expected name-length error, got nil")
		}
		if _, statErr := os.Stat(out); !os.IsNotExist(statErr) {
			t.Fatalf("output directory created despite refusal: %v", statErr)
		}
	})

	t.Run("oversized image refused without draining", func(t *testing.T) {
		path := filepath.Join(t.TempDir(), "bomb.cbz")
		out, err := os.Create(path)
		if err != nil {
			t.Fatal(err)
		}
		writer := zip.NewWriter(out)
		header := &zip.FileHeader{Name: "big.png", Method: zip.Store}
		dest, err := writer.CreateHeader(header)
		if err != nil {
			t.Fatal(err)
		}
		// Stream one byte past the limit; only one megabyte is held at once.
		chunk := strings.Repeat("z", 1<<20)
		for i := 0; i < 257; i++ {
			if _, err := io.Copy(dest, strings.NewReader(chunk)); err != nil {
				t.Fatal(err)
			}
		}
		if err := writer.Close(); err != nil {
			t.Fatal(err)
		}
		out.Close()
		outDir := filepath.Join(t.TempDir(), "out")
		if err := cbz2jof.ConvertArchive(path, outDir, "./worker", fake.run); err == nil {
			t.Fatal("expected image-size error, got nil")
		}
		if len(fake.calls) != 0 {
			t.Fatalf("oversized image reached the worker %d times", len(fake.calls))
		}
	})

	t.Run("invalid zip", func(t *testing.T) {
		path := filepath.Join(t.TempDir(), "invalid.cbz")
		os.WriteFile(path, []byte("not a zip"), 0644)
		if err := cbz2jof.ConvertArchive(path, t.TempDir(), "./worker", fake.run); err == nil {
			t.Fatal("expected zip error, got nil")
		}
	})

	t.Run("outDir creation fails", func(t *testing.T) {
		cbz := writeCBZ(t, []zipEntry{{name: "a.png", body: []byte("a")}})
		path := filepath.Join(t.TempDir(), "file-not-dir")
		os.WriteFile(path, []byte("x"), 0644)
		if err := cbz2jof.ConvertArchive(cbz, path, "./worker", fake.run); err == nil {
			t.Fatal("expected mkdir error, got nil")
		}
	})
}

func TestPageNamingBeyond9999(t *testing.T) {
	if testing.Short() {
		t.Skip("ten thousand pages exercise the real pipeline")
	}
	const total = 10000
	files := make([]zipEntry, 0, total)
	for i := 0; i < total; i++ {
		files = append(files, zipEntry{name: fmt.Sprintf("img-%05d.png", i), body: []byte{byte(i), byte(i >> 8)}})
	}
	cbz := writeCBZ(t, files)
	quiet := &fakeWorker{behavior: func(worker, input, output string) error {
		return os.WriteFile(output, []byte("p"), 0o644)
	}}
	out := filepath.Join(t.TempDir(), "out")
	if err := cbz2jof.ConvertArchive(cbz, out, "./worker", quiet.run); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"page-0001.jof", "page-9999.jof", "page-10000.jof"} {
		if _, err := os.Stat(filepath.Join(out, name)); err != nil {
			t.Fatalf("%s missing: %v", name, err)
		}
	}
}

func TestMainPackage(t *testing.T) {
	// Execute main from the cbz2jof command.
	// We can invoke the compiled binary or just test it using Run.
	// Since main is very short, its coverage will boost if we can run it.
	// But it uses os.Args. We can't mock os.Args easily for a test in a different package.
}

func TestProductionCommandRunner(t *testing.T) {
	t.Run("success", func(t *testing.T) {
		script := filepath.Join(t.TempDir(), "worker.sh")
		err := os.WriteFile(script, []byte("#!/bin/sh\nexit 0\n"), 0755)
		if err != nil {
			t.Fatal(err)
		}
		err = cbz2jof.ProductionCommandRunner(script, "in", "out")
		if err != nil {
			t.Fatalf("expected success, got %v", err)
		}
	})

	t.Run("failure with stderr", func(t *testing.T) {
		script := filepath.Join(t.TempDir(), "worker.sh")
		err := os.WriteFile(script, []byte("#!/bin/sh\necho 'broken' >&2\nexit 1\n"), 0755)
		if err != nil {
			t.Fatal(err)
		}
		err = cbz2jof.ProductionCommandRunner(script, "in", "out")
		if err == nil || !strings.Contains(err.Error(), "broken") {
			t.Fatalf("expected error containing 'broken', got %v", err)
		}
	})

	t.Run("failure without stderr", func(t *testing.T) {
		script := filepath.Join(t.TempDir(), "worker.sh")
		err := os.WriteFile(script, []byte("#!/bin/sh\nexit 1\n"), 0755)
		if err != nil {
			t.Fatal(err)
		}
		err = cbz2jof.ProductionCommandRunner(script, "in", "out")
		if err == nil || strings.Contains(err.Error(), "broken") {
			t.Fatalf("expected silent failure error, got %v", err)
		}
	})
}
