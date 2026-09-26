// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package neutral

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

var (
	ErrHardwareBusy    = errors.New("protected hardware is still active")
	ErrSignalMismatch  = errors.New("fixture signal did not match its neutral profile")
	ErrProfileMismatch = errors.New("neutral profile does not match the server challenge")
	ErrGateUnavailable = errors.New("serialized hardware gate is unavailable")
	ErrUnknownSignal   = errors.New("profile signal source is unavailable")
)

// HardwareGate serializes observation with every ra8ci-mediated hardware
// operation. Production must pass the same gate to the board executor.
type HardwareGate interface {
	Lock(context.Context) (func(), error)
}

// SerialGate is a context-aware exclusive gate suitable for sharing between
// the board executor and neutral observer.
type SerialGate struct {
	semaphore chan struct{}
}

// NewSerialGate creates an initially-unlocked board hardware gate.
func NewSerialGate() *SerialGate { return &SerialGate{semaphore: make(chan struct{}, 1)} }

// Lock obtains the gate or returns when the context expires.
func (g *SerialGate) Lock(ctx context.Context) (func(), error) {
	if g == nil || g.semaphore == nil || ctx == nil {
		return nil, ErrGateUnavailable
	}
	select {
	case g.semaphore <- struct{}{}:
		var once sync.Once
		return func() { once.Do(func() { <-g.semaphore }) }, nil
	case <-ctx.Done():
		return nil, ctx.Err()
	}
}

// SignalReader reads one code-defined, read-only fixture signal source.
type SignalReader interface {
	ReadSignal(context.Context, string) (string, error)
}

// SignalReadFunc adapts a function to SignalReader.
type SignalReadFunc func(context.Context, string) (string, error)

// ReadSignal implements SignalReader.
func (f SignalReadFunc) ReadSignal(ctx context.Context, target string) (string, error) {
	return f(ctx, target)
}

// ActivityInspector refuses neutralization while known programming tools run
// or a process retains an open descriptor to a protected fixture device.
type ActivityInspector interface {
	CheckIdle(context.Context, []string, []string) error
}

// LinuxObserverConfig wires only trusted code adapters; untrusted profiles
// cannot select executables, network endpoints, or observer implementations.
type LinuxObserverConfig struct {
	Profile       Profile
	ProfileSHA256 string
	ProcRoot      string
	SysRoot       string
	DevRoot       string
	Gate          HardwareGate
	Inspector     ActivityInspector
	Readers       map[string]SignalReader
	Now           func() time.Time
}

// LinuxObserver performs read-only fixture checks under the serialized gate.
type LinuxObserver struct {
	profile Profile
	digest  string
	proc    string
	dev     string
	gate    HardwareGate
	idle    ActivityInspector
	readers map[string]SignalReader
	now     func() time.Time
}

var defaultProtectedProcesses = []string{
	"JLinkExe", "JLinkGDBServer", "rfp-cli", "openocd", "esptool", "esptool.py",
	"bench.sh", "bench_host.sh", "tapo_control.py", "ra8-hil-privileged", "ra8-hil-privileged.py",
}

// NewLinuxObserverFromFile loads the reviewed profile and binds its exact file digest.
func NewLinuxObserverFromFile(path string, config LinuxObserverConfig) (*LinuxObserver, error) {
	profile, digest, err := LoadProfile(path)
	if err != nil {
		return nil, err
	}
	config.Profile, config.ProfileSHA256 = profile, digest
	return newLinuxObserver(config)
}

