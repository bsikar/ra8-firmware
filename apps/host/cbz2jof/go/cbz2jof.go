// Copyright (c) 2026 Brighton Sikarskie
// SPDX-License-Identifier: MIT

// Package cbz2jof converts a CBZ archive into one JOF page atlas per image
// by shelling out to the jof-worker C binary for each selected entry.
//
// Archive policy lives here (argument grammar, worker resolution, entry
// selection and ordering, resource limits, atomic page publication); pixels
// belong to the worker, which runs the firmware's own jof_produce pipeline.
package cbz2jof

import (
	"archive/zip"
	"bytes"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
)

const (
	maxCBZBytes = 256 << 20
	// maxEntries caps the ZIP central-directory entry count.
	maxEntries = 65536
	// maxNameBytes caps one ZIP entry name in bytes.
	maxNameBytes = 1024
	// maxSelected caps the image entries converted from one archive.
	maxSelected = 65536
	// maxImageBytes caps one extracted image in bytes.
	maxImageBytes = 256 << 20
)

// Process exit codes.
const (
	// ExitOK reports a fully converted archive.
	ExitOK = 0
	// ExitFailure reports a conversion failure (I/O, limits, worker).
	ExitFailure = 1
	// ExitUsage reports an argument or worker-resolution error.
	ExitUsage = 2
)

// workerBinary is the sibling executable name resolved beside the CLI.
const workerBinary = "jof-worker"

// workerEnv is the environment variable overriding worker resolution.
const workerEnv = "CBZ2JOF_WORKER"

// CommandRunner invokes the worker for one image. It reports worker failure
// as a non-nil error; page publication and cleanup stay with the caller.
type CommandRunner func(worker, input, output string) error

// ArchiveEntry is the selection view of one ZIP entry.
type ArchiveEntry struct {
	// Name is the entry path in byte-wise sort order.
	Name string
	// Index is the original ZIP position, breaking filename ties.
	Index int
	// Size is the declared uncompressed size in bytes.
	Size uint64
	// IsDir marks directory entries, which are never images.
	IsDir bool
}

// imageExtensions is the convertible set, matched case-insensitively.
var imageExtensions = map[string]bool{
	".jpg":  true,
	".jpeg": true,
	".png":  true,
	".webp": true,
}

// SelectEntries filters regular image entries and sorts them by byte-wise
// filename order, then by original ZIP index for ties.
func SelectEntries(all []ArchiveEntry) []ArchiveEntry {
	var selected []ArchiveEntry
	for _, entry := range all {
		if entry.IsDir {
			continue
		}
		ext := strings.ToLower(filepath.Ext(entry.Name))
		if !imageExtensions[ext] {
			continue
		}
		selected = append(selected, entry)
	}
	sort.Slice(selected, func(i, j int) bool {
		if selected[i].Name != selected[j].Name {
			return selected[i].Name < selected[j].Name
		}
		return selected[i].Index < selected[j].Index
	})
	return selected
}

// ProductionCommandRunner runs the exact worker binary with the exact input
// and output paths, capturing stderr into the returned error.
func ProductionCommandRunner(worker, input, output string) error {
	cmd := exec.Command(worker, input, output)
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	if err := cmd.Run(); err != nil {
		if clipped := bytes.TrimSpace(stderr.Bytes()); len(clipped) > 0 {
			return fmt.Errorf("cbz2jof: worker failed on %s: %v: %s", input, err, clipped)
		}
		return fmt.Errorf("cbz2jof: worker failed on %s: %v", input, err)
	}
	return nil
}

// usage reports the CLI grammar on stderr.
func usage() {
	fmt.Fprintf(os.Stderr, "usage: cbz2jof [--worker PATH] input.cbz output-directory\n")
}

