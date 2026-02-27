/*
 * Hook Engine — ARM64 Runtime Function Hooking via Pattern Scan
 *
 * Scans executable memory for a known byte pattern, then installs a
 * trampoline-based function hook at the match location.
 *
 * Platform notes:
 *   - Move-anything LD_PRELOAD shim intercepts mmap(length==4096) to
 *     detect the SPI mailbox. We use 2*page_size to avoid this trap.
 *   - SIGSEGV protection handles guard pages in mapped regions.
 *   - ARM64 split I/D caches require __builtin___clear_cache after patching.
 */

#include "hook_engine.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/mman.h>

/* ARM64 trampoline instructions */
#define ARM64_LDR_X16_PC8  0x58000050   /* LDR X16, [PC, #8] */
#define ARM64_BR_X16       0xD61F0200   /* BR X16             */

/* ── SIGSEGV guard ────────────────────────────────────────────────── */

static sigjmp_buf g_scan_jmpbuf;
static volatile sig_atomic_t g_scan_active = 0;

static void scan_segv_handler(int sig) {
    if (g_scan_active) {
        siglongjmp(g_scan_jmpbuf, 1);
    }
    /* Not our fault — re-raise with default handler */
    signal(sig, SIG_DFL);
    raise(sig);
}

static struct sigaction g_old_segv, g_old_bus;

static void install_segv_guard(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = scan_segv_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_old_segv);
    sigaction(SIGBUS, &sa, &g_old_bus);
}

static void restore_segv_guard(void) {
    sigaction(SIGSEGV, &g_old_segv, NULL);
    sigaction(SIGBUS, &g_old_bus, NULL);
}

/* ── Needle scanning ──────────────────────────────────────────────── */

/*
 * Masked byte comparison.
 * mask=NULL means exact match (all bytes compared).
 * mask[i]=0xFF means byte must match, mask[i]=0x00 means wildcard.
 */
static int masked_match(const uint8_t *data, const uint8_t *needle,
                        const uint8_t *mask, size_t len) {
    if (!mask) return memcmp(data, needle, len) == 0;

    for (size_t i = 0; i < len; i++) {
        if ((data[i] & mask[i]) != (needle[i] & mask[i]))
            return 0;
    }
    return 1;
}

/*
 * Scan a single memory region for the needle (with optional mask).
 * Returns the last match address, or NULL if not found.
 * Increments *count for each match found.
 */
static uint8_t *scan_region(const uint8_t *start, const uint8_t *end,
                            const uint8_t *needle, const uint8_t *mask,
                            size_t needle_len, int *count) {
    uint8_t *found = NULL;
    size_t region_len = (size_t)(end - start);

    if (region_len < needle_len) return NULL;

    /* ARM64 instructions are 4-byte aligned */
    for (const uint8_t *p = start;
         p <= end - needle_len;
         p += 4) {
        if (masked_match(p, needle, mask, needle_len)) {
            (*count)++;
            found = (uint8_t *)p;
        }
    }
    return found;
}

/*
 * Scan all r-x regions of MoveOriginal for the needle.
 * Returns the unique match address, or NULL if 0 or >1 matches.
 */
static uint8_t *scan_for_needle(const hook_spec_t *spec, hook_log_fn log) {
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) {
        log("[hook:%s] ERROR: cannot open /proc/self/maps\n", spec->name);
        return NULL;
    }

    install_segv_guard();

    char line[512];
    uint8_t *match = NULL;
    int total_matches = 0;
    int regions_scanned = 0;

    while (fgets(line, sizeof(line), maps)) {
        uintptr_t start, end;
        char perms[8] = {0};
        char path[256] = {0};

        int n = sscanf(line, "%lx-%lx %7s %*s %*s %*s %255[^\n]",
                       &start, &end, perms, path);
        if (n < 3) continue;

        /* Only scan r-x regions of MoveOriginal */
        if (perms[0] != 'r' || perms[2] != 'x') continue;
        if (!strstr(path, "MoveOriginal")) continue;

        regions_scanned++;
        size_t region_size = end - start;
        log("[hook:%s] Scanning r-x region 0x%lx-0x%lx (%zuKB) %s\n",
            spec->name, (unsigned long)start, (unsigned long)end,
            region_size / 1024, path);

        /* SIGSEGV-safe scan */
        if (sigsetjmp(g_scan_jmpbuf, 1) == 0) {
            g_scan_active = 1;
            int region_count = 0;
            uint8_t *region_match = scan_region(
                (const uint8_t *)start, (const uint8_t *)end,
                spec->needle, spec->mask, spec->needle_len,
                &region_count);
            g_scan_active = 0;

            if (region_count > 0) {
                log("[hook:%s]   Found %d match(es) in this region\n",
                    spec->name, region_count);
                total_matches += region_count;
                if (region_count == 1) match = region_match;
            }
        } else {
            g_scan_active = 0;
            log("[hook:%s]   SIGSEGV/SIGBUS in region — skipped\n", spec->name);
        }
    }

    restore_segv_guard();
    fclose(maps);

    log("[hook:%s] Scanned %d r-x region(s), %d total match(es)\n",
        spec->name, regions_scanned, total_matches);

    if (total_matches == 0) {
        log("[hook:%s] ERROR: needle not found in executable memory\n", spec->name);
        return NULL;
    }
    if (total_matches > 1) {
        log("[hook:%s] ERROR: needle found %d times (ambiguous) — refusing to hook\n",
            spec->name, total_matches);
        return NULL;
    }

    log("[hook:%s] Unique match at %p\n", spec->name, match);
    return match;
}

