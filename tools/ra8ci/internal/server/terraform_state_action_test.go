// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"bufio"
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"slices"
	"strings"
	"testing"
)

// callerChosenMethods are shapes a caller can put on the request line that the
// Terraform state route will accept, since it is registered without a method.
func callerChosenMethods() []string {
	return []string{
		"UNLOCK!#$%&'*+-.^_`|~",
		"DROP",
		"lock",
		"get",
		"Post",
		"",
		strings.Repeat("A", 2000),
		"terraform_state.get",
	}
}

func auditedActionNames() []string {
	names := []string{terraformStateActionPrefix + "unsupported"}
	for _, method := range terraformStateMethods {
		names = append(names, terraformStateActionPrefix+strings.ToLower(method))
	}
	return names
}

func TestEachImplementedMethodNamesItselfInTheAuditTrail(t *testing.T) {
	for method, want := range map[string]string{
		http.MethodGet:        "terraform_state.get",
		http.MethodPost:       "terraform_state.post",
		http.MethodDelete:     "terraform_state.delete",
		terraformLockMethod:   "terraform_state.lock",
		terraformUnlockMethod: "terraform_state.unlock",
	} {
		if got := terraformStateAction(method); got != want {
			t.Errorf("%s audited as %q, want %q", method, got, want)
		}
	}
}

func TestACallerChosenMethodIsAuditedWithoutBeingQuoted(t *testing.T) {
	for _, method := range callerChosenMethods() {
		got := terraformStateAction(method)
		if got != terraformStateActionPrefix+"unsupported" {
			t.Errorf("method %.40q audited as %q, want the unsupported name", method, got)
		}
		// The denial is written before the request is understood, so no part
		// of what the caller wrote may survive into the record.
		if method != "" && strings.Contains(got, method) {
			t.Errorf("audited action %q carries the caller's method %.40q", got, method)
		}
	}
}

func TestACaseVariantOfAnImplementedMethodIsNotTreatedAsOne(t *testing.T) {
	// net/http does not canonicalise the method, and the handler's switch
	// compares it exactly, so "get" is refused by the route and must not be
	// audited as though the caller had asked for a read.
	for _, method := range []string{"get", "Delete", "unlock"} {
		if got := terraformStateAction(method); got != terraformStateActionPrefix+"unsupported" {
			t.Errorf("%q audited as %q, want the unsupported name", method, got)
		}
	}
}

func TestTheAuditedActionIsAlwaysOneTheServerChose(t *testing.T) {
	allowed := auditedActionNames()
	methods := append(callerChosenMethods(), terraformStateMethods...)
	for _, method := range methods {
		if got := terraformStateAction(method); !slices.Contains(allowed, got) {
			t.Errorf("method %.40q produced %q, which is not one of %v", method, got, allowed)
		}
	}
}

func TestTheAuditedActionDoesNotGrowWithTheMethod(t *testing.T) {
	longest := 0
	for _, name := range auditedActionNames() {
		longest = max(longest, len(name))
	}
	for _, method := range []string{strings.Repeat("A", 2000), strings.Repeat("!", 64)} {
		if got := terraformStateAction(method); len(got) > longest {
			t.Errorf("a %d character method produced a %d character action, want at most %d", len(method), len(got), longest)
		}
	}
}

func TestTheAllowHeaderOffersExactlyTheMethodsTheBackendImplements(t *testing.T) {
	if terraformStateAllow != "GET, POST, DELETE, LOCK, UNLOCK" {
		t.Fatalf("Allow offers %q", terraformStateAllow)
	}
	for _, method := range terraformStateMethods {
		if !strings.Contains(terraformStateAllow, method) {
			t.Errorf("Allow omits the implemented method %q", method)
		}
	}
}

func TestTheLockMethodsAreTheOnesTerraformSpeaks(t *testing.T) {
	if terraformLockMethod != "LOCK" || terraformUnlockMethod != "UNLOCK" {
		t.Fatalf("lock methods are %q/%q, which is not what a Terraform HTTP backend sends", terraformLockMethod, terraformUnlockMethod)
	}
}

// TestTheServerDeliversACallerChosenMethodToTheHandler pins the premise the
// rule above rests on: a route registered without a method receives whatever
// token the caller wrote, verbatim. If net/http ever refuses these, the rule
// is still harmless, but this test says so rather than leaving it assumed.
func TestTheServerDeliversACallerChosenMethodToTheHandler(t *testing.T) {
	seen := make(chan string, 1)
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/terraform/runner-states/{reservation_id}", func(w http.ResponseWriter, r *http.Request) {
		seen <- r.Method
		w.WriteHeader(http.StatusNoContent)
	})
	server := httptest.NewServer(mux)
	defer server.Close()

	for _, method := range []string{"UNLOCK!#$%&'*+-.^_`|~", strings.Repeat("A", 2000), "get"} {
		connection, err := net.Dial("tcp", strings.TrimPrefix(server.URL, "http://"))
		if err != nil {
			t.Fatalf("dial: %v", err)
		}
		request := fmt.Sprintf("%s /v1/terraform/runner-states/rv-1 HTTP/1.1\r\nHost: ra8ci\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", method)
		if _, err := connection.Write([]byte(request)); err != nil {
			connection.Close()
			t.Fatalf("write request line: %v", err)
		}
		status, err := bufio.NewReader(connection).ReadString('\n')
		connection.Close()
		if err != nil {
			t.Fatalf("read status line: %v", err)
		}
		if !strings.Contains(status, "204") {
			t.Fatalf("method %.40q answered %q, want the handler to have run", method, strings.TrimSpace(status))
		}
		if delivered := <-seen; delivered != method {
			t.Fatalf("handler saw %.40q, want the caller's %.40q", delivered, method)
		}
		if got := terraformStateAction(method); got != terraformStateActionPrefix+"unsupported" {
			t.Fatalf("a method the handler really receives was audited as %q", got)
		}
	}
}
