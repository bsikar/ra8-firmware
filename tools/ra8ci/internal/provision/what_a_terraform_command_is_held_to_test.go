// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// Every Terraform command the control plane runs goes through one session and
// one run(). Until now nothing held either: the session commands were
// unreachable without a Terraform install and a Proxmox lab.
//
// They are reachable here. PR #2050 showed the runtime constructor runs
// against a shell script whose digest is pinned the same way the real
// executable's is; the session is the same trick one level up, with a
// TerraformSession built directly rather than through WithSession, which
// needs a live Vault. The script records the argument vector, the working
// directory and the environment it was handed, so what the plane actually
// runs can be asserted rather than assumed.

// commandRecorder is a stand-in Terraform that answers the pinned version
// probe, records how it was called, honours -out= by writing a plan, and then
// does whatever tail the test asked for.
type commandRecorder struct {
	runtime *TerraformRuntime
	config  TerraformConfig
	record  string
}

func terraformStandIn(t *testing.T, tail string) *commandRecorder {
	t.Helper()
	config := trustedRuntimeConfig(t)
	record := filepath.Join(t.TempDir(), "record")
	if err := os.MkdirAll(record, 0o700); err != nil {
		t.Fatal(err)
	}
	body := "#!/bin/sh\n" +
		"if [ \"$1\" = \"version\" ]; then printf '%s' '{\"terraform_version\":\"" + trustedProbeVersion + "\"}'; exit 0; fi\n" +
		"pwd > " + record + "/cwd\n" +
		": > " + record + "/args\n" +
		"for a in \"$@\"; do printf '%s\\n' \"$a\" >> " + record + "/args; done\n" +
		"env > " + record + "/env\n" +
		"for a in \"$@\"; do case \"$a\" in -out=*) printf 'PLAN' > \"${a#-out=}\" ;; esac; done\n" +
		tail + "\n"
	binary, digest := trustedProbeScript(t, filepath.Join(t.TempDir(), "bin"), body)
	config.BinaryPath = binary
	config.BinarySHA256 = digest
	return &commandRecorder{runtime: trustedRuntime(t, config), config: config, record: record}
}

// session builds one reservation session the way WithSession would, without
// the Vault round trip that only a live server can answer.
func (recorder *commandRecorder) session(t *testing.T) *TerraformSession {
	t.Helper()
	reservationID := mustProvisionID(t)
	workspace := filepath.Join(recorder.config.StateDirectory, reservationID)
	if err := secureDirectory(workspace); err != nil {
		t.Fatal(err)
	}
	return &TerraformSession{
		runtime:       recorder.runtime,
		reservationID: reservationID,
		workspace:     workspace,
		environment:   []string{"TF_IN_AUTOMATION=1", "TF_DATA_DIR=" + filepath.Join(workspace, "tfdata")},
	}
}

func (recorder *commandRecorder) ran(t *testing.T) bool {
	t.Helper()
	_, err := os.Lstat(filepath.Join(recorder.record, "args"))
	return err == nil
}

func (recorder *commandRecorder) arguments(t *testing.T) []string {
	t.Helper()
	body, err := os.ReadFile(filepath.Join(recorder.record, "args"))
	if err != nil {
		t.Fatalf("the stand-in Terraform was never run: %v", err)
	}
	return strings.Split(strings.TrimRight(string(body), "\n"), "\n")
}

func (recorder *commandRecorder) recorded(t *testing.T, name string) string {
	t.Helper()
	body, err := os.ReadFile(filepath.Join(recorder.record, name))
	if err != nil {
		t.Fatalf("the stand-in Terraform recorded no %s: %v", name, err)
	}
	return strings.TrimRight(string(body), "\n")
}

