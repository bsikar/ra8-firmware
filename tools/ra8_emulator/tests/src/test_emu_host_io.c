/**
 * @file test_emu_host_io.c
 * @brief Fault-injection tests for the emulator raw-descriptor I/O seam.
 * @details Injects deterministic short transfers, EINTR, zero writes, hard
 * faults, and format overflow while checking target-preserving transactions.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "emu_host_io_internal.h"

typedef struct {
  uint8_t data[128]; /**< Captured bytes.                        */
  size_t  length;    /**< Captured byte count.                   */
  size_t  calls;     /**< Mock invocation count.                 */
  size_t  eintr;     /**< EINTR responses remaining.             */
  size_t  quantum;   /**< Maximum bytes per successful response. */
  bool    fail;      /**< Return EIO when true.                  */
  bool    zero;      /**< Return zero bytes when true.           */
  int     last_fd;   /**< Most recent descriptor argument.       */
  off_t   last_off;  /**< Most recent positioned-I/O offset.     */
  size_t  position;  /**< Read cursor within data.               */
} io_mock_t;

static io_mock_t s_mock;

/**
 * @brief Emulate one bounded sequential write response.
 * @details Applies configured EINTR, hard-fault, zero, and short-write behavior.
 * @param[in] fd Observed descriptor.
 * @param[in] buf Source bytes.
 * @param[in] count Requested byte count.
 * @return Scripted byte count or failure.
 * @retval -1 A scripted EINTR or EIO fault fired.
 * @pre @p buf spans @p count readable bytes when count is nonzero.
 * @pre internal_reset_mock() initialised the script.
 * @post Successful bytes are appended to s_mock.data.
 * @post The call count and descriptor are recorded.
 * @note Test-global state makes this helper single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static ssize_t internal_mock_write(int fd, const void* buf, size_t count)
{
  s_mock.calls++;
  s_mock.last_fd = fd;
  if (s_mock.eintr > 0U) {
    s_mock.eintr--;
    errno = EINTR;
    return -1;
  }
  if (s_mock.fail) {
    errno = EIO;
    return -1;
  }
  if (s_mock.zero) {
    return 0;
  }
  size_t amount = (count < s_mock.quantum) ? count : s_mock.quantum;
  if ((s_mock.length + amount) > sizeof(s_mock.data)) {
    amount = sizeof(s_mock.data) - s_mock.length;
  }
  (void)memcpy(&s_mock.data[s_mock.length], buf, amount);
  s_mock.length += amount;
  return (ssize_t)amount;
}

/**
 * @brief Emulate one bounded sequential read response.
 * @details Applies scripted EINTR/fault behavior and copies at most one quantum.
 * @param[in] fd Observed descriptor.
 * @param[out] buf Destination bytes.
 * @param[in] count Requested byte count.
 * @return Scripted byte count or failure.
 * @retval -1 A scripted EINTR or EIO fault fired.
 * @pre @p buf spans @p count writable bytes when count is nonzero.
 * @pre s_mock.position does not exceed s_mock.length.
 * @post Successful bytes advance s_mock.position.
 * @post The call count and descriptor are recorded.
 * @note Test-global state makes this helper single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static ssize_t internal_mock_read(int fd, void* buf, size_t count)
{
  s_mock.calls++;
  s_mock.last_fd = fd;
  if (s_mock.eintr > 0U) {
    s_mock.eintr--;
    errno = EINTR;
    return -1;
  }
  if (s_mock.fail) {
    errno = EIO;
    return -1;
  }
  const size_t available = s_mock.length - s_mock.position;
  size_t       amount    = (count < s_mock.quantum) ? count : s_mock.quantum;
  amount                 = (amount < available) ? amount : available;
  if (amount > 0U) {
    (void)memcpy(buf, &s_mock.data[s_mock.position], amount);
    s_mock.position += amount;
  }
  return (ssize_t)amount;
}

/**
 * @brief Emulate one positioned write and record its offset.
 * @details Delegates data/fault behavior to internal_mock_write().
 * @param[in] fd Observed descriptor.
 * @param[in] buf Source bytes.
 * @param[in] count Requested byte count.
 * @param[in] offset Observed starting offset.
 * @return Scripted byte count or failure.
 * @retval -1 A scripted fault fired.
 * @pre @p buf spans @p count readable bytes when nonzero.
 * @pre internal_reset_mock() initialised the script.
 * @post s_mock.last_off equals @p offset.
 * @post Write behavior matches internal_mock_write().
 * @note Test-global state makes this helper single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static ssize_t
internal_mock_pwrite(int fd, const void* buf, size_t count, off_t offset)
{
  s_mock.last_off = offset;
  return internal_mock_write(fd, buf, count);
}

/**
 * @brief Emulate one positioned read and record its offset.
 * @details Delegates data/fault behavior to internal_mock_read().
 * @param[in] fd Observed descriptor.
 * @param[out] buf Destination bytes.
 * @param[in] count Requested byte count.
 * @param[in] offset Observed starting offset.
 * @return Scripted byte count or failure.
 * @retval -1 A scripted fault fired.
 * @pre @p buf spans @p count writable bytes when nonzero.
 * @pre internal_reset_mock() initialised the script.
 * @post s_mock.last_off equals @p offset.
 * @post Read behavior matches internal_mock_read().
 * @note Test-global state makes this helper single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static ssize_t internal_mock_pread(int fd, void* buf, size_t count, off_t offset)
{
  s_mock.last_off = offset;
  return internal_mock_read(fd, buf, count);
}

static const emu_io_ops_t s_k_mock_ops = {
  .read_fn   = internal_mock_read,
  .write_fn  = internal_mock_write,
  .pread_fn  = internal_mock_pread,
  .pwrite_fn = internal_mock_pwrite,
};

/**
 * @brief Reset the injected operation script.
 * @details Clears captured state and selects the next transfer quantum/retry prefix.
 * @param[in] quantum Maximum successful bytes per call.
 * @param[in] eintr Number of initial EINTR responses.
 * @pre @p quantum is nonzero for progress-producing tests.
 * @pre No transfer is active while the script is reset.
 * @post All captured bytes/counters are cleared.
 * @post The supplied quantum and EINTR count are installed.
 * @note Test-global state makes this helper single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_reset_mock(size_t quantum, size_t eintr)
{
  s_mock         = (io_mock_t){};
  s_mock.quantum = quantum;
  s_mock.eintr   = eintr;
  s_mock.last_fd = -1;
}

/**
 * @brief Prove descriptor selection plus EINTR and short-write recovery.
 * @details Captures output/error payloads and verifies byte order and descriptor routing.
 * @return True when every assertion passes.
 * @retval true Both sinks wrote exact expected bytes.
 * @retval false A result, byte, call count, or descriptor differed.
 * @pre The mock operation table is complete.
 * @pre No other test mutates s_mock concurrently.
 * @post The configured output/error descriptors were independently observed.
 * @post Captured payload bytes remain available for comparison.
 * @note Test-global state makes this helper single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_test_short_write_and_sinks(void)
{
  internal_reset_mock(2U, 1U);
  priv_emu_io_configure(41, 42, &s_k_mock_ops);
  emu_io_result_t result = priv_emu_io_out_text("abcde");
  if ((result.status != k_emu_io_ok) || (result.transferred != 5U) || (s_mock.calls != 4U) ||
      (s_mock.last_fd != 41) || (memcmp(s_mock.data, "abcde", 5U) != 0)) {
    return false;
  }
  internal_reset_mock(8U, 0U);
  result = priv_emu_io_errf("err:%u", 7U);
  return (result.status == k_emu_io_ok) && (s_mock.last_fd == 42) && (s_mock.length == 5U) &&
         (memcmp(s_mock.data, "err:7", 5U) == 0);
}

/**
 * @brief Prove the consecutive EINTR retry budget is finite.
 * @details Scripts nine interruptions and requires failure after eight retries.
 * @return True when the bound and reported errno are exact.
 * @retval true The ninth call returned the expected EINTR result.
 * @retval false Retry count, status, progress, or errno differed.
 * @pre The mock operation table is complete.
 * @pre No other test mutates s_mock concurrently.
 * @post Exactly nine mock calls were attempted.
 * @post Zero transferred bytes are reported.
 * @note Test-global state makes this helper single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_test_eintr_bound(void)
{
  internal_reset_mock(8U, 9U);
  priv_emu_io_configure(1, 2, &s_k_mock_ops);
  const emu_io_result_t result = priv_emu_io_write_exact(7, "x", 1U);
  return (result.status == k_emu_io_error) && (result.transferred == 0U) &&
         (result.os_error == EINTR) && (s_mock.calls == 9U);
}

/**
 * @brief Prove short positioned reads and premature EOF semantics.
 * @details Exercises EINTR recovery, advancing offsets, exact bytes, and partial EOF progress.
 * @return True when both read vectors pass.
 * @retval true Positioned and sequential results match the contract.
 * @retval false Any status, byte, offset, or progress value differed.
 * @pre The mock operation table is complete.
 * @pre No other test mutates s_mock concurrently.
 * @post The first vector returns five exact bytes.
 * @post The second vector reports EOF after six bytes.
 * @note Test-global state makes this helper single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_test_short_read_and_eof(void)
{
  internal_reset_mock(2U, 1U);
  (void)memcpy(s_mock.data, "abcdef", 6U);
  s_mock.length = 6U;
  priv_emu_io_configure(1, 2, &s_k_mock_ops);
  char            text[7] = {};
  emu_io_result_t result  = priv_emu_io_pread_exact(8, text, 5U, 23);
  if ((result.status != k_emu_io_ok) || (result.transferred != 5U) ||
      (memcmp(text, "abcde", 5U) != 0) || (s_mock.last_off != 27)) {
    return false;
  }
  internal_reset_mock(4U, 0U);
  (void)memcpy(s_mock.data, "abcdef", 6U);
  s_mock.length = 6U;
  result        = priv_emu_io_read_exact(8, text, sizeof(text));
  return (result.status == k_emu_io_eof) && (result.transferred == 6U);
}

/**
 * @brief Prove a zero-byte output response cannot spin forever.
 * @details Requires the exact-write loop to classify no-progress output as EIO.
 * @return True when zero-write semantics are exact.
 * @retval true EIO is returned with zero progress.
 * @retval false Status, errno, or progress differed.
 * @pre The mock operation table is complete.
 * @pre No other test mutates s_mock concurrently.
 * @post One no-progress response was consumed.
 * @post No bytes were captured.
 * @note Test-global state makes this helper single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_test_zero_write(void)
{
  internal_reset_mock(4U, 0U);
  s_mock.zero = true;
  priv_emu_io_configure(1, 2, &s_k_mock_ops);
  const emu_io_result_t result = priv_emu_io_write_exact(3, "x", 1U);
  return (result.status == k_emu_io_error) && (result.os_error == EIO) &&
         (result.transferred == 0U);
}

/**
 * @brief Prove oversized formatted text is rejected before output.
 * @details Formats exactly the scratch capacity, which requires a terminator beyond the bound.
 * @return True when truncation is visible and atomic.
 * @retval true Truncation reports zero bytes and zero write calls.
 * @retval false Status, progress, or call count differed.
 * @pre The mock operation table is complete.
 * @pre No other test mutates s_mock concurrently.
 * @post The injected write callback is not called.
 * @post No bytes are captured.
 * @note Test-global state makes this helper single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_test_format_truncation(void)
{
  internal_reset_mock(128U, 0U);
  priv_emu_io_configure(1, 2, &s_k_mock_ops);
  const emu_io_result_t result = priv_emu_io_outf("%01024u", 1U);
  return (result.status == k_emu_io_truncated) && (result.transferred == 0U) &&
         (s_mock.calls == 0U);
}

/**
 * @brief Prove positioned short writes advance their offsets exactly.
 * @details Combines one EINTR with two-byte writes and verifies final bytes/offset.
 * @return True when positioned output is exact.
 * @retval true Four ordered bytes completed at advancing offsets.
 * @retval false Status, progress, offset, or bytes differed.
 * @pre The mock operation table is complete.
 * @pre No other test mutates s_mock concurrently.
 * @post Four bytes are captured in source order.
 * @post The final callback offset is nineteen.
 * @note Test-global state makes this helper single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_test_positioned_short_write(void)
{
  internal_reset_mock(2U, 1U);
  priv_emu_io_configure(1, 2, &s_k_mock_ops);
  const emu_io_result_t result = priv_emu_io_pwrite_exact(9, "abcd", 4U, 17);
  return (result.status == k_emu_io_ok) && (result.transferred == 4U) && (s_mock.last_off == 19) &&
         (memcmp(s_mock.data, "abcd", 4U) == 0);
}

/**
 * @brief Read one real target file through the production seam.
 * @details Opens, size-checks, reads exactly, and closes the descriptor.
 * @param[in] path Target path.
 * @param[out] text Destination buffer.
 * @param[in] length Required file and buffer length.
 * @return True when the exact file was read.
 * @retval true Open, size, read, and close prerequisites succeeded.
 * @retval false Open, size, or read failed.
 * @pre @p path is non-null.
 * @pre @p text spans @p length writable bytes.
 * @post Any opened descriptor is closed.
 * @post On success @p text contains the complete file.
 * @note Uses production raw operations after the caller restores them.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_read_target(const char* path, char* text, size_t length)
{
  emu_io_file_t file = {.fd = -1, .size = 0};
  if ((priv_emu_io_open_read(path, &file).status != k_emu_io_ok) ||
      (file.size != (int64_t)length)) {
    return false;
  }
  const bool ok = priv_emu_io_read_exact(file.fd, text, length).status == k_emu_io_ok;
  (void)priv_emu_io_close(&file);
  return ok;
}

/**
 * @brief Prove transaction abort preserves an existing target.
 * @details Creates an original file, injects a sibling write fault, aborts, and rereads it.
 * @return True when the original target remains byte-identical.
 * @retval true The original three bytes survived the failed transaction.
 * @retval false Setup, fault reporting, reread, or bytes differed.
 * @pre The host permits temporary files under /tmp.
 * @pre No other test mutates s_mock concurrently.
 * @post The sibling temporary is closed and unlinked.
 * @post The final test target is unlinked after verification.
 * @note Test-global state makes this helper single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_test_transaction_preserves_target(void)
{
  char path[] = "/tmp/emu_host_io_test.XXXXXX";
  int  fd     = mkstemp(path);
  if ((fd < 0) || (write(fd, "old", 3U) != 3) || (close(fd) != 0)) {
    return false;
  }
  emu_io_txn_t txn = {.fd = -1};
  bool         ok  = priv_emu_io_txn_begin(path, &txn).status == k_emu_io_ok;
  internal_reset_mock(8U, 0U);
  s_mock.fail = true;
  priv_emu_io_configure(1, 2, &s_k_mock_ops);
  ok = ok && (priv_emu_io_write_exact(txn.fd, "new", 3U).status == k_emu_io_error);
  priv_emu_io_txn_abort(&txn);
  priv_emu_io_configure(STDOUT_FILENO, STDERR_FILENO, nullptr);
  char text[3] = {};
  ok           = internal_read_target(path, text, sizeof(text)) && ok;
  ok           = (memcmp(text, "old", sizeof(text)) == 0) && ok;
  (void)unlink(path);
  return ok;
}

/**
 * @brief Prove regular-file open faults are explicit and ownership-safe.
 * @details Opens a deliberately nonexistent path and checks status plus descriptor state.
 * @return True when the open failure contract is exact.
 * @retval true The call reports a host error and leaves fd at -1.
 * @retval false Status or descriptor ownership differed.
 * @pre The sentinel path does not exist.
 * @pre Production raw operations are configured.
 * @post No descriptor is acquired.
 * @post No filesystem object is created.
 * @note The sentinel name is unique to this test binary.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_test_open_fault(void)
{
  emu_io_file_t         file = {.fd = -1, .size = 0};
  const emu_io_result_t result =
    priv_emu_io_open_read("/tmp/ra8_emulator_io_path_that_does_not_exist", &file);
  return (result.status == k_emu_io_error) && (file.fd == -1);
}

/**
 * @brief Run the complete raw-I/O seam fault matrix.
 * @details Executes every injected and real-file vector, then restores production composition.
 * @return Process success or failure.
 * @retval 0 Every vector passed.
 * @retval 1 At least one vector failed.
 * @pre The test process can create a temporary file under /tmp.
 * @pre No other thread reconfigures the process-wide seam.
 * @post Production descriptors and raw operations are restored.
 * @post Temporary test targets are removed by their owning vector.
 * @note Intentionally single-threaded for deterministic fault scripting.
 * @since 0.1.0
 */
int main(void)
{
  const bool ok = internal_test_short_write_and_sinks() && internal_test_eintr_bound() &&
                  internal_test_short_read_and_eof() && internal_test_zero_write() &&
                  internal_test_format_truncation() && internal_test_positioned_short_write() &&
                  internal_test_transaction_preserves_target() && internal_test_open_fault();
  priv_emu_io_configure(STDOUT_FILENO, STDERR_FILENO, nullptr);
  return ok ? 0 : 1;
}
