/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Brighton Sikarskie */

/**
 * @file port/mbedtls/inc/tf_psa_crypto_config_platform.h
 * @brief PSA crypto configuration -- platform abstraction + test options.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Split-out sub-header of @ref tf_psa_crypto_config.h carrying the
 * platform-abstraction-layer settings (memory/threading/time hooks and
 * the `MBEDTLS_PLATFORM_xxx` knobs) plus the general and test
 * configuration options. The thin umbrella `tf_psa_crypto_config.h`
 * includes this file along with its sibling sub-headers so existing
 * consumers that `#include "tf_psa_crypto_config.h"` are unchanged. The
 * original Mbed TLS license header for the moved content is preserved
 * verbatim below.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

/**
 * \name SECTION: Platform abstraction layer
 *
 * This section sets platform specific settings.
 * \{
 */

/**
 * \def MBEDTLS_MEMORY_BUFFER_ALLOC_C
 *
 * Enable the buffer allocator implementation that makes use of a (stack)
 * based buffer to 'allocate' dynamic memory. (replaces calloc() and free()
 * calls)
 *
 * Module:  platform/memory_buffer_alloc.c
 *
 * Requires: MBEDTLS_PLATFORM_C
 *           MBEDTLS_PLATFORM_MEMORY (to use it within Mbed TLS)
 *
 * Enable this module to enable the buffer memory allocator.
 */
//#define MBEDTLS_MEMORY_BUFFER_ALLOC_C

/**
 * \def MBEDTLS_FS_IO
 *
 * Enable functions that use the filesystem.
 *
 * Disabled: this firmware has no filesystem; the example app passes
 * X.509 trust anchors in as DER buffers (mbedtls_x509_crt_parse_der).
 */
//#define MBEDTLS_FS_IO

/**
 * \def MBEDTLS_HAVE_TIME
 *
 * System has time.h and time().
 * The time does not need to be correct, only time differences are used,
 * by contrast with MBEDTLS_HAVE_TIME_DATE
 *
 * Defining MBEDTLS_HAVE_TIME allows you to specify MBEDTLS_PLATFORM_TIME_ALT,
 * MBEDTLS_PLATFORM_TIME_MACRO, MBEDTLS_PLATFORM_TIME_TYPE_MACRO and
 * MBEDTLS_PLATFORM_STD_TIME.
 *
 * Comment if your system does not support time functions.
 *
 * Disabled: arm-none-eabi newlib has no time() backend. The example
 * app supplies its own monotonic-tick callback when timing-sensitive
 * SSL features are enabled.
 */
//#define MBEDTLS_HAVE_TIME

/**
 * \def MBEDTLS_HAVE_TIME_DATE
 *
 * System has time.h, time(), and an implementation for
 * mbedtls_platform_gmtime_r() (see below).
 * The time needs to be correct (not necessarily very accurate, but at least
 * the date should be correct). This is used to verify the validity period of
 * X.509 certificates.
 *
 * Comment if your system does not have a correct clock.
 *
 * \note mbedtls_platform_gmtime_r() is an abstraction in platform_util.h that
 * behaves similarly to the gmtime_r() function from the C standard. Refer to
 * the documentation for mbedtls_platform_gmtime_r() for more information.
 *
 * \note It is possible to configure an implementation for
 * mbedtls_platform_gmtime_r() at compile-time by using the macro
 * MBEDTLS_PLATFORM_GMTIME_R_ALT.
 *
 * Disabled: this firmware has no calendar-time source yet. The
 * example app accepts cert validity windows blindly until ra8_time
 * is wired.
 */
//#define MBEDTLS_HAVE_TIME_DATE

/**
 * \def MBEDTLS_MEMORY_DEBUG
 *
 * Enable debugging of buffer allocator memory issues. Automatically prints
 * (to stderr) all (fatal) messages on memory allocation issues. Enables
 * function for 'debug output' of allocated memory.
 *
 * Requires: MBEDTLS_MEMORY_BUFFER_ALLOC_C
 *
 * Uncomment this macro to let the buffer allocator print out error messages.
 */
