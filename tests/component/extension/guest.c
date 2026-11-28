// SPDX-License-Identifier: Apache-2.0
//
// The extension world's C guest (plan P11 Task 21): the bindings `wit-bindgen c` generates from
// extension.wit, built with wasi-sdk's clang for wasm32-unknown-unknown without any libc. It brings its
// own bump allocator and the few C library functions the bindings call, and behaves exactly as
// extension.wat does, so one host test drives both (src/isoline/component/api/Extension_test.cpp).
// build-guests.sh builds it into guest.wasm; no build of Isoline ever does.

#include <stddef.h>
#include <stdint.h>

#include "extension.h"

// The C library the bindings call (stdlib.h, string.h), on a bump allocator that never frees: the guest
// lives only as long as one test's store.

extern unsigned char __heap_base;

static uintptr_t heapNext = 0;
static uintptr_t heapEnd = 0;

void abort(void)
{
    __builtin_trap();
}

void* malloc(size_t size)
{
    if (heapNext == 0)
    {
        heapNext = (uintptr_t) &__heap_base;
        heapEnd = __builtin_wasm_memory_size(0) * 65536;
    }
    uintptr_t const start = (heapNext + 7) & ~(uintptr_t) 7;
    if (size > SIZE_MAX - start)
        abort();
    uintptr_t const end = start + size;
    if (end > heapEnd)
    {
        size_t const pages = (end - heapEnd + 65535) / 65536;
        if (__builtin_wasm_memory_grow(0, pages) == SIZE_MAX)
            abort();
        heapEnd += pages * 65536;
    }
    heapNext = end;
    return (void*) start;
}

void free(void* pointer)
{
    (void) pointer;
}

void* memcpy(void* destination, void const* source, size_t count)
{
    unsigned char* to = destination;
    unsigned char const* from = source;
    for (size_t index = 0; index < count; ++index)
        to[index] = from[index];
    return destination;
}

void* realloc(void* pointer, size_t size)
{
    // The old size is unknown to a bump allocator, so a grown block copies what the caller may read:
    // the bindings only ever pass a null pointer.
    if (pointer != NULL)
        abort();
    return malloc(size);
}

/// Whether cabi_realloc misbehaves (ModeBadRealloc).
static bool isMisbehaving = false;

/// The canonical ABI's allocator, in place of the bindings' weak one: the heap, aligned, or a block past the
/// memory's end while the guest misbehaves.
__attribute__((__export_name__("cabi_realloc"))) void* cabi_realloc(void* pointer,
                                                                    size_t oldSize,
                                                                    size_t alignment,
                                                                    size_t newSize)
{
    (void) oldSize;
    if (isMisbehaving)
        return (void*) 0xfffffff0;
    if (newSize == 0)
        return (void*) alignment;
    // A grown block would have to copy the old one: the canonical ABI only ever allocates afresh. malloc()
    // aligns to 8, the most any component value needs.
    if (pointer != NULL || alignment > 8)
        abort();
    return malloc(newSize);
}

size_t strlen(char const* text)
{
    size_t length = 0;
    while (text[length] != '\0')
        ++length;
    return length;
}

// What the guest keeps between calls.

/// How render() behaves, as the configuration's `mode` entry chose at load.
/// How the guest behaves, as the configuration's `mode` entry chose at load (README.md); the values are
/// the indices of modeNames.
typedef enum
{
    ModeNormal,     ///< Draws the title.
    ModeHostile,    ///< render() returns a list<u8> of 2^28 - 1 bytes its memory does not hold.
    ModeSpin,       ///< render() never returns: logs `spinning`, then loops until the host interrupts it.
    ModeLarge,      ///< render() returns 4 MiB its memory holds.
    ModeTrap,       ///< render() traps (unreachable).
    ModeHoard,      ///< paint() keeps the surface it opens.
    ModeBadRealloc, ///< cabi_realloc hands out a block past the memory's end, from load's state() on.
    ModeSpinLoad,   ///< load() never returns, as render() in ModeSpin.
} Mode;

/// The modes' names, by Mode.
static char const* const modeNames[] = { "normal", "hostile", "spin",        "large",
                                         "trap",   "hoard",   "bad-realloc", "spin-load" };

static extension_string_t title = { NULL, 0 };
static Mode mode = ModeNormal;