// parseArgs splits at most one --worker flag from exactly two positionals.
func parseArgs(args []string) (worker string, workerSet bool, positional []string, err error) {
	for i := 0; i < len(args); i++ {
		arg := args[i]
		switch {
		case arg == "--":
			positional = append(positional, args[i+1:]...)
			i = len(args)
		case arg == "--worker":
			if workerSet {
				return "", false, nil, fmt.Errorf("duplicate --worker flag")
			}
			if i+1 >= len(args) {
				return "", false, nil, fmt.Errorf("missing value for --worker")
			}
			i++
			worker, workerSet = args[i], true
		case strings.HasPrefix(arg, "--worker="):
			if workerSet {
				return "", false, nil, fmt.Errorf("duplicate --worker flag")
			}
			worker, workerSet = strings.TrimPrefix(arg, "--worker="), true
			if worker == "" {
				return "", false, nil, fmt.Errorf("missing value for --worker")
			}
		case arg == "--help" || arg == "-h":
			return "", false, nil, ErrHelp{}
		case strings.HasPrefix(arg, "-") && len(arg) > 1:
			return "", false, nil, fmt.Errorf("unknown flag %q", arg)
		default:
			positional = append(positional, arg)
		}
	}
	if len(positional) != 2 {
		return "", false, nil, fmt.Errorf("expected input.cbz and output-directory, got %d positional arguments", len(positional))
	}
	return worker, workerSet, positional, nil
}

func ParseArgsForTest(args []string) (string, bool, []string, error) {
	return parseArgs(args)
}

// ErrHelp signals a help request, which exits zero after printing usage.
type ErrHelp struct{}

func (ErrHelp) Error() string { return "help" }

// HasSeparator reports a path that cannot be a bare PATH lookup.
func HasSeparator(path string) bool {
	for i := range len(path) {
		if os.IsPathSeparator(path[i]) {
			return true
		}
	}
	return false
}

func resolveWorker(flag string, flagSet bool, getenv func(string) string, selfExe string) (string, error) {
	if flagSet {
		if !HasSeparator(flag) {
			return "", fmt.Errorf("--worker path must contain a path separator: %q", flag)
		}
		return flag, nil
	}
	if env := getenv(workerEnv); env != "" {
		if !HasSeparator(env) {
			return "", fmt.Errorf("%s path must contain a path separator: %q", workerEnv, env)
		}
		return env, nil
	}
	sibling := filepath.Join(filepath.Dir(selfExe), workerBinary)
	info, err := os.Stat(sibling)
	if err != nil || info.IsDir() || info.Mode().Perm()&0o111 == 0 {
		return "", fmt.Errorf("no --worker given and sibling %q is not executable", sibling)
	}
	return sibling, nil
}

// Run parses args, resolves the worker, and converts the archive.
func Run(args []string, getenv func(string) string, selfExe string, runner CommandRunner) int {
	workerFlag, workerSet, positional, err := parseArgs(args)
	if _, help := err.(ErrHelp); help {
		fmt.Fprintf(os.Stderr, "usage: cbz2jof [--worker PATH] input.cbz output-directory\n")
		return ExitOK
	}
	if err != nil {
		fmt.Fprintf(os.Stderr, "cbz2jof: %v\n", err)
		usage()
		return ExitUsage
	}
	worker, err := resolveWorker(workerFlag, workerSet, getenv, selfExe)
	if err != nil {
		fmt.Fprintf(os.Stderr, "cbz2jof: %v\n", err)
		usage()
		return ExitUsage
	}
	if err := ConvertArchive(positional[0], positional[1], worker, runner); err != nil {
		fmt.Fprintf(os.Stderr, "cbz2jof: %v\n", err)
		return ExitFailure
	}
	return ExitOK
}