//#define MBEDTLS_MEMORY_DEBUG

/**
 * \def MBEDTLS_MEMORY_BACKTRACE
 *
 * Include backtrace information with each allocated block.
 *
 * Requires: MBEDTLS_MEMORY_BUFFER_ALLOC_C
 *           GLIBC-compatible backtrace() and backtrace_symbols() support
 *
 * Uncomment this macro to include backtrace information
 */
//#define MBEDTLS_MEMORY_BACKTRACE

/**
 * @brief MBEDTLS PLATFORM c.
 *
 * \def MBEDTLS_PLATFORM_C
 *
 * Enable the platform abstraction layer that allows you to re-assign
 * functions like calloc(), free(), snprintf(), printf(), fprintf(), exit().
 *
 * Enabling MBEDTLS_PLATFORM_C enables to use of MBEDTLS_PLATFORM_XXX_ALT
 * or MBEDTLS_PLATFORM_XXX_MACRO directives, allowing the functions mentioned
 * above to be specified at runtime or compile time respectively.
 *
 * \note This abstraction layer must be enabled on Windows (including MSYS2)
 * as other modules rely on it for a fixed snprintf implementation.
 *
 * Module:  platform/platform.c
 * Caller:  Most other .c files
 *
 * This module enables abstraction of common (libc) functions.
 */
#define MBEDTLS_PLATFORM_C

/**
 * \def MBEDTLS_PLATFORM_EXIT_ALT
 *
 * MBEDTLS_PLATFORM_XXX_ALT: Uncomment a macro to let Mbed TLS support the
 * function in the platform abstraction layer.
 *
 * Example: In case you uncomment MBEDTLS_PLATFORM_PRINTF_ALT, Mbed TLS will
 * provide a function "mbedtls_platform_set_printf()" that allows you to set an
 * alternative printf function pointer.
 *
 * All these define require MBEDTLS_PLATFORM_C to be defined!
 *
 * \note MBEDTLS_PLATFORM_SNPRINTF_ALT and MBEDTLS_PLATFORM_VSNPRINTF_ALT
 * are required on some Windows C runtimes.
 * They will be enabled automatically by build_info.h when building with
 * older versions of MSVC or with MinGW32.
 *
 * \warning MBEDTLS_PLATFORM_XXX_ALT cannot be defined at the same time as
 * MBEDTLS_PLATFORM_XXX_MACRO!
 *
 * Requires: MBEDTLS_PLATFORM_TIME_ALT requires MBEDTLS_HAVE_TIME
 *
 * Uncomment a macro to enable alternate implementation of specific base
 * platform function
 */
//#define MBEDTLS_PLATFORM_SETBUF_ALT
//#define MBEDTLS_PLATFORM_EXIT_ALT
//#define MBEDTLS_PLATFORM_TIME_ALT
//#define MBEDTLS_PLATFORM_FPRINTF_ALT
//#define MBEDTLS_PLATFORM_PRINTF_ALT
//#define MBEDTLS_PLATFORM_SNPRINTF_ALT
//#define MBEDTLS_PLATFORM_VSNPRINTF_ALT
//#define MBEDTLS_PLATFORM_NV_SEED_ALT
//#define MBEDTLS_PLATFORM_SETUP_TEARDOWN_ALT
//#define MBEDTLS_PLATFORM_MS_TIME_ALT

/**
 * Uncomment the macro to let Mbed TLS use your alternate implementation of
 * mbedtls_platform_gmtime_r(). This replaces the default implementation in
 * platform_util.c.
 *
 * gmtime() is not a thread-safe function as defined in the C standard. The
 * library will try to use safer implementations of this function, such as
 * gmtime_r() when available. However, if Mbed TLS cannot identify the target
 * system, the implementation of mbedtls_platform_gmtime_r() will default to
 * using the standard gmtime(). In this case, calls from the library to
 * gmtime() will be guarded by the global mutex mbedtls_threading_gmtime_mutex
 * if MBEDTLS_THREADING_C is enabled. We recommend that calls from outside the
 * library are also guarded with this mutex to avoid race conditions. However,
 * if the macro MBEDTLS_PLATFORM_GMTIME_R_ALT is defined, Mbed TLS will
 * unconditionally use the implementation for mbedtls_platform_gmtime_r()
 * supplied at compile time.
 */
