// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"bytes"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
)

// agreeingCommit is one pull request's comparison where the single covered
// task ran here and Actions concluded the same way. It is the smallest
// document the evidence gate can actually grade, so it is what the tests below
// use when they care about what happens after the grading rather than during
// it.
func agreeingCommit(t *testing.T) string {
	t.Helper()
	task := shadowCompareEnv(t, "build")
	return `{"plane":[{"task":"` + task + `","head_sha":"` + shadowCompareHead + `","observed":"success"}],` +
		`"actions":[{"job":"build","head_sha":"` + shadowCompareHead + `","conclusion":"success"}]}`
}

// The plan is written before the verdict is returned, the same convention
// shadow-compare holds to: a caller reading only the exit status must not be
// able to get a clean one from a plan nobody could read.
func TestEvidenceGateReportsAPlanItCouldNotWrite(t *testing.T) {
	input := `{"threshold":1,"commits":[` + agreeingCommit(t) + `],"required":[]}`

	out := &halting{budget: 0}
	err := githubEvidenceGate(strings.NewReader(input), out)
	if err == nil {
		t.Fatal("a plan that could not be written was reported clean")
	}
	if !strings.Contains(err.Error(), "write evidence gate plan") {
		t.Fatalf("err = %v; want the write named", err)
	}
	if out.taken.Len() != 0 {
		t.Fatalf("wrote %q against a budget of nothing", out.taken.String())
	}
}

// Both commands read their document under a limit, and a document over it is
// refused rather than parsed. The bound is on the read itself: whatever is
// piped in must not be able to spend the box's memory on the way to being
// refused.
func TestTheGateCommandsRefuseADocumentLargerThanTheyWillRead(t *testing.T) {
	t.Run("evidence gate", func(t *testing.T) {
		commit := agreeingCommit(t)
		var document strings.Builder
		document.WriteString(`{"threshold":1,"commits":[` + commit)
		for document.Len() <= maxShadowEvidenceBytes {
			document.WriteString("," + commit)
		}
		document.WriteString(`],"required":[]}`)

		var out bytes.Buffer
		err := githubEvidenceGate(strings.NewReader(document.String()), &out)
		if err == nil {
			t.Fatal("an oversized gate document was accepted")
		}
		if !strings.Contains(err.Error(), "read evidence gate document") {
			t.Fatalf("err = %v; want the read named", err)
		}
		if out.Len() != 0 {
			t.Fatalf("a refused document wrote a plan: %q", out.String())
		}
	})

	t.Run("required checks", func(t *testing.T) {
		shadowCompareEnv(t, "build")
		var document strings.Builder
		document.WriteString(`{"required":["ci/one"`)
		for document.Len() <= maxRequiredCheckBytes {
			document.WriteString(`,"ci/one"`)
		}
		document.WriteString(`]}`)

		var out bytes.Buffer
		err := githubRequiredChecks(strings.NewReader(document.String()), &out)
		if err == nil {
			t.Fatal("an oversized required-contexts document was accepted")
		}
		if !strings.Contains(err.Error(), "read the required contexts") {
			t.Fatalf("err = %v; want the read named", err)
		}
		if out.Len() != 0 {
			t.Fatalf("a refused document wrote a plan: %q", out.String())
		}
	})
}

// A correspondence file that is named and absent is a misconfigured
// deployment, not an unconfigured one. Both commands already refuse the
// unconfigured case by telling the operator which variable to set; telling
// them the same thing when the variable IS set would send them to look at the
// one thing that is not wrong.
func TestTheGateCommandsSeparateAMisconfiguredFileFromAnUnsetOne(t *testing.T) {
	cases := map[string]func(string, *bytes.Buffer) error{
		"evidence gate": func(in string, out *bytes.Buffer) error {
			return githubEvidenceGate(strings.NewReader(in), out)
		},
		"required checks": func(in string, out *bytes.Buffer) error {
			return githubRequiredChecks(strings.NewReader(in), out)
		},
	}
	for name, call := range cases {
		t.Run(name, func(t *testing.T) {
			t.Setenv(github.EnvCheckRunMode, "shadow")
			t.Setenv(github.EnvShadowCorrespondenceFile, t.TempDir()+"/absent.json")

			var out bytes.Buffer
			err := call(`{"threshold":1,"commits":[],"required":[]}`, &out)
			if err == nil {
				t.Fatal("an absent correspondence file was accepted")
			}
			if strings.Contains(err.Error(), "is not configured") {
				t.Fatalf("a named but unreadable file was reported as unconfigured: %v", err)
			}
			if out.Len() != 0 {
				t.Fatalf("a refused command wrote a plan: %q", out.String())
			}
		})
	}
}
