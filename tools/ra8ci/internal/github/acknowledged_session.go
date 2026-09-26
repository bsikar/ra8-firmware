// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"fmt"

	"github.com/actions/scaleset"
)

// A scale-set message is acknowledged by deleting it, and the whole reason
// GuardedClient sits in front of the official listener is that the listener
// acknowledges before it hands the message to a handler: anything deleted
// before it was written down is gone. DeleteMessage therefore refuses to
// acknowledge a message this process did not persist, and it decided that by
// asking whether the message identifier was in the recorded map.
//
// A message identifier is not an identity. It is the message queue's
// sequence number, and the queue belongs to the session: the listener drives
// GetMessage with lastMessageID and the upstream client answers from
// session.MessageQueueURL, so the numbering is a property of the session and
// a new session starts its own. The recorded map has always stored the
// session a message was written down under, beside the identifier, and
// nothing read it back.
//
// The window is real because the session is replaced underneath this
// wrapper, on the same client, without the listener restarting. Both
// MessageSessionClient.GetMessage and MessageSessionClient.DeleteMessage
// answer a MessageQueueTokenExpiredError by calling refreshMessageSession,
// which PATCHes the session and assigns whatever the forge answers with over
// c.session, and then retries the call. Session() reports the new one from
// then on. So a process can hold a record of message 7 written down under
// the session it opened with, meet a new session, be handed a different
// message 7 by the new queue, and acknowledge it on the strength of the old
// record. That message was never saved, and the job it carried is not in the
// inbox for the replay on restart to find: exactly the loss this wrapper was
// written to make impossible, arriving through the one door that looked
// safe.
//
// The rule is one-sided on purpose. A refresh that hands back the same
// session identifier is the ordinary case, the queue and its numbering carry
// on, and the acknowledgement goes through untouched. Only a session that
// genuinely became a different one is refused, and the refusal costs
// nothing: the message is still saved, the listener fails the pass, and the
// inbox replays it under the session that owns it.

// nilSessionID is the zero UUID, rendered. A session identifier of that
// shape is not an identity, it is the absence of one, and both doors treat
// it the same way rather than comparing two blanks and calling them equal.
const nilSessionID = "00000000-0000-0000-0000-000000000000"

// ErrForeignSessionAcknowledgement is a message recorded under a session
// other than the one now in force. It is a refusal, never a retry: the
// record belongs to a queue this client no longer reads from, and the
// message under that number today is somebody else's.
var ErrForeignSessionAcknowledgement = errors.New("refusing to acknowledge a message recorded under another session")

// sessionIdentity reports the identifier of the session in force, refusing a
// session that carries none. Both the persist door and the acknowledge door
// reach this, so neither can drift into accepting a blank the other refuses.
func sessionIdentity(session scaleset.RunnerScaleSetSession) (string, error) {
	identity := session.SessionID.String()
	if identity == "" || identity == nilSessionID {
		return "", errors.New("github message has no session identity")
	}
	return identity, nil
}

// acknowledgedUnderThisSession reports whether the record standing for a
// message authorises acknowledging it now.
//
// An empty record is a message this process never persisted; the caller
// establishes that from the map and this states it again rather than letting
// a blank compare equal to anything. A record from another session is the
// refusal this file exists for, and it names both sessions, because the
// operator reading the log needs to see that the session moved and not just
// that an acknowledgement failed.
func acknowledgedUnderThisSession(messageID int, recorded, current string) error {
	if recorded == "" {
		return fmt.Errorf("github message %d was not persisted", messageID)
	}
	if recorded != current {
		return fmt.Errorf("%w: message %d was persisted under session %s, the client now holds %s",
			ErrForeignSessionAcknowledgement, messageID, recorded, current)
	}
	return nil
}