// NewLinuxObserver validates a profile and creates a fail-closed Linux observer.
func newLinuxObserver(config LinuxObserverConfig) (*LinuxObserver, error) {
	config.Profile.Identity = append([]SignalCheck(nil), config.Profile.Identity...)
	config.Profile.State = append([]SignalCheck(nil), config.Profile.State...)
	config.Profile.Sensors = append([]RangeCheck(nil), config.Profile.Sensors...)
	config.Profile.ProtectedProcesses = append([]string(nil), config.Profile.ProtectedProcesses...)
	config.Profile.ProtectedDevices = append([]string(nil), config.Profile.ProtectedDevices...)
	if config.Gate == nil || !validSHA256(config.ProfileSHA256) ||
		ValidateProfile(config.Profile) != nil {
		return nil, ErrInvalidProfile
	}
	if config.ProcRoot == "" {
		config.ProcRoot = "/proc"
	}
	if config.DevRoot == "" {
		config.DevRoot = "/dev"
	}
	if config.Now == nil {
		config.Now = time.Now
	}
	if config.Inspector == nil {
		config.Inspector = LinuxActivityInspector{ProcRoot: config.ProcRoot, DevRoot: config.DevRoot}
	}
	readers := make(map[string]SignalReader, len(config.Readers)+1)
	for name, reader := range config.Readers {
		if name == "" || reader == nil {
			return nil, ErrUnknownSignal
		}
		readers[name] = reader
	}
	if config.SysRoot == "" {
		config.SysRoot = "/sys"
	}
	if readers["sysfs"] == nil {
		readers["sysfs"] = SysfsReader{Root: config.SysRoot}
	}
	for _, check := range append(append([]SignalCheck(nil), config.Profile.Identity...), config.Profile.State...) {
		if readers[check.Source] == nil {
			return nil, ErrUnknownSignal
		}
	}
	for _, check := range config.Profile.Sensors {
		if readers[check.Source] == nil {
			return nil, ErrUnknownSignal
		}
	}
	return &LinuxObserver{profile: config.Profile, digest: config.ProfileSHA256,
		proc: config.ProcRoot, dev: config.DevRoot, gate: config.Gate,
		idle: config.Inspector, readers: readers, now: config.Now}, nil
}

// ObserveNeutral returns positive evidence only when identity, electrical
// state, VTref, process quiescence, and protected device ownership all match.
func (o *LinuxObserver) ObserveNeutral(ctx context.Context, challenge store.NeutralChallenge) (Observation, error) {
	if o == nil || ctx == nil || o.gate == nil || o.idle == nil {
		return Observation{}, ErrGateUnavailable
	}
	if challenge.BoardID != o.profile.BoardID || challenge.FixtureRevision != o.profile.FixtureRevision ||
		challenge.ProfileSHA256 != o.digest || challenge.RestorePolicy != o.profile.RestorePolicy {
		return Observation{}, ErrProfileMismatch
	}
	unlock, err := o.gate.Lock(ctx)
	if err != nil {
		return Observation{}, err
	}
	defer unlock()
	if err := ctx.Err(); err != nil {
		return Observation{}, err
	}
	devices := make([]string, len(o.profile.ProtectedDevices))
	for i, path := range o.profile.ProtectedDevices {
		devices[i] = filepath.Join(o.dev, filepath.FromSlash(path))
	}
	processes := append(append([]string(nil), defaultProtectedProcesses...), o.profile.ProtectedProcesses...)
	if err := o.idle.CheckIdle(ctx, processes, devices); err != nil {
		return Observation{}, fmt.Errorf("%w: %v", ErrHardwareBusy, err)
	}
	evidence := observationEvidence{ProfileSHA256: o.digest, ActivityClear: true,
		ProtectedDevices:   append([]string(nil), o.profile.ProtectedDevices...),
		ProtectedProcesses: append(append([]string(nil), defaultProtectedProcesses...), o.profile.ProtectedProcesses...)}
	for _, check := range o.profile.Identity {
		value, err := o.readers[check.Source].ReadSignal(ctx, check.Target)
		if err != nil {
			return Observation{}, fmt.Errorf("%w: identity %s: %v", ErrSignalMismatch, check.Name, err)
		}
		value = strings.TrimSpace(value)
		evidence.Identity = append(evidence.Identity, observedSignal{Name: check.Name, Value: value})
		if value != check.Expected {
			return Observation{}, fmt.Errorf("%w: identity %s", ErrSignalMismatch, check.Name)
		}
	}
	for _, check := range o.profile.State {
		value, err := o.readers[check.Source].ReadSignal(ctx, check.Target)
		if err != nil {
			return Observation{}, fmt.Errorf("%w: state %s: %v", ErrSignalMismatch, check.Name, err)
		}
		value = strings.TrimSpace(value)
		evidence.State = append(evidence.State, observedSignal{Name: check.Name, Value: value})
		if value != check.Expected {
			return Observation{}, fmt.Errorf("%w: state %s", ErrSignalMismatch, check.Name)
		}
	}
	for _, check := range o.profile.Sensors {
		raw, err := o.readers[check.Source].ReadSignal(ctx, check.Target)
		if err != nil {
			return Observation{}, fmt.Errorf("%w: sensor %s: %v", ErrSignalMismatch, check.Name, err)
		}
		value, err := strconv.ParseFloat(strings.TrimSpace(raw), 64)
		if err != nil || math.IsNaN(value) || math.IsInf(value, 0) ||
			value < check.Min || value > check.Max {
			return Observation{}, fmt.Errorf("%w: sensor %s", ErrSignalMismatch, check.Name)
		}
		evidence.Sensors = append(evidence.Sensors, observedSensor{Name: check.Name, Value: value})
	}
	encoded, err := json.Marshal(evidence)
	if err != nil || len(encoded) == 0 || len(encoded) > MaxEvidenceBytes {
		return Observation{}, ErrObservationAbsent
	}
	observedAt := o.now().UTC()
	return Observation{ChallengeID: challenge.ID, Nonce: challenge.Nonce,
		BoardID: challenge.BoardID, LeaseID: challenge.LeaseID,
		Generation: challenge.Generation, AgentHighWater: challenge.AgentHighWater,
		FixtureRevision: challenge.FixtureRevision, ProfileSHA256: challenge.ProfileSHA256,
		Neutral: true, Evidence: encoded, ObservedAt: observedAt}, nil
}