/// A copy of @p count bytes at @p bytes, on the guest's heap.
static uint8_t* copyOf(uint8_t const* bytes, size_t count)
{
    uint8_t* const copy = malloc(count);
    memcpy(copy, bytes, count);
    return copy;
}

/// The concatenation of @p first and @p second, as a string on the guest's heap.
static extension_string_t joined(char const* first, extension_string_t const* second)
{
    size_t const firstLength = strlen(first);
    extension_string_t result = { malloc(firstLength + second->len), firstLength + second->len };
    memcpy(result.ptr, first, firstLength);
    memcpy(result.ptr + firstLength, second->ptr, second->len);
    return result;
}

/// @p name, `=`, then @p value, as a string on the guest's heap.
static extension_string_t assigned(extension_string_t const* name, extension_string_t const* value)
{
    extension_string_t result = { malloc(name->len + 1 + value->len), name->len + 1 + value->len };
    memcpy(result.ptr, name->ptr, name->len);
    result.ptr[name->len] = '=';
    memcpy(result.ptr + name->len + 1, value->ptr, value->len);
    return result;
}

/// Writes @p message, a C string, to the host's log at @p level.
static void logText(isoline_test_extension_host_log_level_t level, char const* message)
{
    extension_string_t text;
    extension_string_set(&text, message);
    isoline_test_extension_host_log(level, &text);
}

/// Whether @p text holds the C string @p expected.
static bool equals(extension_string_t const* text, char const* expected)
{
    size_t const length = strlen(expected);
    if (text->len != length)
        return false;
    for (size_t index = 0; index < length; ++index)
        if (text->ptr[index] != (uint8_t) expected[index])
            return false;
    return true;
}

/// The names of command-error's cases, in the WIT's order.
static char const* const commandErrorNames[] = { "not-declared", "pending", "denied", "failed", "timed-out" };

extension_event_kinds_t exports_extension_load(extension_list_tuple2_string_string_t* config)
{
    for (size_t index = 0; index < config->len; ++index)
    {
        extension_tuple2_string_string_t const* const entry = &config->ptr[index];
        if (!equals(&entry->f0, "mode"))
            continue;
        for (size_t named = 0; named < sizeof modeNames / sizeof modeNames[0]; ++named)
            if (equals(&entry->f1, modeNames[named]))
                mode = (Mode) named;
    }
    if (mode == ModeSpinLoad)
    {
        logText(ISOLINE_TEST_EXTENSION_HOST_LOG_LEVEL_TRACE, "spinning");
        for (uint32_t volatile turns = 0;; ++turns)
        {
        }
    }
    if (mode == ModeBadRealloc)
        isMisbehaving = true;

    logText(ISOLINE_TEST_EXTENSION_HOST_LOG_LEVEL_INFO, "load");

    isoline_test_extension_host_state_snapshot_t snapshot;
    isoline_test_extension_host_state(&snapshot);
    title = (extension_string_t) { copyOf(snapshot.title.ptr, snapshot.title.len), snapshot.title.len };
    for (size_t index = 0; index < snapshot.tabs.len; ++index)
    {
        isoline_test_extension_host_tab_t const* const tab = &snapshot.tabs.ptr[index];
        if (tab->active)
        {
            extension_string_t message = joined("active tab: ", &tab->title);
            isoline_test_extension_host_log(ISOLINE_TEST_EXTENSION_HOST_LOG_LEVEL_DEBUG, &message);
        }
    }
    for (size_t index = 0; index < snapshot.user_vars.len; ++index)
    {
        extension_tuple2_string_string_t const* const variable = &snapshot.user_vars.ptr[index];
        extension_string_t const name = joined("var: ", &variable->f0);
        extension_string_t message = assigned(&name, &variable->f1);
        isoline_test_extension_host_log(ISOLINE_TEST_EXTENSION_HOST_LOG_LEVEL_DEBUG, &message);
    }
    isoline_test_extension_host_state_snapshot_free(&snapshot);

    extension_string_t command;
    extension_string_set(&command, "status");
    isoline_test_extension_host_command_output_t output;
    isoline_test_extension_host_command_error_t error;
    if (isoline_test_extension_host_run_command(&command, config, &output, &error))
    {
        extension_string_t const stdoutText = { output.stdout_.ptr, output.stdout_.len };
        extension_string_t message = joined("status: ", &stdoutText);
        isoline_test_extension_host_log(ISOLINE_TEST_EXTENSION_HOST_LOG_LEVEL_INFO, &message);
        isoline_test_extension_host_command_output_free(&output);
    }
    else
    {
        extension_string_t name;
        extension_string_set(&name, commandErrorNames[error]);
        extension_string_t message = joined("status: ", &name);
        isoline_test_extension_host_log(ISOLINE_TEST_EXTENSION_HOST_LOG_LEVEL_WARN, &message);
    }

    extension_list_tuple2_string_string_free(config);

    isoline_test_extension_host_request_frame(250);
    return ISOLINE_TEST_EXTENSION_TYPES_EVENT_KINDS_TITLE | ISOLINE_TEST_EXTENSION_TYPES_EVENT_KINDS_CWD;
}

