// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package catalog validates the reviewed task definitions embedded in ra8ci.
package catalog

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path"
	"path/filepath"
	"runtime"
	"strings"
	"time"

	embedded "github.com/bsikar/ra8-firmware/tools/ra8ci/catalog"
)

const SchemaVersion = 1

var (
	ErrInvalidCatalog  = errors.New("invalid task catalog")
	ErrDigestMismatch  = errors.New("task catalog digest mismatch")
	ErrInvalidCheckout = errors.New("invalid repository checkout")
)

// ArgsSchema is deliberately closed until a task with reviewed arguments is added.
type ArgsSchema struct {
	Positional []string `json:"positional"`
	Flags      []string `json:"flags"`
}

// Step is an argv command, never a string passed to a shell for evaluation.
type Step struct {
	Name    string   `json:"name"`
	Program string   `json:"program"`
	Args    []string `json:"args"`
}

// RetryPolicy describes the maximum number of task attempts.
type RetryPolicy struct {
	MaxAttempts int `json:"max_attempts"`
}

// ResourceHints are static placement hints, not measured host facts.
type ResourceHints struct {
	CPU            int   `json:"cpu,omitempty"`
	RAMBytes       int64 `json:"ram_bytes,omitempty"`
	MaxParallelism int   `json:"max_parallelism,omitempty"`
}

// HILTask binds a HIL task to reviewed manifest and board identity. Fixture
// revision and profile hash are captured from the granted board session.
type HILTask struct {
	BoardID              string `json:"board_id"`
	BoardModel           string `json:"board_model"`
	ManifestPath         string `json:"manifest_path"`
	ProgramFamily        string `json:"program_family"`
	Mode                 string `json:"mode"`
	ObservationStep      string `json:"observation_step"`
	FlashRestoreSeconds  int    `json:"flash_restore_seconds"`
	TimeoutDeclared      bool   `json:"timeout_declared"`
	TimeoutSeconds       int    `json:"timeout_seconds,omitempty"`
	SafetyMaximumSeconds int    `json:"safety_maximum_seconds,omitempty"`

	// HandoffSafeStepSeconds and HandoffRestoreProbeSeconds are the reviewed
	// bounds on giving the board up: the longest indivisible step this task
	// may be in the middle of when it is asked to yield, and the longest
	// restore-and-probe that follows before the board is neutral again.
	// Their sum is the safety bound a handoff ETA may never be quoted below.
	//
	// They are declared together or not at all. Undeclared is a real
	// answer, not a default: a task with no declared bounds has an unknown
	// handoff ETA, which a person may still choose to wait out and a
	// scheduler may not dispatch against.
	HandoffSafeStepSeconds     int `json:"handoff_safe_step_seconds,omitempty"`
	HandoffRestoreProbeSeconds int `json:"handoff_restore_probe_seconds,omitempty"`
}

// HandoffBoundsDeclared reports whether this task states what giving the
// board up costs.
func (h HILTask) HandoffBoundsDeclared() bool {
	return h.HandoffSafeStepSeconds > 0 && h.HandoffRestoreProbeSeconds > 0
}

// HandoffSafetyBound is the declared request-to-neutral safety bound, zero
// when the task declares none. It is a floor under an estimate, never an
// estimate: history may only ever push a quoted ETA above it.
func (h HILTask) HandoffSafetyBound() time.Duration {
	if !h.HandoffBoundsDeclared() {
		return 0
	}
	return time.Duration(h.HandoffSafeStepSeconds+h.HandoffRestoreProbeSeconds) * time.Second
}

// Task is a versioned definition of one executable task.
type Task struct {
	Name            string        `json:"name"`
	Version         int           `json:"version"`
	Tier            string        `json:"tier"`
	Scope           string        `json:"scope"`
	OS              []string      `json:"os"`
	Capabilities    []string      `json:"capabilities"`
	ArgsSchema      ArgsSchema    `json:"args_schema"`
	DeadlineSeconds int           `json:"deadline_seconds"`
	BoardPolicy     string        `json:"board_policy"`
	Steps           []Step        `json:"steps"`
	Outputs         []string      `json:"outputs"`
	Retry           RetryPolicy   `json:"retry"`
	ResourceHints   ResourceHints `json:"resource_hints"`
	HIL             *HILTask      `json:"hil,omitempty"`
}

type manifest struct {
	SchemaVersion int    `json:"schema_version"`
	Tasks         []Task `json:"tasks"`
}

// Catalog is immutable after validation. Accessors return copies of task slices.
type Catalog struct {
	digest string
	tasks  map[string]Task
	names  []string
}