//#define MBEDTLS_PLATFORM_GMTIME_R_ALT

/**
 * @brief MBEDTLS PLATFORM MEMORY.
 *
 * \def MBEDTLS_PLATFORM_MEMORY
 *
 * Enable the memory allocation layer.
 *
 * By default Mbed TLS uses the system-provided calloc() and free().
 * This allows different allocators (self-implemented or provided) to be
 * provided to the platform abstraction layer.
 *
 * Enabling #MBEDTLS_PLATFORM_MEMORY without the
 * MBEDTLS_PLATFORM_{FREE,CALLOC}_MACROs will provide
 * "mbedtls_platform_set_calloc_free()" allowing you to set an alternative calloc() and
 * free() function pointer at runtime.
 *
 * Enabling #MBEDTLS_PLATFORM_MEMORY and specifying
 * MBEDTLS_PLATFORM_{CALLOC,FREE}_MACROs will allow you to specify the
 * alternate function at compile time.
 *
 * An overview of how the value of mbedtls_calloc is determined:
 *
 * - if !MBEDTLS_PLATFORM_MEMORY
 *     - mbedtls_calloc = calloc
 * - if MBEDTLS_PLATFORM_MEMORY
 *     - if (MBEDTLS_PLATFORM_CALLOC_MACRO && MBEDTLS_PLATFORM_FREE_MACRO):
 *         - mbedtls_calloc = MBEDTLS_PLATFORM_CALLOC_MACRO
 *     - if !(MBEDTLS_PLATFORM_CALLOC_MACRO && MBEDTLS_PLATFORM_FREE_MACRO):
 *         - Dynamic setup via mbedtls_platform_set_calloc_free is now possible with a default value MBEDTLS_PLATFORM_STD_CALLOC.
 *         - How is MBEDTLS_PLATFORM_STD_CALLOC handled?
 *         - if MBEDTLS_PLATFORM_NO_STD_FUNCTIONS:
 *             - MBEDTLS_PLATFORM_STD_CALLOC is not set to anything;
 *             - MBEDTLS_PLATFORM_STD_MEM_HDR can be included if present;
 *         - if !MBEDTLS_PLATFORM_NO_STD_FUNCTIONS:
 *             - if MBEDTLS_PLATFORM_STD_CALLOC is present:
 *                 - User-defined MBEDTLS_PLATFORM_STD_CALLOC is respected;
 *             - if !MBEDTLS_PLATFORM_STD_CALLOC:
 *                 - MBEDTLS_PLATFORM_STD_CALLOC = calloc
 *
 *         - At this point the presence of MBEDTLS_PLATFORM_STD_CALLOC is checked.
 *         - if !MBEDTLS_PLATFORM_STD_CALLOC
 *             - MBEDTLS_PLATFORM_STD_CALLOC = uninitialized_calloc
 *
 *         - mbedtls_calloc = MBEDTLS_PLATFORM_STD_CALLOC.
 *
 * Defining MBEDTLS_PLATFORM_CALLOC_MACRO and #MBEDTLS_PLATFORM_STD_CALLOC at the same time is not possible.
 * MBEDTLS_PLATFORM_CALLOC_MACRO and MBEDTLS_PLATFORM_FREE_MACRO must both be defined or undefined at the same time.
 * #MBEDTLS_PLATFORM_STD_CALLOC and #MBEDTLS_PLATFORM_STD_FREE do not have to be defined at the same time, as, if they are used,
 * dynamic setup of these functions is possible. See the tree above to see how are they handled in all cases.
 * An uninitialized #MBEDTLS_PLATFORM_STD_CALLOC always fails, returning a null pointer.
 * An uninitialized #MBEDTLS_PLATFORM_STD_FREE does not do anything.
 *
 * Requires: MBEDTLS_PLATFORM_C
 *
 * Enable this layer to allow use of alternative memory allocators.
 *
 * Enabled: the example app binds mbedtls_calloc / mbedtls_free to a
 * ThreadX byte pool via mbedtls_platform_set_calloc_free() so the
 * "no malloc after init" rule is preserved.
 */