extension_render_request_t exports_extension_update(extension_event_t* event)
{
    if (event->tag != ISOLINE_TEST_EXTENSION_TYPES_EVENT_TITLE)
    {
        extension_event_free(event);
        return ISOLINE_TEST_EXTENSION_TYPES_RENDER_REQUEST_NONE;
    }
    title = (extension_string_t) { copyOf(event->val.title.ptr, event->val.title.len), event->val.title.len };
    extension_event_free(event);
    return ISOLINE_TEST_EXTENSION_TYPES_RENDER_REQUEST_RENDER;
}

void exports_extension_render(uint32_t cols, uint32_t rows, extension_list_u8_t* result)
{
    (void) rows;
    if (mode == ModeHostile)
    {
        // A length its memory does not hold: the host's lift must refuse it before it allocates.
        *result = (extension_list_u8_t) { (uint8_t*) 8, ((size_t) 1 << 28) - 1 };
        return;
    }
    if (mode == ModeSpin)
    {
        logText(ISOLINE_TEST_EXTENSION_HOST_LOG_LEVEL_TRACE, "spinning");
        for (uint32_t volatile turns = 0;; ++turns)
        {
        }
    }
    if (mode == ModeLarge)
    {
        // 4 MiB its memory holds, which the host's caps may refuse.
        result->len = (size_t) 4 << 20;
        result->ptr = malloc(result->len);
        return;
    }
    if (mode == ModeTrap)
        __builtin_trap();
    // `[title]`, the title cut to the surface's width less the brackets.
    size_t const room = cols >= 2 ? cols - 2 : 0;
    size_t const shown = title.len < room ? title.len : room;
    result->len = shown + 2;
    result->ptr = malloc(result->len);
    result->ptr[0] = '[';
    memcpy(result->ptr + 1, title.ptr, shown);
    result->ptr[shown + 1] = ']';
}

bool exports_extension_panes(extension_string_t* filter,
                             extension_list_pane_t* panes,
                             extension_control_error_t* error)
{
    // The import's list and the export's are one WIT type, which the bindings name twice.
    isoline_test_extension_control_list_pane_t found;
    if (!isoline_test_extension_control_list_panes(filter, &found, error))
        return false;
    *panes = (extension_list_pane_t) { found.ptr, found.len };
    return true;
}

void exports_extension_paint(extension_list_u8_t* bytes, extension_size_t* size)
{
    isoline_test_extension_host_own_surface_t const surface = isoline_test_extension_host_open_surface();
    isoline_test_extension_host_borrow_surface_t const lent =
        isoline_test_extension_host_borrow_surface(surface);
    if (mode == ModeHoard)
    {
        // A guest that keeps every surface it opens: one write, its size, and no drop.
        isoline_test_extension_host_method_surface_write(lent, bytes);
        isoline_test_extension_host_method_surface_size(lent, size);
        return;
    }
    for (int round = 0; round < 3; ++round)
    {
        extension_list_u8_t chunk = { bytes->ptr, bytes->len };
        isoline_test_extension_host_method_surface_write(lent, &chunk);
    }
    isoline_test_extension_host_method_surface_size(lent, size);
    isoline_test_extension_host_surface_drop_own(surface);
    isoline_test_extension_host_present(isoline_test_extension_host_open_surface());
    extension_list_u8_free(bytes);
}