// ConvertArchive converts every selected image in cbzPath into outDir as
// page-0001.jof, page-0002.jof, and so on. Each page publishes through a
// temporary file plus rename, so a failed page never appears as a completed
// JOF file; earlier pages survive a later failure. Reruns replace only the
// pages they convert; stale higher-numbered pages are left alone.
func ConvertArchive(cbzPath, outDir, worker string, runner CommandRunner) error {
	info, err := os.Stat(cbzPath)
	if err != nil {
		return fmt.Errorf("cannot stat input: %w", err)
	}
	if info.IsDir() || info.Size() > maxCBZBytes {
		return fmt.Errorf("input exceeds %d bytes", maxCBZBytes)
	}
	reader, err := zip.OpenReader(cbzPath)
	if err != nil {
		return fmt.Errorf("cannot open archive: %w", err)
	}
	defer reader.Close()

	if len(reader.File) > maxEntries {
		return fmt.Errorf("archive holds %d entries, limit %d", len(reader.File), maxEntries)
	}
	all := make([]ArchiveEntry, 0, len(reader.File))
	for i, file := range reader.File {
		if len(file.Name) > maxNameBytes {
			return fmt.Errorf("entry name exceeds %d bytes", maxNameBytes)
		}
		all = append(all, ArchiveEntry{
			Name:  file.Name,
			Index: i,
			Size:  file.UncompressedSize64,
			IsDir: file.FileInfo().IsDir(),
		})
	}
	selected := SelectEntries(all)
	if len(selected) > maxSelected {
		return fmt.Errorf("archive selects %d images, limit %d", len(selected), maxSelected)
	}

	if err := os.MkdirAll(outDir, 0o755); err != nil {
		return fmt.Errorf("cannot create output directory: %w", err)
	}

	for n, entry := range selected {
		if err := convertEntry(reader.File[entry.Index], outDir, n+1, worker, runner); err != nil {
			return err
		}
	}
	return nil
}

// convertEntry extracts one entry through the worker and atomically publishes
// page number n.
func convertEntry(file *zip.File, outDir string, n int, worker string, runner CommandRunner) error {
	source, err := file.Open()
	if err != nil {
		return fmt.Errorf("cannot open entry %q: %w", file.Name, err)
	}
	defer source.Close()

	input, err := os.CreateTemp("", "cbz2jof-in-*")
	if err != nil {
		return fmt.Errorf("cannot stage entry %q: %w", file.Name, err)
	}
	inputName := input.Name()
	copied, copyErr := copyBounded(input, source)
	closeErr := input.Close()
	if copyErr != nil || closeErr != nil || copied > maxImageBytes {
		os.Remove(inputName)
		if copyErr != nil {
			return fmt.Errorf("cannot extract entry %q: %w", file.Name, copyErr)
		}
		if closeErr != nil {
			return fmt.Errorf("cannot stage entry %q: %w", file.Name, closeErr)
		}
		return fmt.Errorf("entry %q exceeds %d bytes", file.Name, maxImageBytes)
	}
	staged, err := os.CreateTemp(outDir, ".page-*.jof.tmp")
	if err != nil {
		os.Remove(inputName)
		return fmt.Errorf("cannot stage page %d: %w", n, err)
	}
	stagedName := staged.Name()
	if err := staged.Close(); err != nil {
		os.Remove(stagedName)
		os.Remove(inputName)
		return fmt.Errorf("cannot stage page %d: %w", n, err)
	}

	runErr := runner(worker, inputName, stagedName)
	os.Remove(inputName)
	if runErr != nil {
		os.Remove(stagedName)
		return fmt.Errorf("page %d (%s): %w", n, file.Name, runErr)
	}
	final := filepath.Join(outDir, fmt.Sprintf("page-%04d.jof", n))
	if err := os.Rename(stagedName, final); err != nil {
		os.Remove(stagedName)
		return fmt.Errorf("cannot publish page %d: %w", n, err)
	}
	return nil
}

// copyBounded streams one entry to disk, stopping one byte past the image
// limit without draining an oversized entry.
func copyBounded(dst *os.File, src io.Reader) (int64, error) {
	return io.Copy(dst, io.LimitReader(src, maxImageBytes+1))
}