type observationEvidence struct {
	ProfileSHA256      string           `json:"profile_sha256"`
	ActivityClear      bool             `json:"activity_clear"`
	ProtectedDevices   []string         `json:"protected_devices"`
	ProtectedProcesses []string         `json:"protected_processes"`
	Identity           []observedSignal `json:"identity"`
	State              []observedSignal `json:"state"`
	Sensors            []observedSensor `json:"sensors"`
}

type observedSignal struct {
	Name  string `json:"name"`
	Value string `json:"value"`
}

type observedSensor struct {
	Name  string  `json:"name"`
	Value float64 `json:"value"`
}

// SysfsReader reads one bounded scalar beneath its configured sysfs root.
type SysfsReader struct{ Root string }

// ReadSignal implements SignalReader and refuses symlink escapes or non-files.
func (r SysfsReader) ReadSignal(ctx context.Context, target string) (string, error) {
	if ctx == nil || ctx.Err() != nil || !validRelativePath(target) || r.Root == "" {
		return "", ErrInvalidProfile
	}
	root, err := filepath.EvalSymlinks(r.Root)
	if err != nil {
		return "", err
	}
	path, err := filepath.EvalSymlinks(filepath.Join(root, filepath.FromSlash(target)))
	if err != nil {
		return "", err
	}
	if !withinRoot(root, path) {
		return "", ErrInvalidProfile
	}
	info, err := os.Stat(path)
	if err != nil || !info.Mode().IsRegular() {
		return "", ErrInvalidProfile
	}
	file, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer file.Close()
	raw, err := io.ReadAll(io.LimitReader(file, 4097))
	if err != nil || len(raw) > 4096 {
		return "", ErrInvalidProfile
	}
	return string(raw), nil
}

func withinRoot(root, path string) bool {
	relative, err := filepath.Rel(root, path)
	return err == nil && relative != "." && relative != ".." &&
		!strings.HasPrefix(relative, ".."+string(filepath.Separator)) && !filepath.IsAbs(relative)
}