#define MBEDTLS_PLATFORM_MEMORY

/**
 * \def MBEDTLS_PLATFORM_NO_STD_FUNCTIONS
 *
 * Do not assign standard functions in the platform layer (e.g. calloc() to
 * MBEDTLS_PLATFORM_STD_CALLOC and printf() to MBEDTLS_PLATFORM_STD_PRINTF)
 *
 * This makes sure there are no linking errors on platforms that do not support
 * these functions. You will HAVE to provide alternatives, either at runtime
 * via the platform_set_xxx() functions or at compile time by setting
 * the MBEDTLS_PLATFORM_STD_XXX defines, or enabling a
 * MBEDTLS_PLATFORM_XXX_MACRO.
 *
 * Requires: MBEDTLS_PLATFORM_C
 *
 * Uncomment to prevent default assignment of standard functions in the
 * platform layer.
 */
//#define MBEDTLS_PLATFORM_NO_STD_FUNCTIONS

/**
 * Uncomment the macro to let Mbed TLS use your alternate implementation of
 * mbedtls_platform_zeroize(), to wipe sensitive data in memory. This replaces
 * the default implementation in platform_util.c.
 *
 * By default, the library uses a system function such as memset_s()
 * (optional feature of C11), explicit_bzero() (BSD and compatible), or
 * SecureZeroMemory (Windows). If no such function is detected, the library
 * falls back to a plain C implementation. Compilers are technically
 * permitted to optimize this implementation out, meaning that the memory is
 * not actually wiped. The library tries to prevent that, but the C language
 * makes it impossible to guarantee that the memory will always be wiped.
 *
 * If your platform provides a guaranteed method to wipe memory which
 * `platform_util.c` does not detect, define this macro to the name of
 * a function that takes two arguments, a `void *` pointer and a length,
 * and wipes that many bytes starting at the specified address. For example,
 * if your platform has explicit_bzero() but `platform_util.c` does not
 * detect its presence, define `MBEDTLS_PLATFORM_ZEROIZE_ALT` to be
 * `explicit_bzero` to use that function as mbedtls_platform_zeroize().
 */
//#define MBEDTLS_PLATFORM_ZEROIZE_ALT

/**
 * \def MBEDTLS_THREADING_ALT
 *
 * Provide your own alternate implementation of threading primitives:
 * mutexes and condition variables. If you enable this option:
 *
 * - Provide a header file `"threading_alt.h"`, defining the following
 *  elements:
 *     - The type `mbedtls_platform_mutex_t` of mutex objects.
 *     - The type `mbedtls_platform_condition_variable_t` of
 *       condition variable objects.
 *
 * - Call the function mbedtls_threading_set_alt() in your application
 *   before calling any other library function (in particular before
 *   calling psa_crypto_init()).
 *
 * See mbedtls/threading.h for more details, especially the documentation
 * of mbedtls_threading_set_alt().
 *
 * Requires: MBEDTLS_THREADING_C
 *
 * Uncomment this to allow your own alternate threading implementation.
 */
//#define MBEDTLS_THREADING_ALT

/**
 * \def MBEDTLS_THREADING_PTHREAD
 *
 * Enable the pthread wrapper layer for the threading layer.
 *
 * Requires: MBEDTLS_THREADING_C
 *
 * Uncomment this to enable pthread mutexes.
 */
//#define MBEDTLS_THREADING_PTHREAD

