package store

import (
	"slices"
	"testing"
)

// everyGrantRole is every role api_grants.role admits after migration 0011.
// It is written out rather than derived so a role added to the schema without
// a decision here shows up as a compile-time edit to this list.
var everyGrantRole = []string{
	"observer", "submitter", "board_human", "operator",
	"board_agent", "agent_executor", "terraform_state",
}

func TestClientAPIGrantAllowedPairsKindWithRole(t *testing.T) {
	allowed := map[string]map[string][]string{
		PermissionRead: {
			KindHuman:     {"observer", "submitter", "operator"},
			KindGitHubApp: {"observer", "submitter", "operator"},
		},
		PermissionSubmit: {
			KindHuman:     {"submitter", "operator"},
			KindGitHubApp: {"submitter", "operator"},
		},
		PermissionTerraformState: {
			KindHuman:     {"terraform_state"},
			KindGitHubApp: {"terraform_state"},
		},
	}
	for _, permission := range APIPermissions() {
		for _, kind := range PrincipalKinds() {
			for _, role := range everyGrantRole {
				want := slices.Contains(allowed[permission][kind], role)
				if got := ClientAPIGrantAllowed(kind, role, permission); got != want {
					t.Fatalf("ClientAPIGrantAllowed(%q,%q,%q)=%v want %v", kind, role, permission, got, want)
				}
			}
		}
	}
}

// The split this file exists for: an agent certificate is provisioned onto a
// runner VM, the least trusted host in the system. It claims work and posts
// evidence over the agent protocol. It must not spend a client grant on the
// client surface, and AuthorizeBoardPeer deliberately admits kind='agent' with
// role='submitter' for the board, so holding that pair is not hypothetical.
func TestClientSurfaceRefusesRunnerCertificates(t *testing.T) {
	for _, kind := range []string{KindAgent, KindBoardAgent} {
		for _, permission := range APIPermissions() {
			for _, role := range everyGrantRole {
				if ClientAPIGrantAllowed(kind, role, permission) {
					t.Fatalf("client surface admitted kind %q with role %q for %q", kind, role, permission)
				}
			}
		}
	}
}

func TestAPIPermissionsCoverTheRuleTable(t *testing.T) {
	listed := APIPermissions()
	if len(listed) != len(clientAPIRules) {
		t.Fatalf("APIPermissions has %d entries, rule table has %d", len(listed), len(clientAPIRules))
	}
	for permission := range clientAPIRules {
		if !slices.Contains(listed, permission) {
			t.Fatalf("permission %q is in the rule table but not in APIPermissions", permission)
		}
		if !ValidAPIPermission(permission) {
			t.Fatalf("permission %q is in the rule table but not valid", permission)
		}
		if len(GrantRolesForPermission(permission)) == 0 || len(PrincipalKindsForPermission(permission)) == 0 {
			t.Fatalf("permission %q has an empty rule", permission)
		}
	}
}

// A grant role is not a permission. The old signature took the caller's word
// for a string named role; the two vocabularies are now separate and a value
// from one is refused by the other.
func TestValidAPIPermissionRefusesGrantRolesAndKinds(t *testing.T) {
	for _, role := range everyGrantRole {
		if ValidAPIPermission(role) && role != PermissionTerraformState {
			t.Fatalf("grant role %q was accepted as a permission", role)
		}
	}
	for _, kind := range PrincipalKinds() {
		if ValidAPIPermission(kind) {
			t.Fatalf("principal kind %q was accepted as a permission", kind)
		}
	}
	for _, bogus := range []string{"", "READ", "read ", "agent_executor ", "*"} {
		if ValidAPIPermission(bogus) {
			t.Fatalf("%q was accepted as a permission", bogus)
		}
	}
}

func TestPermissionRuleSlicesAreCopies(t *testing.T) {
	roles := GrantRolesForPermission(PermissionSubmit)
	kinds := PrincipalKindsForPermission(PermissionSubmit)
	for i := range roles {
		roles[i] = "operator"
	}
	for i := range kinds {
		kinds[i] = KindAgent
	}
	if ClientAPIGrantAllowed(KindAgent, "operator", PermissionSubmit) {
		t.Fatal("mutating a returned rule slice widened the decision")
	}
	if !slices.Contains(GrantRolesForPermission(PermissionSubmit), "submitter") {
		t.Fatal("submit lost its submitter role after a caller mutated a copy")
	}
}

func TestUnknownKindOrPermissionIsRefused(t *testing.T) {
	if ClientAPIGrantAllowed("operator", "operator", PermissionRead) {
		t.Fatal("a grant role passed as a principal kind was admitted")
	}
	if ValidPrincipalKind("runner") || ValidPrincipalKind("") {
		t.Fatal("an unknown principal kind was accepted")
	}
	if ClientAPIGrantAllowed(KindHuman, "operator", "cancel") {
		t.Fatal("an unknown permission was admitted")
	}
}
