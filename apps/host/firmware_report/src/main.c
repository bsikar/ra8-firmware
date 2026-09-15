// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

/**
 * @file main.c
 * @brief Hosted firmware-image reporting command.
 * @details Owns bounded file input, diagnostics, ABI lifecycle, and presentation.
 */

#include <stdio.h>
#include <stdlib.h>

#include "firmware_report.h"
#include "firmware_report_cli.h"

/**
 * @enum firmware_report_limit_t
 * @brief Bounded host input limit.
 * @details Prevents unbounded allocation before invoking the provider.
 * @invariant The limit equals the public ABI maximum.
 * @code
 * size_t limit = k_firmware_report_max_image;
 * @endcode
 * @see firmware_report_create
 */
typedef enum : uint32_t {
  k_firmware_report_max_image = 16U * 1024U * 1024U, /**< Maximum accepted input bytes. */
} firmware_report_limit_t;

/**
 * @struct image_buffer_t
 * @brief Owned input bytes and their complete length.
 * @details Represents either one heap allocation or an empty image with a null pointer.
 * @invariant `bytes` is non-null whenever `size` is nonzero.
 * @code
 * image_buffer_t image = {};
 * @endcode
 * @see read_image
 */
typedef struct {
  uint8_t* bytes; /**< Heap buffer, or null for an empty input. */
  size_t   size;  /**< Complete input length in bytes.          */
} image_buffer_t;

/**
 * @brief Write one stable diagnostic and return the command failure status.
 * @details Prefixes the supplied static message; no ABI state is modified.
 * @param[in] message Non-null, null-terminated diagnostic text.
 * @return Process status two.
 * @retval 2 The diagnostic path always reports command failure.
 * @pre `message` points to a readable null-terminated string.
 * @pre Standard error is a valid hosted stream.
 * @post A best-effort diagnostic write has been attempted.
 * @post No caller-owned storage is modified.
 * @note Thread-compatible; standard error serialization is owned by the C runtime.
 * @since 0.1.0
 */
static int fail(const char* message)
{
  (void)fprintf(stderr, "firmware_report: %s\n", message);
  return 2;
}

/**
 * @brief Read one complete bounded firmware image.
 * @details Measures before allocation and publishes the output only after the complete read.
 * @param[in] path Non-empty path to the input image.
 * @param[out] out_image Writable empty image-buffer destination.
 * @return Process-compatible status.
 * @retval 0 Input was read completely.
 * @retval 2 Open, measurement, allocation, read, or close failed.
 * @pre `path` names a readable null-terminated string.
 * @pre `out_image` points to writable zero-initialized storage.
 * @post Success transfers one buffer to `out_image` or represents an empty file with null.
 * @post Failure leaves `out_image` empty and releases all temporary resources.
 * @note Thread-compatible; it uses only local state and hosted file services.
 * @since 0.1.0
 */
static int read_image(const char* path, image_buffer_t* out_image)
{
  FILE* input = fopen(path, "rb");
  if (input == nullptr) {
    return fail("cannot open input");
  }
  if (fseek(input, 0L, SEEK_END) != 0) {
    (void)fclose(input);
    return fail("cannot determine input size");
  }
  const long measured = ftell(input);
  if (measured < 0L) {
    (void)fclose(input);
    return fail("cannot determine input size");
  }
  if (((unsigned long)measured > k_firmware_report_max_image) ||
      (fseek(input, 0L, SEEK_SET) != 0)) {
    (void)fclose(input);
    return fail("input exceeds the 16 MiB limit");
  }
  const size_t size  = (size_t)measured;
  uint8_t*     bytes = size == 0U ? nullptr : malloc(size);
  if ((size != 0U) && (bytes == nullptr)) {
    (void)fclose(input);
    return fail("cannot allocate input buffer");
  }
  if ((size != 0U) && (fread(bytes, 1U, size, input) != size)) {
    free(bytes);
    (void)fclose(input);
    return fail("cannot read complete input");
  }
  if (fclose(input) != 0) {
    free(bytes);
    return fail("cannot close input");
  }
  out_image->bytes = bytes;
  out_image->size  = size;
  return 0;
}

/**
 * @brief Analyze and print one owned image buffer.
 * @details Borrows, queries, and releases the Rust provider token before formatting output.
 * @param[in] image Complete image buffer whose bytes remain readable for this call.
 * @return Process-compatible status.
 * @retval 0 Analysis and output succeeded.
 * @retval 2 ABI lifecycle or output failed.
 * @pre `image` is non-null and its pointer/length pair is valid.
 * @pre The provider has capacity for one report borrow.
 * @post Every successful provider borrow is released exactly once.
 * @post The input buffer remains owned by the caller.
 * @note Thread-compatible; the provider serializes its registry.
 * @since 0.1.0
 */
static int print_report(const image_buffer_t* image)
{
  firmware_report_handle_t* handle  = nullptr;
  firmware_report_summary_t summary = {};
  firmware_report_status_t  status  = firmware_report_create(image->bytes, image->size, &handle);
  if (status != k_firmware_report_ok) {
    return fail("Rust provider rejected input");
  }
  status                                        = firmware_report_query(handle, &summary);
  const firmware_report_status_t release_status = firmware_report_release(&handle);
  if ((status != k_firmware_report_ok) || (release_status != k_firmware_report_ok) ||
      (handle != nullptr)) {
    return fail("Rust provider lifecycle failed");
  }
  if (printf("bytes=%llu\nzero=%llu\nerased=%llu\nfnv1a64=%016llx\n",
             (unsigned long long)summary.byte_count,
             (unsigned long long)summary.zero_count,
             (unsigned long long)summary.erased_count,
             (unsigned long long)summary.fnv1a64) < 0) {
    return fail("cannot write output");
  }
  return 0;
}

/**
 * @brief Run the bounded firmware reporting command.
 * @details Validates arguments, reads one input, delegates reporting, and frees the C buffer.
 * @param[in] argc Number of command-line arguments.
 * @param[in] argv Hosted argument vector.
 * @return Process completion status.
 * @retval 0 Report emitted successfully.
 * @retval 2 Usage, input, ABI lifecycle, or output failed.
 * @pre `argc` and `argv` satisfy the hosted C entry-point contract.
 * @pre Hosted file and stream runtime services are initialized.
 * @post Every opened file and allocated C input buffer is released.
 * @post Every successfully borrowed Rust report token is released exactly once.
 * @note Not thread-safe; this is the process entry point.
 * @since 0.1.0
 */
int main(int argc, char** argv)
{
  const char* path = nullptr;
  if (firmware_report_parse_args(argc, argv, &path) != k_firmware_report_cli_ok) {
    (void)fprintf(stderr, "usage: firmware_report <firmware-image>\n");
    return 2;
  }
  image_buffer_t image  = {};
  const int      status = read_image(path, &image);
  if (status != 0) {
    return status;
  }
  const int report_status = print_report(&image);
  free(image.bytes);
  return report_status;
}