// TestASessionIsJudgedBeforeEveryCommand pins the guard each of the four
// commands opens with. A session missing any part of its identity is not a
// session that can be re-driven with care; it is one that would run Terraform
// against an unbound backend, so it is refused rather than repaired.
func TestASessionIsJudgedBeforeEveryCommand(t *testing.T) {
	recorder := terraformStandIn(t, "exit 0")
	for name, break_ := range map[string]func(*TerraformSession){
		"no runtime":           func(s *TerraformSession) { s.runtime = nil },
		"no reservation":       func(s *TerraformSession) { s.reservationID = "" },
		"reservation not a v7": func(s *TerraformSession) { s.reservationID = "runner-7" },
		"no workspace":         func(s *TerraformSession) { s.workspace = "" },
		"no environment":       func(s *TerraformSession) { s.environment = nil },
		"an empty environment": func(s *TerraformSession) { s.environment = []string{} },
	} {
		t.Run(name, func(t *testing.T) {
			session := recorder.session(t)
			break_(session)
			if err := session.Init(context.Background()); err == nil ||
				err.Error() != "invalid Terraform session" {
				t.Fatalf("refusal %v is not the session refusal", err)
			}
			if recorder.ran(t) {
				t.Fatal("a refused session still ran Terraform")
			}
		})
	}
	t.Run("a nil session", func(t *testing.T) {
		var session *TerraformSession
		if err := session.Init(context.Background()); err == nil {
			t.Fatal("a nil session accepted a command")
		}
	})
}

// TestACancelledContextStopsACommandBeforeItStarts pins that the session
// hands back the context's own error rather than a refusal of its own. A
// cancelled operation is not a malformed one and the caller must be able to
// tell them apart.
func TestACancelledContextStopsACommandBeforeItStarts(t *testing.T) {
	recorder := terraformStandIn(t, "exit 0")
	session := recorder.session(t)
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if err := session.Init(ctx); err != context.Canceled {
		t.Fatalf("refusal %v is not the context's own error", err)
	}
	if recorder.ran(t) {
		t.Fatal("a cancelled command still ran Terraform")
	}
}

// TestInitRunsThePinnedCommandInTheSourceEnvironment pins what the plane
// actually executes. The argument vector is fixed, the working directory is
// the reviewed source environment rather than the reservation workspace, and
// the child is handed the session environment and nothing else, which is what
// keeps an unrelated credential in this process out of a provider plugin.
func TestInitRunsThePinnedCommandInTheSourceEnvironment(t *testing.T) {
	recorder := terraformStandIn(t, "exit 0")
	session := recorder.session(t)
	if err := session.Init(context.Background()); err != nil {
		t.Fatal(err)
	}
	want := []string{"init", "-input=false", "-lockfile=readonly", "-no-color"}
	got := recorder.arguments(t)
	if strings.Join(got, " ") != strings.Join(want, " ") {
		t.Fatalf("init ran %q want %q", got, want)
	}
	if cwd := recorder.recorded(t, "cwd"); cwd != recorder.config.EnvironmentDirectory {
		t.Fatalf("init ran in %q want the source environment %q", cwd, recorder.config.EnvironmentDirectory)
	}
	// The stand-in is a shell script and /bin/sh exports PWD into its own
	// environment before the body runs, so that one entry is the shell's
	// rather than the plane's. Everything else the child sees has to be
	// exactly what the session handed it.
	handed := make([]string, 0, len(session.environment))
	for _, entry := range strings.Split(recorder.recorded(t, "env"), "\n") {
		if strings.HasPrefix(entry, "PWD=") {
			continue
		}
		handed = append(handed, entry)
	}
	if len(handed) != len(session.environment) {
		t.Fatalf("the child was handed %d variables, want exactly the session's %d: %q",
			len(handed), len(session.environment), handed)
	}
	for _, entry := range handed {
		if entry != "TF_IN_AUTOMATION=1" && !strings.HasPrefix(entry, "TF_DATA_DIR=") {
			t.Fatalf("the child inherited %q", entry)
		}
	}
}

