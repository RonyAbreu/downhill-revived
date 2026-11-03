/* Guest linear memory on the Nintendo Switch (finding 144): guest_memory.c's reserve-and-commit
   without its aliased RAM views. Horizon gives a homebrew no shared memory object to map several
   times, and no way to reserve address space by itself; what it has is svcMapMemory, which moves
   heap pages to an address in the process's stack region. So the guest's address space is a
   range of the stack region (taken from libnx's virtmem, so no thread stack lands in it), and
   committing a span maps fresh, zeroed heap memory there. Only what the game uses costs memory:
   RAM, the module's own memory and what its owners commit (scratchpad and hardware windows, EE
   stack slots, the bridge arena), about as on the other targets, instead of the 1.8 GiB the whole
   range would be (and that not even title takeover has as one allocation).

   What this changes against guest_memory.c:
   - The six RAM views are not separate mappings of one section. Only canonical RAM
     (0x08000000) is mapped; the module folds every access that stays inside one 32 MiB window
     onto it (memory_access.h), and host code does the same through GUEST() (guest.h, under
     __SWITCH__). A span that crosses a window's edge into a view faults; memory_access.h counts
     those (host64_switch_alias_crossings) first.
   - Commits are whole GRANULEs (few, large mappings: the kernel's memory blocks are a limited
     resource), so the null page and the EE stack slots' guard pages end up mapped with their
     neighbours: an overflow there is not caught.
   - A fault on an uncommitted address is a CPU exception: host_switch.c's handler logs it (the
     host address; guest = host - guest_base) and Horizon ends the program. */
#include <switch.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wasm-rt.h"
#include "core_host.h"
#include "memory_layout.h"

#define GRANULE (1u << 20)        /* what one commit maps at least */
#define HEADROOM (64ull << 20)    /* room for memory.grow when the module's maximum is not reservable */

uintptr_t guest_base;
static wasm_rt_memory_t *owner;
static uint64_t reserved;         /* bytes of address space at guest_base */
static uint8_t *committed;        /* one flag per GRANULE */
static Mutex commit_lock;         /* commits come from the game thread and the host's */
unsigned long long host64_switch_alias_crossings;

static void fail(const char *what)
{
    fprintf(stderr, "host64 memory: %s\n", what);
    abort();
}

static uint64_t info(InfoType type)
{
    uint64_t value = 0;
    svcGetInfo(&value, type, CUR_PROCESS_HANDLE, 0);
    return value;
}

/* [lo, hi), granule-aligned and none of it committed: one mapping of zeroed heap memory, which
   the process then never touches at its own address (svcMapMemory makes it inaccessible there). */
static int map_run(uint64_t lo, uint64_t hi)
{
    size_t size = (size_t)(hi - lo);
    void *source = aligned_alloc(0x1000, size);
    if (!source) return 0;
    memset(source, 0, size);
    if (R_FAILED(svcMapMemory((void *)(guest_base + lo), source, size))) {
        free(source);
        return 0;
    }
    return 1;
}

static int commit_range(uint64_t lo, uint64_t hi)
{
    lo &= ~(uint64_t)(GRANULE - 1);
    hi = (hi + GRANULE - 1) & ~(uint64_t)(GRANULE - 1);
    if (hi > reserved) return 0;
    int ok = 1;
    mutexLock(&commit_lock);
    for (uint64_t at = lo; at < hi && ok;) {
        if (committed[at / GRANULE]) {
            at += GRANULE;
            continue;
        }
        uint64_t end = at;
        while (end < hi && !committed[end / GRANULE]) end += GRANULE;
        if (!map_run(at, end)) {
            fprintf(stderr, "host64 memory: commit of %llu MiB at guest %08llX failed (process %llu of %llu MiB used)\n",
                    (unsigned long long)((end - at) >> 20), (unsigned long long)at,
                    (unsigned long long)(info(InfoType_UsedMemorySize) >> 20),
                    (unsigned long long)(info(InfoType_TotalMemorySize) >> 20));
            ok = 0;
            break;
        }
        for (; at < end; at += GRANULE) committed[at / GRANULE] = 1;
    }
    mutexUnlock(&commit_lock);
    return ok;
}

int host64_memory_commit(uint64_t at, uint64_t size)
{
    if (!owner || at + size > owner->size || at + size < at) return 0;
    return commit_range(at, at + size);
}

int host64_memory_commit_stack(uint32_t sp)
{
    if (sp > HOST64_MODULE_BASE) return 1;  /* inside the module's committed memory */
    return sp > HOST64_NULL_GUARD && sp <= 0x00400000 &&
           host64_memory_commit(HOST64_NULL_GUARD, sp - HOST64_NULL_GUARD);
}

void wasm_rt_allocate_memory(wasm_rt_memory_t *m, uint64_t initial, uint64_t max, bool is64, uint32_t page_size)
{
    if (owner || is64 || page_size != 65536 || initial < HOST64_MIN_MEMORY_SIZE / 65536 || initial > max ||
        max > 65536 || initial * 65536 <= HOST64_MODULE_BASE)
        fail("unsupported module memory layout");
    /* the module's whole maximum if the stack region has room, else what it starts with and some */
    const uint64_t sizes[] = {max * 65536, initial * 65536 + HEADROOM, initial * 65536};
    void *base = NULL;
    virtmemLock();
    for (unsigned i = 0; i < 3 && !base; i++) {
        reserved = (sizes[i] + GRANULE - 1) & ~(uint64_t)(GRANULE - 1);
        if ((base = virtmemFindStack(reserved, 0)) && !virtmemAddReservation(base, reserved)) base = NULL;
    }
    virtmemUnlock();
    fprintf(stderr, "host64 memory: process %llu of %llu MiB used, stack region %llu MiB\n",
            (unsigned long long)(info(InfoType_UsedMemorySize) >> 20),
            (unsigned long long)(info(InfoType_TotalMemorySize) >> 20),
            (unsigned long long)(info(InfoType_StackRegionSize) >> 20));
    if (!base) fail("no room for the guest's address space in the stack region");
    guest_base = (uintptr_t)base;
    if (!(committed = calloc((size_t)(reserved / GRANULE), 1))) fail("commit map");
    owner = m;  /* host64_memory_commit's bounds */
    m->size = initial * 65536;
    if (!commit_range(0x08000000, 0x0a000000) || !commit_range(HOST64_MODULE_BASE, initial * 65536))
        fail("RAM or module commit");
    m->data = (uint8_t *)base;
    m->data_end = m->data + m->size;
    m->pages = initial;
    m->max_pages = max;
    m->is64 = false;
    m->page_size = page_size;
    fprintf(stderr, "host64 memory: %llu MiB of address space at %p, RAM and the module's %llu MiB committed\n",
            (unsigned long long)(reserved >> 20), base,
            (unsigned long long)((initial * 65536 - HOST64_MODULE_BASE) >> 20));
}

uint64_t wasm_rt_grow_memory(wasm_rt_memory_t *m, uint64_t pages)
{
    if (m != owner || pages > m->max_pages - m->pages) return UINT32_MAX;
    uint64_t old = m->pages, end = (old + pages) * 65536;
    if (end > reserved || !commit_range(m->size, end)) return UINT32_MAX;
    m->pages += pages;
    m->size = end;
    m->data_end = m->data + end;
    return old;
}

/* At the program's end only: the mappings and the heap behind them go with the process. */
void wasm_rt_free_memory(wasm_rt_memory_t *m)
{
    if (m != owner) fail("free of non-owner");
    guest_base = 0;
    owner = NULL;
    m->data = m->data_end = NULL;
    m->size = m->pages = 0;
}