// Load validates and returns the definitions compiled into this binary.
func Load() (*Catalog, error) {
	return Parse(embedded.Manifest(), string(embedded.Digest()))
}

// Parse validates a source manifest against the SHA-256 of its canonical JSON.
func Parse(raw []byte, expectedDigest string) (*Catalog, error) {
	canonical, err := CanonicalJSON(raw)
	if err != nil {
		return nil, err
	}
	digest, err := parseDigest(expectedDigest)
	if err != nil {
		return nil, err
	}
	sum := sha256.Sum256(canonical)
	if hex.EncodeToString(sum[:]) != digest {
		return nil, ErrDigestMismatch
	}
	dec := json.NewDecoder(bytes.NewReader(raw))
	dec.DisallowUnknownFields()
	var source manifest
	if err := dec.Decode(&source); err != nil {
		return nil, fmt.Errorf("%w: %v", ErrInvalidCatalog, err)
	}
	if err := expectEOF(dec); err != nil {
		return nil, err
	}
	if err := requireManifestFields(raw); err != nil {
		return nil, err
	}
	if source.SchemaVersion != SchemaVersion {
		return nil, fmt.Errorf("%w: unsupported schema version %d", ErrInvalidCatalog, source.SchemaVersion)
	}
	if len(source.Tasks) == 0 {
		return nil, fmt.Errorf("%w: no tasks", ErrInvalidCatalog)
	}
	c := &Catalog{digest: digest, tasks: make(map[string]Task, len(source.Tasks))}
	for _, task := range source.Tasks {
		if err := ValidateTask(task); err != nil {
			return nil, err
		}
		if err := ValidateTaskDispatch(task); err != nil {
			return nil, err
		}
		if _, found := c.tasks[task.Name]; found {
			return nil, fmt.Errorf("%w: duplicate task %q", ErrInvalidCatalog, task.Name)
		}
		c.tasks[task.Name] = cloneTask(task)
		c.names = append(c.names, task.Name)
	}
	return c, nil
}

// CanonicalJSON encodes JSON with sorted object keys and no whitespace.
func CanonicalJSON(raw []byte) ([]byte, error) {
	dec := json.NewDecoder(bytes.NewReader(raw))
	dec.UseNumber()
	if err := inspectJSONValue(dec); err != nil {
		return nil, fmt.Errorf("%w: %v", ErrInvalidCatalog, err)
	}
	if err := expectEOF(dec); err != nil {
		return nil, err
	}
	dec = json.NewDecoder(bytes.NewReader(raw))
	dec.UseNumber()
	var value any
	if err := dec.Decode(&value); err != nil {
		return nil, fmt.Errorf("%w: %v", ErrInvalidCatalog, err)
	}
	canonical, err := json.Marshal(value)
	if err != nil {
		return nil, fmt.Errorf("%w: %v", ErrInvalidCatalog, err)
	}
	return canonical, nil
}

// VerifyCheckout requires the current checkout to carry the embedded catalog.
func VerifyCheckout(root string) (string, error) {
	if root == "" {
		return "", fmt.Errorf("%w: repository root is empty", ErrInvalidCheckout)
	}
	absolute, err := filepath.Abs(root)
	if err != nil {
		return "", fmt.Errorf("%w: %v", ErrInvalidCheckout, err)
	}
	resolved, err := filepath.EvalSymlinks(absolute)
	if err != nil {
		return "", fmt.Errorf("%w: %v", ErrInvalidCheckout, err)
	}
	if _, err := os.Stat(filepath.Join(resolved, ".git")); err != nil {
		return "", fmt.Errorf("%w: missing .git: %v", ErrInvalidCheckout, err)
	}
	base := filepath.Join(resolved, "tools", "ra8ci", "catalog")
	raw, err := os.ReadFile(filepath.Join(base, "tasks.json"))
	if err != nil {
		return "", fmt.Errorf("%w: manifest: %v", ErrInvalidCheckout, err)
	}
	storedDigest, err := os.ReadFile(filepath.Join(base, "sha256.txt"))
	if err != nil {
		return "", fmt.Errorf("%w: digest: %v", ErrInvalidCheckout, err)
	}
	checkoutCatalog, err := Parse(raw, string(storedDigest))
	if err != nil {
		return "", err
	}
	embeddedCatalog, err := Load()
	if err != nil {
		return "", err
	}
	if checkoutCatalog.Digest() != embeddedCatalog.Digest() {
		return "", ErrDigestMismatch
	}
	return resolved, nil
}

// Digest returns the canonical JSON digest used to identify this catalog.
func (c *Catalog) Digest() string {
	if c == nil {
		return ""
	}
	return c.digest
}

