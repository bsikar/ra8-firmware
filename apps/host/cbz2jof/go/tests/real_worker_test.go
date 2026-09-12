// Copyright (c) 2026 Brighton Sikarskie
// SPDX-License-Identifier: MIT

//go:build realworker

package cbz2jof_test

import (
	"archive/zip"
	"bytes"
	"encoding/binary"
	"image"
	"image/color"
	"image/png"
	"os"
	"os/exec"
	"path/filepath"
	"testing"

	"cbz2jof"
)

// workerPath returns the C worker binary staged by CMake, or skips when the
// tag is run by hand without one.
func workerPath(t *testing.T) string {
	t.Helper()
	path := os.Getenv("CBZ2JOF_WORKER")
	if path == "" {
		t.Skip("CBZ2JOF_WORKER is not set; run through ctest")
	}
	if info, err := os.Stat(path); err != nil || info.IsDir() {
		t.Fatalf("worker %q is not a file: %v", path, err)
	}
	return path
}

// onePixelPNG encodes a 1x1 translucent red image through the standard
// library. The pixel is deliberately not opaque: the encoder drops a fully
// opaque alpha channel, so translucency pins the RGBA8888 producer path.
func onePixelPNG(t *testing.T) []byte {
	t.Helper()
	img := image.NewNRGBA(image.Rect(0, 0, 1, 1))
	img.Set(0, 0, color.NRGBA{R: 0xff, G: 0x00, B: 0x00, A: 0x80})
	var raw bytes.Buffer
	if err := png.Encode(&raw, img); err != nil {
		t.Fatal(err)
	}
	return raw.Bytes()
}

// writeCBZFile stores one entry under name in a fresh archive.
func writeCBZFile(t *testing.T, name string, body []byte) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "one.cbz")
	out, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	writer := zip.NewWriter(out)
	dest, err := writer.Create(name)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := dest.Write(body); err != nil {
		t.Fatal(err)
	}
	if err := writer.Close(); err != nil {
		t.Fatal(err)
	}
	if err := out.Close(); err != nil {
		t.Fatal(err)
	}
	return path
}

// parseJOF validates the header, footer, index window, and total-size fields.
func parseJOF(t *testing.T, raw []byte) {
	t.Helper()
	if len(raw) < 48 {
		t.Fatalf("atlas is %d bytes, shorter than header plus footer", len(raw))
	}
	if string(raw[0:4]) != "JOF1" {
		t.Fatalf("header magic = %q, want JOF1", raw[0:4])
	}
	width := binary.LittleEndian.Uint16(raw[4:6])
	height := binary.LittleEndian.Uint16(raw[6:8])
	tileW := binary.LittleEndian.Uint16(raw[8:10])
	tileH := binary.LittleEndian.Uint16(raw[10:12])
	bpp := raw[12]
	codec := raw[13]
	count := binary.LittleEndian.Uint32(raw[16:20])
	if width != 1 || height != 1 {
		t.Fatalf("geometry = %dx%d, want 1x1", width, height)
	}
	if tileW != 1 || tileH != 1 {
		t.Fatalf("tiles = %dx%d, want full-width 1x1 bands", tileW, tileH)
	}
	if bpp != 4 {
		t.Fatalf("bpp = %d, want RGBA8888", bpp)
	}
	if codec != 1 {
		t.Fatalf("codec = %d, want deflate", codec)
	}
	if count != 1 {
		t.Fatalf("tile count = %d, want 1", count)
	}

	footer := raw[len(raw)-16:]
	indexOff := binary.LittleEndian.Uint32(footer[0:4])
	footCount := binary.LittleEndian.Uint32(footer[4:8])
	total := binary.LittleEndian.Uint32(footer[8:12])
	if string(footer[12:16]) != "JOFE" {
		t.Fatalf("footer magic = %q, want JOFE", footer[12:16])
	}
	if footCount != count {
		t.Fatalf("footer count = %d, header count = %d", footCount, count)
	}
	if total != uint32(len(raw)) {
		t.Fatalf("total size = %d, file size = %d", total, len(raw))
	}
	if indexOff+count*8+16 != total {
		t.Fatalf("index window [%d, %d) escapes total %d", indexOff, indexOff+count*8+16, total)
	}
	entryOff := binary.LittleEndian.Uint32(raw[indexOff : indexOff+4])
	entryLen := binary.LittleEndian.Uint32(raw[indexOff+4 : indexOff+8])
	if entryOff != 32 || entryOff+entryLen != indexOff {
		t.Fatalf("tile window [%d, %d) is not the bytes between header and index %d", entryOff, entryOff+entryLen, indexOff)
	}
}

func TestRealWorkerConvert(t *testing.T) {
	worker := workerPath(t)
	cbz := writeCBZFile(t, "page.PNG", onePixelPNG(t))
	out := filepath.Join(t.TempDir(), "out")
	if err := cbz2jof.ConvertArchive(cbz, out, worker, cbz2jof.ProductionCommandRunner); err != nil {
		t.Fatal(err)
	}
	raw, err := os.ReadFile(filepath.Join(out, "page-0001.jof"))
	if err != nil {
		t.Fatal(err)
	}
	parseJOF(t, raw)
}

func TestWorkerResultCodes(t *testing.T) {
	worker := workerPath(t)

	run := func(input, output string) int {
		t.Helper()
		cmd := exec.Command(worker, input, output)
		if err := cmd.Run(); err == nil {
			t.Fatalf("worker unexpectedly succeeded for %q", input)
			return -1
		} else if exit, ok := err.(*exec.ExitError); ok {
			return exit.ExitCode()
		} else {
			t.Fatalf("worker did not exit: %v", err)
			return -1
		}
	}

	t.Run("malformed input is geometry", func(t *testing.T) {
		bad := filepath.Join(t.TempDir(), "bad.png")
		if err := os.WriteFile(bad, []byte("not an image"), 0o644); err != nil {
			t.Fatal(err)
		}
		if code := run(bad, filepath.Join(t.TempDir(), "out.jof")); code != 4 {
			t.Fatalf("exit = %d, want 4 (geometry)", code)
		}
	})

	t.Run("unopenable output is output", func(t *testing.T) {
		good := filepath.Join(t.TempDir(), "good.png")
		if err := os.WriteFile(good, onePixelPNG(t), 0o644); err != nil {
			t.Fatal(err)
		}
		missing := filepath.Join(t.TempDir(), "no-such-dir", "out.jof")
		if code := run(good, missing); code != 3 {
			t.Fatalf("exit = %d, want 3 (output)", code)
		}
	})

	t.Run("usage without paths", func(t *testing.T) {
		err := exec.Command(worker).Run()
		exit, ok := err.(*exec.ExitError)
		if !ok {
			t.Fatalf("worker did not exit: %v", err)
		}
		if exit.ExitCode() != 1 {
			t.Fatalf("exit = %d, want 1 (usage)", exit.ExitCode())
		}
	})
}
