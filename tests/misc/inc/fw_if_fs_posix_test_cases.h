/**
 * @file fw_if_fs_posix_test_cases.h
 * @brief POSIX-specific filesystem security and transaction-retry vectors
 *
 * @details
 * Declares the test-only suite kept separate from the portable
 * filesystem conformance driver.
 * The companion owns hosted path setup while the public test entry retains a
 * narrow assertion-based interface.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include "fw_if_fs.h"
#include "fw_if_fs_posix.h"
#include "ra8_attributes.h"

/**
 * @brief Run POSIX lifecycle, confinement, and occupied-stage scenarios
 *
 * @details
 * Validates every borrowed pointer before forwarding to focused public
 * lifecycle, capability, canonical-alias, symlink-confinement, and
 * transaction-stage collision helpers. Assertion failures terminate the
 * current hosted test process with a diagnostic.
 *
 * @param[in] fs Initialized filesystem facade rooted at @p root.
 * @param[in,out] state Initialized POSIX adapter state owned by @p fs.
 * @param[in] root Native private directory backing @p fs.
 * @pre @p fs and @p state are non-null and describe the same adapter.
 * @pre @p root is a non-null NUL-terminated path to an empty private fixture.
 * @post Every created symlink, directory, cursor, and staged file has been removed.
 * @post The adapter has no active transaction and retains a usable root descriptor.
 * @note Not thread-safe; assertions and filesystem mutations are process-local.
 * @par MC/DC:
 * No compound production decision or MC/DC citation is attributed to this
 * test helper.
 * @par Branch and security vectors:
 * Public init covers each null guard, duplicate state, missing and regular-file
 * roots, truthful removable-media capability, successful deinit, repeated
 * deinit, and null deinit. Failed binds must remain uninitialized with root fd
 * -1. Exact `/tmp` and `/var` bindings are namespace-read-only; Darwin also
 * compares their descriptors with `/private/tmp` and `/private/var`.
 *
 * Relative, absolute, and exact-looking nested targets for `tmp` and `var`,
 * plus an ordinary-name control, must fail both intermediate stat and final
 * directory-open operations without opening a cursor. A symlinked private
 * composition root must also fail initialization without retaining state. The
 * pure root-alias classifier accepts only exact component/target pairs. The
 * canonical opener rejects missing or symlinked `private`, `tmp`, and `var`
 * components, opens both real children, rejects an invalid alias, and always
 * consumes or resets its output descriptor.
 * @since 0.1.0
 */
RA8_TEST_HELPER void
ra8_test_fw_if_fs_posix_cases(const fw_fs_t* fs, fw_fs_posix_state_t* state, const char* root);
