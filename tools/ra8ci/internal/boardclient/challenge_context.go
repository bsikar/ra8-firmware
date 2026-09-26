package boardclient

import "github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"

// checkChallengeContext holds a one-use neutral challenge to the reviewed
// context its purpose is issued under.
//
// The server derives that context itself and fails shut without it: a release
// challenge is only issued while a lease is live, and a recovery challenge is
// only issued while a recovery plan is open, and the challenge is stamped with
// whichever one applies. The client asked only half of that. Free holds a
// release challenge to the lease and generation it is releasing; FinishRecovery
// held a recovery challenge to the board, the snapshot version, the purpose and
// the fixture identity, and asked nothing about the plan, which is the one
// field naming WHICH reviewed hardware sequence this completion ends.
//
// A recovery is never started automatically. What puts a board back in service
// is a person approving a hardware sequence, which is why StartRecovery
// requires a plan and never defaults it. So a challenge naming no plan is not a
// challenge for a reviewed recovery, and the board agent signs over every field
// of whatever it is handed.
func checkChallengeContext(challenge store.NeutralChallenge) bool {
	switch challenge.Purpose {
	case "release":
		// A release ends a lease, not a hardware sequence. The lease
		// binding itself is the caller's to check, against the token it
		// holds; what does not belong here at all is a recovery plan.
		return challenge.RecoveryPlanID == ""
	case "recovery":
		return store.ValidID(challenge.RecoveryPlanID)
	default:
		return false
	}
}
