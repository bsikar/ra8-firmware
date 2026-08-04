/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file port/esp-hosted/inc/idf_compat/esp_event_base.h
 * @brief ESP-IDF-compatible event-base identity: the type and its two declarators.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * ``host/esp_hosted_os_abstraction.h`` includes ``esp_event_base.h`` by name,
 * and every vendored translation unit that touches the OS-abstraction vtable --
 * which is nearly all of them -- reaches it transitively. It is included for
 * one type: entry 66 of the vtable is
 * ``int (*_h_event_post)(esp_event_base_t event_base, int32_t event_id, ...)``.
 *
 * @par What an event base actually is
 * In ESP-IDF an event base is a ``const char*`` and nothing more. Its address
 * is the identity -- two subsystems are distinct because their base pointers
 * differ, not because their strings differ -- and the string it points at is
 * carried purely so a log line can name the subsystem. That is why
 * ::ESP_EVENT_DECLARE_BASE and ::ESP_EVENT_DEFINE_BASE exist as a pair: exactly
 * one translation unit defines the object, everyone else refers to it, and the
 * one definition is what makes the identity comparison meaningful.
 *
 * @par Why the DEFINE half is here even though the vendored tree never uses it
 * The tree declares ``ESP_HOSTED_EVENT`` in ``host/esp_hosted_event.h`` and
 * posts against it from four files -- ``esp_hosted_api.c``, ``rpc_wrap.c`` and
 * ``sdio_drv.c`` -- but never defines it. Upstream expects the surrounding
 * ESP-IDF application to. Here there is no such application, so the port must
 * supply the definition, and ::ESP_EVENT_DEFINE_BASE is the spelling that keeps
 * the definition and the declaration in the same shape. Leaving it out would
 * not remove the obligation, only the standard way of meeting it: the link
 * would fail on an undefined ``ESP_HOSTED_EVENT`` and the fix would be a
 * hand-written ``const char* ESP_HOSTED_EVENT = ...`` that no longer matches
 * the macro it is paired with.
 *
 * @par Symbols deliberately NOT defined here
 * ``ESP_EVENT_ANY_ID``, ``ESP_EVENT_ANY_BASE``, ``esp_event_handler_t``,
 * ``esp_event_loop_handle_t`` and the rest of ESP-IDF's event-loop API do not
 * appear anywhere in the vendored tree. The core never registers a handler; it
 * only posts, and it posts through the port's own vtable entry rather than
 * through ``esp_event_post()``. Adding them would be compatibility surface
 * nothing exercises.
 *
 * Note also that ``host/esp_hosted_event.h`` -- the file that uses
 * ::ESP_EVENT_DECLARE_BASE -- includes ``esp_event.h`` and ``esp_system.h``,
 * not this header. Those two are separate compatibility headers owned
 * elsewhere in this port; this file supplies what they are expected to expose.
 *
 * @since 0.1.0
 */

#pragma once

/* The type and macro spellings below are fixed by ESP-IDF: the vendored
   OS-abstraction vtable names esp_event_base_t in a function-pointer signature
   and esp_hosted_event.h invokes ESP_EVENT_DECLARE_BASE by name, so neither can
   take this project's conventions. clang-tidy's naming rule is suppressed
   across the block, following the ThreadX shim precedent in
   libs/ra8_wdt_supervisor and the sibling esp_log.h. */
/* NOLINTBEGIN(readability-identifier-naming) -- ESP-IDF-fixed spellings. */

/**
 * @typedef esp_event_base_t
 * @brief Identity of an event-producing subsystem.
 * @details A ``const char*``, exactly as in ESP-IDF. The pointer value is the
 * identity and the string it addresses is a human-readable name for logs; a
 * consumer compares bases with ``==``, never with ``strcmp``. The vendored core
 * uses exactly one, ``ESP_HOSTED_EVENT``, and passes it as the first argument
 * of the OS-abstraction vtable's ``_h_event_post`` entry.
 * @note The pointed-to string must have static storage duration -- an event
 * outlives the call that posted it, and the receiving side may format the name
 * long afterwards.
 * @see ESP_EVENT_DECLARE_BASE
 * @see ESP_EVENT_DEFINE_BASE
 * @since 0.1.0
 */
typedef const char* esp_event_base_t;

/* NOLINTEND(readability-identifier-naming) */

/**
 * @def ESP_EVENT_DECLARE_BASE
 * @brief Declare an event base defined in some other translation unit.
 * @details Expands to an ``extern`` declaration of a ``const``-qualified
 * ::esp_event_base_t object. Belongs in a header, so that every file posting
 * against the base refers to one object; the matching ::ESP_EVENT_DEFINE_BASE
 * belongs in exactly one ``.c``. The vendored ``host/esp_hosted_event.h``
 * invokes this once, for ``ESP_HOSTED_EVENT``.
 * @param[in] id Identifier naming the base, used verbatim as the object name.
 * @note Read-only build configuration. It declares an object, so it is a
 * declaration and needs its own semicolon at the call site -- upstream writes
 * ``ESP_EVENT_DECLARE_BASE(ESP_HOSTED_EVENT);``.
 * @warning A declaration with no matching definition anywhere in the link is
 *          only diagnosed at link time, and only if something actually posts
 *          against the base.
 * @par Example:
 * @code
 * ESP_EVENT_DECLARE_BASE(ESP_HOSTED_EVENT);
 * @endcode
 * @since 0.1.0
 */
#define ESP_EVENT_DECLARE_BASE(id) extern const esp_event_base_t id

/**
 * @def ESP_EVENT_DEFINE_BASE
 * @brief Define an event base, giving it its address and its name.
 * @details The definition half of ::ESP_EVENT_DECLARE_BASE, and the reason both
 * are here: the vendored tree declares ``ESP_HOSTED_EVENT`` and posts against
 * it but never defines it, leaving the definition to the integrator. It
 * initialises the object with the stringised identifier, so the base's log name
 * cannot drift from the symbol callers write.
 * @param[in] id Identifier naming the base. Must match the identifier passed to
 *               ::ESP_EVENT_DECLARE_BASE in the corresponding header.
 * @note Read-only build configuration. Place it in exactly one ``.c``; a second
 * definition is a duplicate-symbol error at link time, which is the intended
 * outcome -- two objects would give one logical base two identities.
 * @warning The object is ``const`` and has static storage duration, which is
 *          what lets a posted event outlive the poster. Do not reproduce this
 *          expansion with a non-static or non-const object.
 * @par Example:
 * @code
 * ESP_EVENT_DEFINE_BASE(ESP_HOSTED_EVENT);
 * @endcode
 * @since 0.1.0
 */
#define ESP_EVENT_DEFINE_BASE(id) const esp_event_base_t id = #id