/**
 * \def MBEDTLS_THREADING_C
 *
 * Enable the threading abstraction layer.
 *
 * \note You must enable this option if TF-PSA-Crypto runs in a
 * multithreaded environment. Otherwise the PSA cryptography subsystem is
 * not thread-safe. As an exception, this option can be disabled if all
 * PSA crypto functions are ever called from a single thread. Note that
 * this includes indirect calls, for example through PK.
 *
 * Module:  platform/threading.c
 *
 * This allows different threading implementations (built-in or
 * provided externally).
 *
 * You will have to enable either #MBEDTLS_THREADING_ALT or
 * #MBEDTLS_THREADING_PTHREAD.
 *
 * Enable this layer to allow use of mutexes within Mbed TLS
 */
//#define MBEDTLS_THREADING_C

/* Memory buffer allocator options */
//#define MBEDTLS_MEMORY_ALIGN_MULTIPLE      4 /**< Align on multiples of this value */

/* To use the following function macros, MBEDTLS_PLATFORM_C must be enabled. */
/* MBEDTLS_PLATFORM_XXX_MACRO and MBEDTLS_PLATFORM_XXX_ALT cannot both be defined */
//#define MBEDTLS_PLATFORM_CALLOC_MACRO        calloc /**< Default allocator macro to use, can be undefined. See MBEDTLS_PLATFORM_STD_CALLOC for requirements. */
//#define MBEDTLS_PLATFORM_EXIT_MACRO            exit /**< Default exit macro to use, can be undefined */
//#define MBEDTLS_PLATFORM_FREE_MACRO            free /**< Default free macro to use, can be undefined. See MBEDTLS_PLATFORM_STD_FREE for requirements. */
//#define MBEDTLS_PLATFORM_FPRINTF_MACRO      fprintf /**< Default fprintf macro to use, can be undefined */
//#define MBEDTLS_PLATFORM_MS_TIME_TYPE_MACRO   int64_t //#define MBEDTLS_PLATFORM_MS_TIME_TYPE_MACRO   int64_t /**< Default milliseconds time macro to use, can be undefined. MBEDTLS_HAVE_TIME must be enabled. It must be signed, and at least 64 bits. If it is changed from the default, MBEDTLS_PRINTF_MS_TIME must be updated to match.*/
//#define MBEDTLS_PLATFORM_NV_SEED_READ_MACRO   mbedtls_platform_std_nv_seed_read /**< Default nv_seed_read function to use, can be undefined */
//#define MBEDTLS_PLATFORM_NV_SEED_WRITE_MACRO  mbedtls_platform_std_nv_seed_write /**< Default nv_seed_write function to use, can be undefined */
//#define MBEDTLS_PLATFORM_PRINTF_MACRO        printf /**< Default printf macro to use, can be undefined */
//#define MBEDTLS_PLATFORM_SETBUF_MACRO      setbuf /**< Default setbuf macro to use, can be undefined */
/* Note: your snprintf must correctly zero-terminate the buffer! */
//#define MBEDTLS_PLATFORM_SNPRINTF_MACRO    snprintf /**< Default snprintf macro to use, can be undefined */

/** \def MBEDTLS_PLATFORM_STD_CALLOC
 *
 * Default allocator to use, can be undefined.
 * It must initialize the allocated buffer memory to zeroes.
 * The size of the buffer is the product of the two parameters.
 * The calloc function returns either a null pointer or a pointer to the allocated space.
 * If the product is 0, the function may either return NULL or a valid pointer to an array of size 0 which is a valid input to the deallocation function.
 * An uninitialized #MBEDTLS_PLATFORM_STD_CALLOC always fails, returning a null pointer.
 * See the description of #MBEDTLS_PLATFORM_MEMORY for more details.
 * The corresponding deallocation function is #MBEDTLS_PLATFORM_STD_FREE.
 */
//#define MBEDTLS_PLATFORM_STD_CALLOC        calloc

