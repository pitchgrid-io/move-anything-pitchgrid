/*
 * Hook Engine — ARM64 Runtime Function Hooking via Pattern Scan
 *
 * Scans executable memory for a known byte pattern (needle), then installs
 * a trampoline-based function hook at the match location. This makes hooks
 * resilient to firmware updates — the code stays the same, only its address
 * may move.
 *
 * Usage:
 *   1. Define a hook_spec_t with the needle bytes and your hook function
 *   2. Call hook_install() — scans r-x memory, hooks if exactly 1 match
 *   3. In your hook function, call original via handle->original_fn
 *   4. Call hook_remove() to restore original code
 */

#ifndef HOOK_ENGINE_H
#define HOOK_ENGINE_H

#include <stdint.h>
#include <stddef.h>

/* Printf-like logging callback. The engine doesn't own the log destination. */
typedef void (*hook_log_fn)(const char *fmt, ...);

/*
 * Hook specification — describes what to find and hook.
 *
 * needle:      Byte pattern to search for in executable memory.
 *              Must start at the function entry point (we overwrite the
 *              first 16 bytes with a redirect trampoline).
 * mask:        Per-byte mask for the needle. 0xFF = must match exactly,
 *              0x00 = wildcard (any byte accepted). NULL means all-0xFF
 *              (exact match for every byte). Use wildcards for PC-relative
 *              fields (BL offsets, ADRP immediates) that change when the
 *              function moves.
 * needle_len:  Length of needle (and mask, if non-NULL). Must be >= 16.
 *              Longer needles reduce false-positive risk but we only ever
 *              overwrite the first 16.
 * hook_fn:     Your replacement function. Will be called instead of the
 *              original. Cast to void* for storage.
 * name:        Human-readable name for log messages (e.g. "pad_to_note").
 */
typedef struct {
    const uint8_t *needle;
    const uint8_t *mask;
    size_t needle_len;
    void *hook_fn;
    const char *name;
} hook_spec_t;

/*
 * Hook handle — tracks an installed hook's state.
 * Caller allocates this (typically as a static global) so the hook function
 * can access original_fn without indirection.
 */
typedef struct {
    uint8_t *target_addr;       /* Runtime address where needle was found */
    uint8_t saved_bytes[16];    /* Original first 16 bytes (for unhook) */
    void *trampoline;           /* mmap'd executable page */
    size_t trampoline_size;     /* mmap allocation size (for munmap) */
    void *original_fn;          /* Callable pointer to original via trampoline */
    int installed;              /* 1 if hook is active, 0 otherwise */
} hook_handle_t;

/*
 * Scan r-x memory regions for needle, install trampoline hook at match.
 *
 * Requires exactly 1 match in executable memory. If 0 or >1 matches
 * are found, the hook is not installed and -1 is returned.
 *
 * On success, handle->original_fn is set to a callable pointer that
 * invokes the original function (via trampoline).
 *
 * Returns 0 on success, -1 on failure.
 */
int hook_install(const hook_spec_t *spec, hook_handle_t *handle, hook_log_fn log);

/*
 * Remove a previously installed hook.
 * Restores original bytes, flushes icache, frees trampoline.
 * Safe to call if hook was never installed (no-op).
 */
void hook_remove(hook_handle_t *handle, hook_log_fn log);

#endif /* HOOK_ENGINE_H */