// TestAFailedCommandIsReportedAsOneToReconcile pins the two outcomes run()
// distinguishes, and the fact that it distinguishes only those two. The
// child's own output is discarded, so a caller cannot be told what Terraform
// hit; what it can be told is whether the deadline was the thing that ended
// the command, because that is the one case where retrying is not obviously
// wrong.
func TestAFailedCommandIsReportedAsOneToReconcile(t *testing.T) {
	recorder := terraformStandIn(t, "exit 1")
	err := recorder.session(t).Init(context.Background())
	if err == nil || err.Error() != "Terraform command failed; reconcile state before retry" {
		t.Fatalf("refusal %v is not the reconcile refusal", err)
	}
}

// TestACommandThatOutlivesItsDeadlineSaysSo pins the other outcome, with the
// timeout at the bottom of bounded policy so the test does not wait on a real
// twenty-minute operation.
func TestACommandThatOutlivesItsDeadlineSaysSo(t *testing.T) {
	config := trustedRuntimeConfig(t)
	config.OperationTimeout = time.Second
	recorder := terraformStandIn(t, "sleep 30")
	recorder.config.OperationTimeout = time.Second
	recorder.runtime = trustedRuntime(t, recorder.config)
	start := time.Now()
	err := recorder.session(t).Init(context.Background())
	if err == nil || err.Error() != "Terraform command exceeded its deadline" {
		t.Fatalf("refusal %v is not the deadline refusal", err)
	}
	if elapsed := time.Since(start); elapsed > 20*time.Second {
		t.Fatalf("the command was not stopped at its deadline, it took %v", elapsed)
	}
	_ = config
}

// TestRunRefusesACommandWithNoArguments pins the last guard in run itself. An
// empty argument vector would execute the pinned binary with no subcommand,
// which is a usage message rather than an operation, reported as a failed
// Terraform command nobody can act on.
func TestRunRefusesACommandWithNoArguments(t *testing.T) {
	recorder := terraformStandIn(t, "exit 0")
	session := recorder.session(t)
	if err := session.run(context.Background(), nil); err == nil ||
		err.Error() != "invalid Terraform command" {
		t.Fatalf("refusal %v is not the command refusal", err)
	}
	if err := session.run(nil, nil, "init"); err == nil ||
		err.Error() != "invalid Terraform command" {
		t.Fatalf("refusal %v is not the command refusal", err)
	}
	if recorder.ran(t) {
		t.Fatal("a refused command still ran Terraform")
	}
}

// TestAPlanIsBoundToOneOperationAndItsOwnDigest pins the whole plan path: the
// operation identifier, the private variable file, the saved plan's mode, and
// the digest the caller records as its apply intent.
func TestAPlanIsBoundToOneOperationAndItsOwnDigest(t *testing.T) {
	recorder := terraformStandIn(t, "exit 0")
	session := recorder.session(t)
	operationID := mustProvisionID(t)
	variableFile := filepath.Join(session.workspace, "runner.tfvars.json")
	if err := os.WriteFile(variableFile, []byte("{}"), 0o600); err != nil {
		t.Fatal(err)
	}
	planFile, digest, err := session.Plan(context.Background(), operationID, variableFile, false)
	if err != nil {
		t.Fatal(err)
	}
	if want := filepath.Join(session.workspace, operationID, "saved.tfplan"); planFile != want {
		t.Fatalf("plan file %q want %q", planFile, want)
	}
	sum := sha256.Sum256([]byte("PLAN"))
	if digest != hex.EncodeToString(sum[:]) {
		t.Fatalf("digest %q is not the saved plan's own digest", digest)
	}
	info, err := os.Lstat(planFile)
	if err != nil {
		t.Fatal(err)
	}
	if info.Mode().Perm() != 0o600 {
		t.Fatalf("saved plan mode %v want 0600", info.Mode().Perm())
	}
	got := strings.Join(recorder.arguments(t), " ")
	want := "plan -input=false -no-color -lock-timeout=30s -var-file=" + variableFile + " -out=" + planFile
	if got != want {
		t.Fatalf("plan ran %q want %q", got, want)
	}
}