/* ── Hook installation ────────────────────────────────────────────── */

int hook_install(const hook_spec_t *spec, hook_handle_t *handle, hook_log_fn log) {
    memset(handle, 0, sizeof(*handle));

    if (!spec || !spec->needle || spec->needle_len < 16 || !spec->hook_fn) {
        log("[hook:%s] ERROR: invalid hook spec\n", spec ? spec->name : "?");
        return -1;
    }

    log("[hook:%s] Installing hook (needle: %zu bytes)\n",
        spec->name, spec->needle_len);

    /* Log first 16 bytes of needle for reference */
    log("[hook:%s] Needle: ", spec->name);
    for (size_t i = 0; i < (spec->needle_len < 32 ? spec->needle_len : 32); i++)
        log("%02x ", spec->needle[i]);
    log("\n");

    /* 1. Scan for the needle in executable memory */
    handle->target_addr = scan_for_needle(spec, log);
    if (!handle->target_addr) return -1;

    /* 2. Save original bytes */
    memcpy(handle->saved_bytes, handle->target_addr, 16);

    /* 3. Allocate executable trampoline page.
     *    IMPORTANT: Do NOT use size 4096 — the move-anything LD_PRELOAD
     *    shim intercepts mmap(length==4096) to detect the SPI mailbox. */
    long page_size = sysconf(_SC_PAGESIZE);
    handle->trampoline_size = (size_t)page_size * 2;
    handle->trampoline = mmap(NULL, handle->trampoline_size,
                              PROT_READ | PROT_WRITE | PROT_EXEC,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (handle->trampoline == MAP_FAILED) {
        log("[hook:%s] ERROR: mmap RWX failed: %s (errno=%d)\n",
            spec->name, strerror(errno), errno);
        handle->trampoline = NULL;
        handle->target_addr = NULL;
        return -1;
    }
    log("[hook:%s] Trampoline at %p (size=%zu)\n",
        spec->name, handle->trampoline, handle->trampoline_size);

    /* 4. Build "call-original" trampoline:
     *    [0-15]  original 4 instructions (saved bytes)
     *    [16-19] LDR X16, [PC, #8]
     *    [20-23] BR X16
     *    [24-31] absolute address of target+16 (continuation) */
    uint8_t *t = (uint8_t *)handle->trampoline;
    memcpy(t, handle->saved_bytes, 16);

    uint32_t ldr_x16 = ARM64_LDR_X16_PC8;
    uint32_t br_x16  = ARM64_BR_X16;
    memcpy(t + 16, &ldr_x16, 4);
    memcpy(t + 20, &br_x16, 4);

    uint64_t return_addr = (uint64_t)(handle->target_addr + 16);
    memcpy(t + 24, &return_addr, 8);

    __builtin___clear_cache((char *)t, (char *)(t + 32));

    handle->original_fn = handle->trampoline;
    log("[hook:%s] Trampoline ready → returns to %p\n",
        spec->name, (void *)return_addr);

    /* 5. Make the target code page writable */
    uintptr_t page_start = (uintptr_t)handle->target_addr & ~(page_size - 1);

    if (mprotect((void *)page_start, page_size,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        log("[hook:%s] ERROR: mprotect RWX failed: %s (errno=%d)\n",
            spec->name, strerror(errno), errno);
        munmap(handle->trampoline, handle->trampoline_size);
        handle->trampoline = NULL;
        handle->original_fn = NULL;
        handle->target_addr = NULL;
        return -1;
    }

    /* 6. Write redirect trampoline at target:
     *    [0-3]  LDR X16, [PC, #8]
     *    [4-7]  BR X16
     *    [8-15] absolute address of hook function */
    uint8_t hook_code[16];
    memcpy(hook_code + 0, &ldr_x16, 4);
    memcpy(hook_code + 4, &br_x16, 4);
    uint64_t hook_addr = (uint64_t)(uintptr_t)spec->hook_fn;
    memcpy(hook_code + 8, &hook_addr, 8);

    memcpy(handle->target_addr, hook_code, 16);

    /* 7. Flush icache + restore page protection */
    __builtin___clear_cache((char *)handle->target_addr,
                            (char *)(handle->target_addr + 16));
    mprotect((void *)page_start, page_size, PROT_READ | PROT_EXEC);

    handle->installed = 1;
    log("[hook:%s] INSTALLED at %p → hook at %p\n",
        spec->name, handle->target_addr, spec->hook_fn);

    return 0;
}

/* ── Hook removal ─────────────────────────────────────────────────── */

void hook_remove(hook_handle_t *handle, hook_log_fn log) {
    if (!handle->installed || !handle->target_addr) return;

    long page_size = sysconf(_SC_PAGESIZE);
    uintptr_t page_start = (uintptr_t)handle->target_addr & ~(page_size - 1);

    if (mprotect((void *)page_start, page_size,
                 PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
        memcpy(handle->target_addr, handle->saved_bytes, 16);
        __builtin___clear_cache((char *)handle->target_addr,
                                (char *)(handle->target_addr + 16));
        mprotect((void *)page_start, page_size, PROT_READ | PROT_EXEC);
        log("[hook] Restored original bytes at %p\n", handle->target_addr);
    } else {
        log("[hook] WARNING: mprotect failed during unhook: %s\n",
            strerror(errno));
    }

    if (handle->trampoline) {
        munmap(handle->trampoline, handle->trampoline_size);
        handle->trampoline = NULL;
    }

    handle->original_fn = NULL;
    handle->target_addr = NULL;
    handle->installed = 0;
    log("[hook] Hook removed.\n");
}