// LinuxActivityInspector checks process names and protected device descriptors
// through procfs. Any uninspectable live process fails closed.
type LinuxActivityInspector struct {
	ProcRoot string
	DevRoot  string
}

// CheckIdle implements ActivityInspector without exposing command lines or PIDs.
func (i LinuxActivityInspector) CheckIdle(ctx context.Context, processNames, devicePaths []string) error {
	if ctx == nil || i.ProcRoot == "" || i.DevRoot == "" {
		return ErrObservationAbsent
	}
	procRoot, err := filepath.EvalSymlinks(i.ProcRoot)
	if err != nil {
		return err
	}
	devRoot, err := filepath.EvalSymlinks(i.DevRoot)
	if err != nil {
		return err
	}
	protected := make(map[string]bool, len(processNames))
	for _, name := range processNames {
		protected[strings.ToLower(name)] = true
	}
	devices := make(map[string]bool, len(devicePaths))
	for _, path := range devicePaths {
		if !withinRoot(devRoot, path) {
			return ErrInvalidProfile
		}
		resolved, err := filepath.EvalSymlinks(path)
		if err != nil || !withinRoot(devRoot, resolved) {
			return ErrObservationAbsent
		}
		info, err := os.Stat(resolved)
		if err != nil || info.Mode()&os.ModeDevice == 0 {
			return ErrObservationAbsent
		}
		devices[resolved] = true
	}
	entries, err := os.ReadDir(procRoot)
	if err != nil {
		return err
	}
	for _, entry := range entries {
		if err := ctx.Err(); err != nil {
			return err
		}
		if !entry.IsDir() || !numericPID(entry.Name()) {
			continue
		}
		processDir := filepath.Join(procRoot, entry.Name())
		comm, err := os.ReadFile(filepath.Join(processDir, "comm"))
		if err != nil {
			if errors.Is(err, os.ErrNotExist) {
				continue
			}
			return fmt.Errorf("inspect process: %w", err)
		}
		if protected[strings.ToLower(strings.TrimSpace(string(comm)))] {
			return ErrHardwareBusy
		}
		cmdline, err := os.ReadFile(filepath.Join(processDir, "cmdline"))
		if err == nil {
			if len(cmdline) > 128<<10 {
				return ErrObservationAbsent
			}
			for _, argument := range strings.Split(string(cmdline), "\x00") {
				if protected[strings.ToLower(filepath.Base(argument))] {
					return ErrHardwareBusy
				}
			}
		} else if !errors.Is(err, os.ErrNotExist) {
			return fmt.Errorf("inspect process command line: %w", err)
		}
		fdDir := filepath.Join(processDir, "fd")
		fds, err := os.ReadDir(fdDir)
		if err != nil {
			if errors.Is(err, os.ErrNotExist) {
				continue
			}
			return fmt.Errorf("inspect process descriptors: %w", err)
		}
		for _, fd := range fds {
			if err := ctx.Err(); err != nil {
				return err
			}
			target, err := os.Readlink(filepath.Join(fdDir, fd.Name()))
			if err != nil {
				if errors.Is(err, os.ErrNotExist) {
					continue
				}
				return fmt.Errorf("inspect process descriptor: %w", err)
			}
			resolved, err := filepath.EvalSymlinks(target)
			if err != nil {
				// An unresolvable descriptor is evidence only when it names a
				// path under the protected device root; see
				// descriptorCouldBeProtectedDevice.
				if descriptorCouldBeProtectedDevice(devRoot, target, len(devices) > 0) {
					return ErrObservationAbsent
				}
				continue
			}
			if devices[resolved] {
				return ErrHardwareBusy
			}
		}
	}
	return nil
}

func numericPID(value string) bool {
	if value == "" {
		return false
	}
	for _, r := range value {
		if r < '0' || r > '9' {
			return false
		}
	}
	return true
}