// Names returns task names in manifest order.
func (c *Catalog) Names() []string {
	if c == nil {
		return nil
	}
	return append([]string(nil), c.names...)
}

// Task returns a copy of a task by its exact semantic name.
func (c *Catalog) Task(name string) (Task, bool) {
	if c == nil {
		return Task{}, false
	}
	task, found := c.tasks[name]
	return cloneTask(task), found
}

// IsSafeLocal reports whether the task may run without the server.
func (t Task) IsSafeLocal() bool {
	return t.Scope == "safe-local-read-only" || t.Scope == "safe-local-write-working-tree"
}

// SupportsOS reports whether this exact host OS is declared for the task.
func (t Task) SupportsOS(goos string) bool {
	for _, candidate := range t.OS {
		if candidate == goos {
			return true
		}
	}
	return false
}

// ValidateArguments rejects arguments not yet represented by a reviewed schema.
func (t Task) ValidateArguments(args []string) error {
	if len(args) != 0 {
		return fmt.Errorf("%w: task %q accepts no arguments", ErrInvalidCatalog, t.Name)
	}
	return nil
}

// ValidateTask rejects malformed or unsupported task behavior before execution.
func ValidateTask(task Task) error {
	if !validName(task.Name) || task.Version < 1 {
		return fmt.Errorf("%w: invalid task identity", ErrInvalidCatalog)
	}
	if task.Tier != "required" && task.Tier != "optional" && task.Tier != "nightly" {
		return fmt.Errorf("%w: invalid tier for %q", ErrInvalidCatalog, task.Name)
	}
	if task.Scope != "safe-local-read-only" && task.Scope != "safe-local-write-working-tree" &&
		task.Scope != "runner" && task.Scope != "linux-vm" && task.Scope != "windows-vm" && task.Scope != "hil" {
		return fmt.Errorf("%w: invalid scope for %q", ErrInvalidCatalog, task.Name)
	}
	if len(task.OS) == 0 || task.DeadlineSeconds < 1 || task.DeadlineSeconds > 86400 {
		return fmt.Errorf("%w: missing OS or invalid deadline for %q", ErrInvalidCatalog, task.Name)
	}
	seenOS := make(map[string]bool, len(task.OS))
	for _, goos := range task.OS {
		if (goos != "linux" && goos != "windows") || seenOS[goos] {
			return fmt.Errorf("%w: invalid OS for %q", ErrInvalidCatalog, task.Name)
		}
		seenOS[goos] = true
	}
	if task.Retry.MaxAttempts != 1 || len(task.Outputs) != 0 ||
		len(task.Capabilities) != 0 || len(task.ArgsSchema.Positional) != 0 || len(task.ArgsSchema.Flags) != 0 {
		return fmt.Errorf("%w: unsupported v1 behavior for %q", ErrInvalidCatalog, task.Name)
	}
	if task.Scope == "hil" {
		if task.BoardPolicy != "exclusive" || task.HIL == nil || validateHILTask(*task.HIL, task.Steps) != nil {
			return fmt.Errorf("%w: HIL task %q lacks a valid exclusive-board contract", ErrInvalidCatalog, task.Name)
		}
	} else if task.BoardPolicy != "none" || task.HIL != nil {
		return fmt.Errorf("%w: non-HIL task %q declares board behavior", ErrInvalidCatalog, task.Name)
	}
	if task.ResourceHints.CPU < 0 || task.ResourceHints.RAMBytes < 0 || task.ResourceHints.MaxParallelism < 0 {
		return fmt.Errorf("%w: negative resource hint for %q", ErrInvalidCatalog, task.Name)
	}
	if len(task.Steps) == 0 {
		return fmt.Errorf("%w: no steps for %q", ErrInvalidCatalog, task.Name)
	}
	seenSteps := make(map[string]bool, len(task.Steps))
	for _, step := range task.Steps {
		if !validName(step.Name) || step.Program == "" || strings.ContainsAny(step.Program, "\x00\r\n") || seenSteps[step.Name] {
			return fmt.Errorf("%w: invalid step for %q", ErrInvalidCatalog, task.Name)
		}
		seenSteps[step.Name] = true
		for _, arg := range step.Args {
			if strings.ContainsRune(arg, '\x00') {
				return fmt.Errorf("%w: NUL argument for %q", ErrInvalidCatalog, task.Name)
			}
		}
	}
	return nil
}

// maxHandoffBoundSeconds caps either declared handoff bound. It matches the
// estimator's own ceiling on a bound (board.MaxHandoffBound), stated here in
// seconds so this package keeps no dependency on the board state machine.
const maxHandoffBoundSeconds = 3600