//#define MBEDTLS_PLATFORM_STD_EXIT            exit /**< Default exit to use, can be undefined */
//#define MBEDTLS_PLATFORM_STD_EXIT_FAILURE       1 /**< Default exit value to use, can be undefined */
//#define MBEDTLS_PLATFORM_STD_EXIT_SUCCESS       0 /**< Default exit value to use, can be undefined */
//#define MBEDTLS_PLATFORM_STD_FPRINTF      fprintf /**< Default fprintf to use, can be undefined */

/** \def MBEDTLS_PLATFORM_STD_FREE
 *
 * Default free to use, can be undefined.
 * NULL is a valid parameter, and the function must do nothing.
 * A non-null parameter will always be a pointer previously returned by #MBEDTLS_PLATFORM_STD_CALLOC and not yet freed.
 * An uninitialized #MBEDTLS_PLATFORM_STD_FREE does not do anything.
 * See the description of #MBEDTLS_PLATFORM_MEMORY for more details (same principles as for MBEDTLS_PLATFORM_STD_CALLOC apply).
 */
//#define MBEDTLS_PLATFORM_STD_FREE            free

//#define MBEDTLS_PLATFORM_STD_MEM_HDR   <stdlib.h> /**< Header to include if MBEDTLS_PLATFORM_NO_STD_FUNCTIONS is defined. Don't define if no header is needed. */
//#define MBEDTLS_PLATFORM_STD_NV_SEED_FILE  "seedfile" /**< Seed file to read/write with default implementation */
//#define MBEDTLS_PLATFORM_STD_NV_SEED_READ   mbedtls_platform_std_nv_seed_read /**< Default nv_seed_read function to use, can be undefined */
//#define MBEDTLS_PLATFORM_STD_NV_SEED_WRITE  mbedtls_platform_std_nv_seed_write /**< Default nv_seed_write function to use, can be undefined */
//#define MBEDTLS_PLATFORM_STD_PRINTF        printf /**< Default printf to use, can be undefined */
//#define MBEDTLS_PLATFORM_STD_SETBUF      setbuf /**< Default setbuf to use, can be undefined */
/* Note: your snprintf must correctly zero-terminate the buffer! */
//#define MBEDTLS_PLATFORM_STD_SNPRINTF    snprintf /**< Default snprintf to use, can be undefined */
//#define MBEDTLS_PLATFORM_STD_TIME            time /**< Default time to use, can be undefined. MBEDTLS_HAVE_TIME must be enabled */
//#define MBEDTLS_PLATFORM_TIME_MACRO            time /**< Default time macro to use, can be undefined. MBEDTLS_HAVE_TIME must be enabled */
//#define MBEDTLS_PLATFORM_TIME_TYPE_MACRO       time_t /**< Default time macro to use, can be undefined. MBEDTLS_HAVE_TIME must be enabled */
//#define MBEDTLS_PLATFORM_VSNPRINTF_MACRO    vsnprintf /**< Default vsnprintf macro to use, can be undefined */
//#define MBEDTLS_PRINTF_MS_TIME    PRId64 /**< Default fmt for printf. That's avoid compiler warning if mbedtls_ms_time_t is redefined */

/** \def MBEDTLS_PLATFORM_DEV_RANDOM
 *
 * Path to a special file that returns cryptographic-quality random bytes
 * when read. This is used by the default platform entropy source on
 * non-Windows platforms unless a dedicated system call is available
 * (see #MBEDTLS_PSA_BUILTIN_GET_ENTROPY).
 *
 * The default value is `/dev/random`, which is suitable on most platforms
 * other than Linux. On Linux, either `/dev/random` or `/dev/urandom`
 * may be the right choice, depending on the circumstances:
 *
 * - If possible, the library will use the getrandom() system call,
 *   which is preferable, and #MBEDTLS_PLATFORM_DEV_RANDOM is not used.
 * - If there is a dedicated hardware entropy source (e.g. RDRAND on x86
 *   processors), then both `/dev/random` and `/dev/urandom` are fine.
 * - `/dev/random` is always secure. However, with kernels older than 5.6,
 *   `/dev/random` often blocks unnecessarily if there is no dedicated
 *   hardware entropy source.
 * - `/dev/urandom` never blocks. However, it may return predictable data
 *   if it is used early after the kernel boots, especially on embedded
 *   devices without an interactive user.
 *
 * Thus you should change the value to `/dev/urandom` if your application
 * definitely won't be used on a device running Linux without a dedicated
 * entropy source early during or after boot.
 *
 *
 * This is the default value of ::mbedtls_platform_dev_random, which
 * can be changed at run time.
 */
