package store

import "slices"

// The client HTTP surface and the agent protocol are separate doors. A client
// certificate submits runs and reads their evidence; an agent certificate
// claims work and posts evidence about it. Every other authorization path in
// this package already pairs the principal kind with the grant role it is
// allowed to exercise: agentForCertificate requires kind='agent', and
// AuthorizeBoardPeer spells the pairs out for the board surface.
// AuthorizeCertificate did not, so a certificate of any kind holding a client
// grant passed the client surface. This file states the pairing once, as a
// decision with no SQL in it, so both the query and the check after it are
// taken from the same rule.

// Principal kinds recorded in api_principals.kind.
const (
	KindHuman      = "human"
	KindGitHubApp  = "github_app"
	KindAgent      = "agent"
	KindBoardAgent = "board_agent"
)

// Permissions the client HTTP surface asks AuthorizeCertificate for. These are
// permissions, not grant roles: a permission is satisfied by any of several
// roles, and the mapping is stated below rather than at each call site.
const (
	PermissionRead           = "read"
	PermissionSubmit         = "submit"
	PermissionTerraformState = "terraform_state"
)

type clientAPIRule struct {
	roles []string
	kinds []string
}

// clientAPIRules is the whole client-surface access rule. A permission absent
// from this map is not a permission the client surface offers.
var clientAPIRules = map[string]clientAPIRule{
	PermissionRead: {
		roles: []string{"observer", "submitter", "operator"},
		kinds: []string{KindHuman, KindGitHubApp},
	},
	PermissionSubmit: {
		roles: []string{"submitter", "operator"},
		kinds: []string{KindHuman, KindGitHubApp},
	},
	PermissionTerraformState: {
		roles: []string{"terraform_state"},
		kinds: []string{KindHuman, KindGitHubApp},
	},
}

// PrincipalKinds returns every kind api_principals.kind admits.
func PrincipalKinds() []string {
	return []string{KindHuman, KindGitHubApp, KindAgent, KindBoardAgent}
}

// ValidPrincipalKind reports whether kind is one the schema admits.
func ValidPrincipalKind(kind string) bool {
	return slices.Contains(PrincipalKinds(), kind)
}

// APIPermissions returns the permissions the client surface offers, in a
// stable order so a caller's switch cannot silently fall behind this file.
func APIPermissions() []string {
	return []string{PermissionRead, PermissionSubmit, PermissionTerraformState}
}

// ValidAPIPermission reports whether permission is one the client surface offers.
func ValidAPIPermission(permission string) bool {
	_, ok := clientAPIRules[permission]
	return ok
}

// GrantRolesForPermission returns the grant roles that satisfy permission. The
// slice is a copy: a caller cannot widen the rule through the result.
func GrantRolesForPermission(permission string) []string {
	return slices.Clone(clientAPIRules[permission].roles)
}

// PrincipalKindsForPermission returns the principal kinds allowed to exercise
// permission on the client surface. The slice is a copy.
func PrincipalKindsForPermission(permission string) []string {
	return slices.Clone(clientAPIRules[permission].kinds)
}

// ClientAPIGrantAllowed is the decision itself: may a principal of this kind
// exercise this permission through this grant role? An unknown permission,
// kind or role is refused rather than ignored.
func ClientAPIGrantAllowed(kind, role, permission string) bool {
	rule, ok := clientAPIRules[permission]
	if !ok || !ValidPrincipalKind(kind) {
		return false
	}
	return slices.Contains(rule.kinds, kind) && slices.Contains(rule.roles, role)
}