func validateHILTask(hil HILTask, steps []Step) error {
	if err := ValidateHILTaskMetadata(hil); err != nil {
		return err
	}
	for _, step := range steps {
		if step.Name == hil.ObservationStep {
			return nil
		}
	}
	return ErrInvalidCatalog
}

// ValidateHILTaskMetadata validates catalog-owned HIL workload identity.
func ValidateHILTaskMetadata(hil HILTask) error {
	if !validName(hil.BoardID) || hil.BoardModel == "" || strings.TrimSpace(hil.BoardModel) != hil.BoardModel ||
		len(hil.BoardModel) > 128 || !validHILManifestPath(hil.ManifestPath) ||
		!validName(hil.ProgramFamily) || !validName(hil.ObservationStep) ||
		hil.FlashRestoreSeconds < 1 || hil.FlashRestoreSeconds > 3600 ||
		(hil.TimeoutDeclared && (hil.TimeoutSeconds < 1 || hil.TimeoutSeconds > 3600)) ||
		(!hil.TimeoutDeclared && hil.TimeoutSeconds != 0) ||
		hil.SafetyMaximumSeconds < 0 || hil.SafetyMaximumSeconds > 3600 {
		return ErrInvalidCatalog
	}
	if err := validateHandoffBounds(hil); err != nil {
		return err
	}
	fallback := 30
	if hil.TimeoutDeclared {
		fallback = hil.TimeoutSeconds
	}
	if hil.SafetyMaximumSeconds > 0 && hil.SafetyMaximumSeconds < fallback {
		return ErrInvalidCatalog
	}
	switch hil.Mode {
	case "alive", "uart_scrape", "rtt_scrape", "jlink_memprobe", "hil_eth_tcp", "c6_camera_livestream":
		return nil
	default:
		return ErrInvalidCatalog
	}
}

// validateHandoffBounds judges the reviewed cost of giving the board up.
//
// The two halves travel together. Half a bound is worse than none: the sum is
// what a handoff ETA is floored by, so a task declaring only its safe step
// would quote a target that omits the restore it always pays, and one
// declaring only its restore probe would quote a target that assumes it can
// be interrupted anywhere.
func validateHandoffBounds(hil HILTask) error {
	if hil.HandoffSafeStepSeconds == 0 && hil.HandoffRestoreProbeSeconds == 0 {
		return nil
	}
	if hil.HandoffSafeStepSeconds < 1 || hil.HandoffSafeStepSeconds > maxHandoffBoundSeconds ||
		hil.HandoffRestoreProbeSeconds < 1 || hil.HandoffRestoreProbeSeconds > maxHandoffBoundSeconds {
		return ErrInvalidCatalog
	}
	// An indivisible step cannot outlast the cap on the whole attempt it
	// runs inside: such a task could be asked to yield in the middle of a
	// step the safety maximum would already have killed.
	if hil.SafetyMaximumSeconds > 0 && hil.HandoffSafeStepSeconds > hil.SafetyMaximumSeconds {
		return ErrInvalidCatalog
	}
	return nil
}

func validHILManifestPath(manifest string) bool {
	return strings.HasPrefix(manifest, "examples/") && strings.HasSuffix(manifest, "/hil.conf") &&
		!strings.Contains(manifest, "\\") && !strings.Contains(manifest, "..") &&
		path.Clean(manifest) == manifest && len(manifest) <= 512
}

// SupportsCurrentOS is a convenience for CLI admission checks.
func (t Task) SupportsCurrentOS() bool {
	return t.SupportsOS(runtime.GOOS)
}

func cloneTask(task Task) Task {
	task.OS = append([]string(nil), task.OS...)
	task.Capabilities = append([]string(nil), task.Capabilities...)
	task.ArgsSchema.Positional = append([]string(nil), task.ArgsSchema.Positional...)
	task.ArgsSchema.Flags = append([]string(nil), task.ArgsSchema.Flags...)
	task.Outputs = append([]string(nil), task.Outputs...)
	task.Steps = append([]Step(nil), task.Steps...)
	for i := range task.Steps {
		task.Steps[i].Args = append([]string(nil), task.Steps[i].Args...)
	}
	if task.HIL != nil {
		hil := *task.HIL
		task.HIL = &hil
	}
	return task
}

func validName(value string) bool {
	if value == "" {
		return false
	}
	for _, char := range value {
		if (char < 'a' || char > 'z') && (char < '0' || char > '9') && char != '-' {
			return false
		}
	}
	return true
}