//#define MBEDTLS_PLATFORM_DEV_RANDOM "/dev/random"

/** \} name SECTION: Platform abstraction layer */

/**
 * \name SECTION: General and test configuration options
 *
 * This section sets test specific settings.
 * \{
 */

/**
 * \def MBEDTLS_CHECK_RETURN_WARNING
 *
 * If this macro is defined, emit a compile-time warning if application code
 * calls a function without checking its return value, but the return value
 * should generally be checked in portable applications.
 *
 * This is only supported on platforms where #MBEDTLS_CHECK_RETURN is
 * implemented. Otherwise this option has no effect.
 *
 * Uncomment to get warnings on using fallible functions without checking
 * their return value.
 *
 * \note  This feature is a work in progress.
 *        Warnings will be added to more functions in the future.
 *
 * \note  A few functions are considered critical, and ignoring the return
 *        value of these functions will trigger a warning even if this
 *        macro is not defined. To completely disable return value check
 *        warnings, define #MBEDTLS_CHECK_RETURN with an empty expansion.
 */
//#define MBEDTLS_CHECK_RETURN_WARNING

/**
 * \def MBEDTLS_DEPRECATED_WARNING
 *
 * Mark deprecated functions and features so that they generate a warning if
 * used. Functionality deprecated in one version will usually be removed in the
 * next version. You can enable this to help you prepare the transition to a
 * new major version by making sure your code is not using this functionality.
 *
 * This only works with GCC and Clang. With other compilers, you may want to
 * use MBEDTLS_DEPRECATED_REMOVED
 *
 * Uncomment to get warnings on using deprecated functions and features.
 */
//#define MBEDTLS_DEPRECATED_WARNING

/**
 * \def MBEDTLS_DEPRECATED_REMOVED
 *
 * Remove deprecated functions and features so that they generate an error if
 * used. Functionality deprecated in one version will usually be removed in the
 * next version. You can enable this to help you prepare the transition to a
 * new major version by making sure your code is not using this functionality.
 *
 * Uncomment to get errors on using deprecated functions and features.
 */
//#define MBEDTLS_DEPRECATED_REMOVED

/** \def MBEDTLS_CHECK_RETURN
 *
 * This macro is used at the beginning of the declaration of a function
 * to indicate that its return value should be checked. It should
 * instruct the compiler to emit a warning or an error if the function
 * is called without checking its return value.
 *
 * There is a default implementation for popular compilers in platform_util.h.
 * You can override the default implementation by defining your own here.
 *
 * If the implementation here is empty, this will effectively disable the
 * checking of functions' return values.
 */
[[nodiscard]] //#define MBEDTLS_CHECK_RETURN

/** \def MBEDTLS_IGNORE_RETURN
 *
 * This macro requires one argument, which should be a C function call.
 * If that function call would cause a #MBEDTLS_CHECK_RETURN warning, this
 * warning is suppressed.
 */
//#define MBEDTLS_IGNORE_RETURN( result ) ((void) !(result))

/**
 * \def TF_PSA_CRYPTO_CONFIG_FILE
 *
 * If defined, this is a header which will be included instead of
 * `"psa/crypto_config.h"`.
 * This header file specifies which cryptographic mechanisms are available
 * through the PSA API.
 *
 * This macro is expanded after an <tt>\#include</tt> directive. This is a popular but
 * non-standard feature of the C language, so this feature is only available
 * with compilers that perform macro expansion on an <tt>\#include</tt> line.
 *
 * The value of this symbol is typically a path in double quotes, either
 * absolute or relative to a directory on the include search path.
 */
