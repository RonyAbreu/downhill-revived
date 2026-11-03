/* Guest linear memory on the Nintendo Switch (finding 144): one plain, zeroed allocation instead of
   guest_memory.c's 8 GiB reservation with aliased RAM views. Horizon gives a homebrew no shared
   memory object to map several times, and in title takeover libnx hands the process its whole
   heap at start, so a flat block costs nothing the process does not already hold.

   What this changes against guest_memory.c:
   - The six RAM views are not separate mappings of one section. The module folds every access
     that stays inside one 32 MiB window onto canonical RAM (memory_access.h), and host code does
     the same through GUEST() (guest.h, under __SWITCH__). Only a span that crosses a window's edge
     into a view would see this block's own bytes there instead of RAM's; memory_access.h counts
     those (host64_switch_alias_crossings) so a run shows whether it ever happens.
   - Nothing faults: there are no guard pages, so an out-of-range access is not reported with its
     guest address. Correct runs never make one (they never fault on the other targets either).
   - Commits succeed for any in-range span: the block is all committed.
   Growth stays inside the block: it is allocated with HEADROOM extra bytes, so the base never moves. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wasm-rt.h"
#include "core_host.h"
#include "memory_layout.h"

#define HEADROOM (256u << 20)  /* room for the module's memory.grow without moving the block */

uintptr_t guest_base;
static wasm_rt_memory_t *owner;
static uint64_t capacity;
unsigned long long host64_switch_alias_crossings;

static void fail(const char *what)
{
    fprintf(stderr, "host64 memory: %s\n", what);
    abort();
}

int host64_memory_commit(uint64_t at, uint64_t size)
{
    return owner && at + size <= owner->size && at + size >= at;
}

int host64_memory_commit_stack(uint32_t sp)
{
    return sp > HOST64_MODULE_BASE || (sp > HOST64_NULL_GUARD && sp <= 0x00400000);
}

void wasm_rt_allocate_memory(wasm_rt_memory_t *m, uint64_t initial, uint64_t max, bool is64, uint32_t page_size)
{
    if (owner || is64 || page_size != 65536 || initial < HOST64_MIN_MEMORY_SIZE / 65536 || initial > max ||
        max > 65536 || initial * 65536 <= HOST64_MODULE_BASE)
        fail("unsupported module memory layout");
    capacity = initial * 65536 + HEADROOM;
    if (capacity > max * 65536) capacity = max * 65536;
    void *block = aligned_alloc(HOST64_PAGE, (size_t)capacity);
    if (!block) {
        fprintf(stderr, "host64 memory: %llu MiB not available (start in title takeover: hold R on a game)\n",
                (unsigned long long)(capacity >> 20));
        abort();
    }
    memset(block, 0, (size_t)capacity);
    guest_base = (uintptr_t)block;
    m->data = (uint8_t *)block;
    m->size = initial * 65536;
    m->data_end = m->data + m->size;
    m->pages = initial;
    m->max_pages = max;
    m->is64 = false;
    m->page_size = page_size;
    owner = m;
}

uint64_t wasm_rt_grow_memory(wasm_rt_memory_t *m, uint64_t pages)
{
    if (m != owner || pages > m->max_pages - m->pages) return UINT32_MAX;
    uint64_t old = m->pages, end = (old + pages) * 65536;
    if (end > capacity) return UINT32_MAX;
    m->pages += pages;
    m->size = end;
    m->data_end = m->data + end;
    return old;
}

void wasm_rt_free_memory(wasm_rt_memory_t *m)
{
    if (m != owner) fail("free of non-owner");
    free(m->data);
    guest_base = 0;
    owner = NULL;
    m->data = m->data_end = NULL;
    m->size = m->pages = 0;
}