// TestADestroyPlanIsTheSameCommandWithOneFlag pins that a destroy is not a
// separate path. It is the same saved-plan mechanism, so it is bound by the
// same digest and applied by the same Apply.
func TestADestroyPlanIsTheSameCommandWithOneFlag(t *testing.T) {
	recorder := terraformStandIn(t, "exit 0")
	session := recorder.session(t)
	variableFile := filepath.Join(session.workspace, "runner.tfvars.json")
	if err := os.WriteFile(variableFile, []byte("{}"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, _, err := session.Plan(context.Background(), mustProvisionID(t), variableFile, true); err != nil {
		t.Fatal(err)
	}
	arguments := recorder.arguments(t)
	if last := arguments[len(arguments)-1]; last != "-destroy" {
		t.Fatalf("a destroy plan ended with %q want -destroy", last)
	}
}

// TestAPlanIsRefusedBeforeTerraformRuns pins every refusal the plan path
// meets ahead of the command, and that none of them leaves a saved plan
// behind for a later Apply to find.
func TestAPlanIsRefusedBeforeTerraformRuns(t *testing.T) {
	t.Run("an operation that is not a v7 identifier", func(t *testing.T) {
		recorder := terraformStandIn(t, "exit 0")
		session := recorder.session(t)
		variableFile := filepath.Join(session.workspace, "runner.tfvars.json")
		if err := os.WriteFile(variableFile, []byte("{}"), 0o600); err != nil {
			t.Fatal(err)
		}
		for _, operationID := range []string{"", "operation-1", strings.ToUpper(mustProvisionID(t))} {
			_, _, err := session.Plan(context.Background(), operationID, variableFile, false)
			if err == nil || err.Error() != "invalid Terraform plan request" {
				t.Fatalf("refusal %v for %q is not the plan refusal", err, operationID)
			}
		}
		if recorder.ran(t) {
			t.Fatal("a refused plan still ran Terraform")
		}
	})
	t.Run("a variable file outside the workspace", func(t *testing.T) {
		recorder := terraformStandIn(t, "exit 0")
		session := recorder.session(t)
		outside := filepath.Join(t.TempDir(), "runner.tfvars.json")
		if err := os.WriteFile(outside, []byte("{}"), 0o600); err != nil {
			t.Fatal(err)
		}
		_, _, err := session.Plan(context.Background(), mustProvisionID(t), outside, false)
		if err == nil || !strings.Contains(err.Error(), "inside its reservation workspace") {
			t.Fatalf("refusal %v does not name the workspace", err)
		}
		if recorder.ran(t) {
			t.Fatal("a refused plan still ran Terraform")
		}
	})
	t.Run("a variable file others can read", func(t *testing.T) {
		recorder := terraformStandIn(t, "exit 0")
		session := recorder.session(t)
		variableFile := filepath.Join(session.workspace, "runner.tfvars.json")
		if err := os.WriteFile(variableFile, []byte("{}"), 0o644); err != nil {
			t.Fatal(err)
		}
		_, _, err := session.Plan(context.Background(), mustProvisionID(t), variableFile, false)
		if err == nil || !strings.Contains(err.Error(), "private, bounded, and regular") {
			t.Fatalf("refusal %v does not name the file policy", err)
		}
		if recorder.ran(t) {
			t.Fatal("a refused plan still ran Terraform")
		}
	})
	t.Run("an operation that already planned", func(t *testing.T) {
		recorder := terraformStandIn(t, "exit 0")
		session := recorder.session(t)
		variableFile := filepath.Join(session.workspace, "runner.tfvars.json")
		if err := os.WriteFile(variableFile, []byte("{}"), 0o600); err != nil {
			t.Fatal(err)
		}
		operationID := mustProvisionID(t)
		if _, _, err := session.Plan(context.Background(), operationID, variableFile, false); err != nil {
			t.Fatal(err)
		}
		_, _, err := session.Plan(context.Background(), operationID, variableFile, false)
		if err == nil || err.Error() != "create private Terraform plan file" {
			t.Fatalf("refusal %v is not the second-plan refusal", err)
		}
	})
}

// TestAFailedPlanLeavesNoSavedPlanBehind pins the cleanup. A saved plan is an
// apply intent the caller may record and act on later, so one left behind by
// a plan that failed is a plan nobody approved sitting exactly where Apply
// looks.
func TestAFailedPlanLeavesNoSavedPlanBehind(t *testing.T) {
	recorder := terraformStandIn(t, "exit 1")
	session := recorder.session(t)
	variableFile := filepath.Join(session.workspace, "runner.tfvars.json")
	if err := os.WriteFile(variableFile, []byte("{}"), 0o600); err != nil {
		t.Fatal(err)
	}
	operationID := mustProvisionID(t)
	if _, _, err := session.Plan(context.Background(), operationID, variableFile, false); err == nil {
		t.Fatal("a failed plan was reported as a plan")
	}
	planFile := filepath.Join(session.workspace, operationID, "saved.tfplan")
	if _, err := os.Lstat(planFile); !os.IsNotExist(err) {
		t.Fatalf("a failed plan left %s behind, stat error %v", planFile, err)
	}
}

// TestApplyRunsOnlyThePlanItWasApprovedFor pins the apply intent. The digest
// is the whole mechanism: a saved plan that changed between plan and apply is
// a different operation wearing the approved one's name.
func TestApplyRunsOnlyThePlanItWasApprovedFor(t *testing.T) {
	recorder := terraformStandIn(t, "exit 0")
	session := recorder.session(t)
	variableFile := filepath.Join(session.workspace, "runner.tfvars.json")
	if err := os.WriteFile(variableFile, []byte("{}"), 0o600); err != nil {
		t.Fatal(err)
	}
	planFile, digest, err := session.Plan(context.Background(), mustProvisionID(t), variableFile, false)
	if err != nil {
		t.Fatal(err)
	}
	if err := session.Apply(context.Background(), planFile, digest); err != nil {
		t.Fatal(err)
	}
	got := strings.Join(recorder.arguments(t), " ")
	if want := "apply -input=false -no-color " + planFile; got != want {
		t.Fatalf("apply ran %q want %q", got, want)
	}
	t.Run("a plan that changed after it was approved", func(t *testing.T) {
		if err := os.WriteFile(planFile, []byte("PLAN2"), 0o600); err != nil {
			t.Fatal(err)
		}
		if err := session.Apply(context.Background(), planFile, digest); err == nil {
			t.Fatal("applied a plan that no longer hashes to the approved digest")
		}
	})
	t.Run("a plan outside the workspace", func(t *testing.T) {
		outside := filepath.Join(t.TempDir(), "saved.tfplan")
		if err := os.WriteFile(outside, []byte("PLAN"), 0o600); err != nil {
			t.Fatal(err)
		}
		err := session.Apply(context.Background(), outside, digest)
		if err == nil || !strings.Contains(err.Error(), "inside its reservation workspace") {
			t.Fatalf("refusal %v does not name the workspace", err)
		}
	})
}

// TestStatePullHandsBackExactlyWhatTerraformWrote pins the one command that
// returns bytes, and that a failed pull hands back nothing rather than a
// partial state a caller might reconcile against.
func TestStatePullHandsBackExactlyWhatTerraformWrote(t *testing.T) {
	recorder := terraformStandIn(t, "printf '%s' '{\"version\":4,\"serial\":7}'")
	state, err := recorder.session(t).StatePull(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if string(state) != `{"version":4,"serial":7}` {
		t.Fatalf("state pull returned %q", string(state))
	}
	if got := strings.Join(recorder.arguments(t), " "); got != "state pull" {
		t.Fatalf("state pull ran %q want \"state pull\"", got)
	}
	failing := terraformStandIn(t, "printf '%s' 'half a state'; exit 1")
	partial, err := failing.session(t).StatePull(context.Background())
	if err == nil {
		t.Fatal("a failed state pull was reported as state")
	}
	if len(partial) != 0 {
		t.Fatalf("a failed state pull returned %d bytes", len(partial))
	}
}