//#define TF_PSA_CRYPTO_CONFIG_FILE "psa/crypto_config.h"

/**
 * \def TF_PSA_CRYPTO_USER_CONFIG_FILE
 *
 * If defined, this is a header which will be included after
 * `"psa/crypto_config.h"` or #TF_PSA_CRYPTO_CONFIG_FILE.
 * This allows you to modify the default configuration, including the ability
 * to undefine options that are enabled by default.
 *
 * This macro is expanded after an <tt>\#include</tt> directive. This is a popular but
 * non-standard feature of the C language, so this feature is only available
 * with compilers that perform macro expansion on an <tt>\#include</tt> line.
 *
 * The value of this symbol is typically a path in double quotes, either
 * absolute or relative to a directory on the include search path.
 */
//#define TF_PSA_CRYPTO_USER_CONFIG_FILE "/dev/null"

/**
 * @brief MBEDTLS SELF TEST.
 *
 * \def MBEDTLS_SELF_TEST
 *
 * Enable the checkup functions (*_self_test).
 */
#define MBEDTLS_SELF_TEST

/**
 * \def MBEDTLS_TEST_CONSTANT_FLOW_MEMSAN
 *
 * Enable testing of the constant-flow nature of some sensitive functions with
 * clang's MemorySanitizer. This causes some existing tests to also test
 * this non-functional property of the code under test.
 *
 * This setting requires compiling with clang -fsanitize=memory. The test
 * suites can then be run normally.
 *
 * \warning This macro is only used for extended testing; it is not considered
 * part of the library's API, so it may change or disappear at any time.
 *
 * Uncomment to enable testing of the constant-flow nature of selected code.
 */
//#define MBEDTLS_TEST_CONSTANT_FLOW_MEMSAN

/**
 * \def MBEDTLS_TEST_CONSTANT_FLOW_VALGRIND
 *
 * Enable testing of the constant-flow nature of some sensitive functions with
 * valgrind's memcheck tool. This causes some existing tests to also test
 * this non-functional property of the code under test.
 *
 * This setting requires valgrind headers for building, and is only useful for
 * testing if the tests suites are run with valgrind's memcheck. This can be
 * done for an individual test suite with 'valgrind ./test_suite_xxx', or when
 * using CMake, this can be done for all test suites with 'make memcheck'.
 *
 * \warning This macro is only used for extended testing; it is not considered
 * part of the library's API, so it may change or disappear at any time.
 *
 * Uncomment to enable testing of the constant-flow nature of selected code.
 */
//#define MBEDTLS_TEST_CONSTANT_FLOW_VALGRIND

/**
 * \def MBEDTLS_TEST_HOOKS
 *
 * Enable features for invasive testing such as introspection functions and
 * hooks for fault injection. This enables additional unit tests.
 *
 * Merely enabling this feature should not change the behavior of the product.
 * It only adds new code, and new branching points where the default behavior
 * is the same as when this feature is disabled.
 * However, this feature increases the attack surface: there is an added
 * risk of vulnerabilities, and more gadgets that can make exploits easier.
 * Therefore this feature must never be enabled in production.
 *
 * See `docs/architecture/testing/mbed-crypto-invasive-testing.md` for more
 * information.
 *
 * Uncomment to enable invasive tests.
 */
//#define MBEDTLS_TEST_HOOKS

/**
 * @brief TF PSA CRYPTO VERSION.
 *
 * \def TF_PSA_CRYPTO_VERSION
 *
 * Enable run-time version information.
 *
 * This option enables functions for getting the version of TF-PSA-Crypto
 * at runtime defined in include/tf-psa-crypto/version.h.
 */
#define TF_PSA_CRYPTO_VERSION

/** \} name SECTION: General and test configuration options */