func parseDigest(value string) (string, error) {
	value = strings.TrimSpace(value)
	if len(value) != sha256.Size*2 {
		return "", fmt.Errorf("%w: SHA-256 must be 64 lowercase hex characters", ErrInvalidCatalog)
	}
	if _, err := hex.DecodeString(value); err != nil || strings.ToLower(value) != value {
		return "", fmt.Errorf("%w: invalid SHA-256", ErrInvalidCatalog)
	}
	return value, nil
}

func expectEOF(dec *json.Decoder) error {
	var trailing any
	if err := dec.Decode(&trailing); err == io.EOF {
		return nil
	} else if err != nil {
		return fmt.Errorf("%w: %v", ErrInvalidCatalog, err)
	}
	return fmt.Errorf("%w: trailing JSON value", ErrInvalidCatalog)
}

func inspectJSONValue(dec *json.Decoder) error {
	token, err := dec.Token()
	if err != nil {
		return err
	}
	delimiter, ok := token.(json.Delim)
	if !ok {
		return nil
	}
	switch delimiter {
	case '{':
		seen := make(map[string]bool)
		for dec.More() {
			keyToken, err := dec.Token()
			if err != nil {
				return err
			}
			key, ok := keyToken.(string)
			if !ok || seen[key] {
				return fmt.Errorf("duplicate or invalid JSON key %q", key)
			}
			seen[key] = true
			if err := inspectJSONValue(dec); err != nil {
				return err
			}
		}
	case '[':
		for dec.More() {
			if err := inspectJSONValue(dec); err != nil {
				return err
			}
		}
	default:
		return fmt.Errorf("unexpected JSON delimiter %q", delimiter)
	}
	end, err := dec.Token()
	if err != nil {
		return err
	}
	if end != json.Delim(delimiter+2) {
		return fmt.Errorf("mismatched JSON delimiter %q", end)
	}
	return nil
}

func requireManifestFields(raw []byte) error {
	var top map[string]json.RawMessage
	if err := json.Unmarshal(raw, &top); err != nil {
		return fmt.Errorf("%w: %v", ErrInvalidCatalog, err)
	}
	if err := requireFields(top, "schema_version", "tasks"); err != nil {
		return err
	}
	if err := requireKind(top["tasks"], '['); err != nil {
		return err
	}
	var tasks []map[string]json.RawMessage
	if err := json.Unmarshal(top["tasks"], &tasks); err != nil {
		return fmt.Errorf("%w: tasks: %v", ErrInvalidCatalog, err)
	}
	for _, task := range tasks {
		if err := requireFields(task, "name", "version", "tier", "scope", "os", "capabilities", "args_schema", "deadline_seconds", "board_policy", "steps", "outputs", "retry", "resource_hints"); err != nil {
			return err
		}
		for _, key := range []string{"os", "capabilities", "steps", "outputs"} {
			if err := requireKind(task[key], '['); err != nil {
				return err
			}
		}
		for _, key := range []string{"args_schema", "retry", "resource_hints"} {
			if err := requireKind(task[key], '{'); err != nil {
				return err
			}
		}
		var args map[string]json.RawMessage
		if err := json.Unmarshal(task["args_schema"], &args); err != nil {
			return fmt.Errorf("%w: args_schema: %v", ErrInvalidCatalog, err)
		}
		if err := requireFields(args, "positional", "flags"); err != nil {
			return err
		}
		for _, key := range []string{"positional", "flags"} {
			if err := requireKind(args[key], '['); err != nil {
				return err
			}
		}
		var retry map[string]json.RawMessage
		if err := json.Unmarshal(task["retry"], &retry); err != nil {
			return fmt.Errorf("%w: retry: %v", ErrInvalidCatalog, err)
		}
		if err := requireFields(retry, "max_attempts"); err != nil {
			return err
		}
		var steps []map[string]json.RawMessage
		if err := json.Unmarshal(task["steps"], &steps); err != nil {
			return fmt.Errorf("%w: steps: %v", ErrInvalidCatalog, err)
		}
		for _, step := range steps {
			if err := requireFields(step, "name", "program", "args"); err != nil {
				return err
			}
			if err := requireKind(step["args"], '['); err != nil {
				return err
			}
		}
	}
	return nil
}

func requireFields(fields map[string]json.RawMessage, names ...string) error {
	for _, name := range names {
		if _, found := fields[name]; !found {
			return fmt.Errorf("%w: missing field %q", ErrInvalidCatalog, name)
		}
	}
	return nil
}

func requireKind(raw json.RawMessage, kind byte) error {
	trimmed := bytes.TrimSpace(raw)
	if len(trimmed) == 0 || trimmed[0] != kind {
		return fmt.Errorf("%w: expected JSON %q", ErrInvalidCatalog, kind)
	}
	return nil
}
