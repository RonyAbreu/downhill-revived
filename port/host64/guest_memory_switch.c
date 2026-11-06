/* Guest linear memory on the Nintendo Switch (finding 144): guest_memory.c's reserve-and-commit
   without its aliased RAM views. The guest needs SPAN of contiguous address space (its memory runs
   up to the module's, above 0x70100000), of which it uses a few tens of MiB. Horizon has no way to
   reserve address space by itself, so it is found in one of two places:
   - Stack region: a free run of SPAN there, taken from libnx's virtmem (so no thread stack lands
     in it); committing a span maps fresh heap memory there with svcMapMemory. Only what the game
     uses costs memory, as on the other targets. The main thread's stack sits at a random place in
     the 2 GiB region, so such a run is often not there.
   - Heap: the homebrew loader hands the program its whole heap (title takeover: about 3.1 GiB,
     already the process's memory), and __libnx_initheap below keeps its first SPAN for the guest
     before newlib's malloc gets the rest. Committing a span zeroes it on first use.
   Which one: __libnx_initheap looks before main, while the stack region is still empty but for
   the main thread's stack; dhd.log says which, and why.

   What this changes against guest_memory.c:
   - The six RAM views are not separate mappings of one section. Only canonical RAM (0x08000000)
     is backed; the module folds every access that stays inside one 32 MiB window onto it
     (memory_access.h), and host code does the same through GUEST() (guest.h, under __SWITCH__).
     A span crossing a window's edge into a view faults in the stack region and reads zeros in
     the heap; memory_access.h counts those (host64_switch_alias_crossings).
   - Commits are whole GRANULEs (few, large mappings: the kernel's memory blocks are a limited
     resource), so the null page and the EE stack slots' guard pages are backed with their
     neighbours: an overflow there is not caught.
   - In the stack region, an access to an uncommitted address is a CPU exception: host_switch.c's
     handler logs it (the host address; guest = host - guest_base) and Horizon ends the program.
     In the heap nothing faults. */
#include <switch.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wasm-rt.h"
#include "core_host.h"
#include "memory_layout.h"

#define SPAN 0x74000000ull        /* the module's initial memory (0x70E00000 today) and room to grow */
#define GRANULE (1u << 20)        /* what one commit backs at least */
#define HEAP_LEFT (512ull << 20)  /* malloc's least share when the guest takes the heap's start */

uintptr_t guest_base;
static wasm_rt_memory_t *owner;
static uintptr_t span_base;       /* SPAN of address space, found before main */
static int in_heap;               /* span_base is the heap's start (else the stack region's) */
static VirtmemReservation *span_reservation;   /* the stack region's run, while it is the guest's */
static char span_why[160];        /* for dhd.log: main has no log yet when the choice is made */
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

/* The lowest free run of `need` bytes in the stack region, page-aligned, or 0. */
static uintptr_t stack_region_run(uint64_t need, uint64_t *largest)
{
    uintptr_t at = info(InfoType_StackRegionAddress), end = at + info(InfoType_StackRegionSize);
    *largest = 0;
    while (at < end) {
        MemoryInfo m;
        u32 page;
        if (R_FAILED(svcQueryMemory(&m, &page, at)) || m.addr + m.size <= at) break;
        uintptr_t lo = m.addr > at ? m.addr : at, hi = m.addr + m.size < end ? m.addr + m.size : end;
        if (m.type == MemType_Unmapped) {
            if (hi - lo > *largest) *largest = hi - lo;
            if (hi - lo >= need) return lo;
        }
        at = m.addr + m.size;
    }
    return 0;
}

/* Pages of the heap something else still holds (finding 170). The homebrew loader hands every
   program it runs in this process the same heap, so a program that left part of it with a system
   service -- transfer memory a service never gave back, GPU memory, a mapping's locked source --
   leaves it inaccessible to the next one, which faults on its first write there. A block counts
   as held when it is not plain read-write heap with no attribute. heap_scan lists them (the first
   HELD_MAX) and returns how many there are, and the largest run of [lo, hi) clear of them. */
enum { HELD_MAX = 8 };
typedef struct { uintptr_t addr; uint64_t size; u32 type, perm, attr; } HeldBlock;

static int heap_scan(uintptr_t lo, uintptr_t hi, HeldBlock *held, uintptr_t *clear_lo, uintptr_t *clear_hi)
{
    int count = 0;
    uintptr_t run = lo, best_lo = lo, best_hi = lo;
    for (uintptr_t at = lo; at < hi;) {
        MemoryInfo m;
        u32 page;
        if (R_FAILED(svcQueryMemory(&m, &page, at)) || m.addr + m.size <= at) break;
        uintptr_t end = m.addr + m.size < hi ? m.addr + m.size : hi;
        int plain = (m.type & MemState_Type) == MemType_Heap && (m.perm & Perm_Rw) == Perm_Rw && !m.attr;
        if (!plain) {
            if (count < HELD_MAX)
                held[count] = (HeldBlock){at, end - at, m.type & MemState_Type, m.perm, m.attr};
            count++;
            if (at - run > best_hi - best_lo) best_lo = run, best_hi = at;
            run = end;
        }
        at = end;
    }
    if (hi - run > best_hi - best_lo) best_lo = run, best_hi = hi;
    if (clear_lo) *clear_lo = best_lo, *clear_hi = best_hi;
    return count;
}

static void held_log(const char *when, const HeldBlock *held, int count)
{
    fprintf(stderr, "host64 memory: %s: %d held heap block%s%s\n", when, count, count == 1 ? "" : "s",
            count ? " (type, permission, attribute)" : "");
    for (int i = 0; i < count && i < HELD_MAX; i++)
        fprintf(stderr, "  %p +%llu KiB: type 0x%X perm %u attr 0x%X\n", (void *)held[i].addr,
                (unsigned long long)(held[i].size >> 10), held[i].type, held[i].perm, held[i].attr);
}

static HeldBlock start_held[HELD_MAX];  /* what __libnx_initheap found, logged once the log is open */
static int start_held_count;
static uint64_t start_skipped;          /* bytes of heap left out to stay clear of them */
static uintptr_t heap_lo, heap_hi;      /* the heap as the loader gave it */

/* The exit's view (port/host/switch_main.c), after every teardown: what this program leaves held
   in the heap the next program gets. */
void host64_switch_heap_report(void)
{
    HeldBlock held[HELD_MAX];
    if (!heap_hi) return;
    held_log("at the exit", held, heap_scan(heap_lo, heap_hi, held, NULL, NULL));
}

/* libnx's own heap setup (weak there), with the guest's address space chosen first. */
extern char *fake_heap_start, *fake_heap_end;

void __libnx_initheap(void)
{
    void *addr;
    size_t size = 0;
    if (envHasHeapOverride()) {
        addr = envGetHeapOverrideAddr();
        size = envGetHeapOverrideSize();
    } else {
        uint64_t total = info(InfoType_TotalMemorySize), used = info(InfoType_UsedMemorySize);
        if (total > used + 0x200000) size = (total - used - 0x200000) & ~(uint64_t)0x1FFFFF;
        if (!size) size = 0x2000000 * 16;
        if (R_FAILED(svcSetHeapSize(&addr, size))) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
    }
    /* A heap that a program before this one left partly held: only its largest clear run is used,
       guest and malloc alike (the first is the bsd transfer memory socketInitialize zeroes, which
       is where a second start from the Homebrew Menu crashed). */
    heap_lo = (uintptr_t)addr, heap_hi = (uintptr_t)addr + size;
    uintptr_t clear_lo, clear_hi;
    start_held_count = heap_scan(heap_lo, heap_hi, start_held, &clear_lo, &clear_hi);
    if (start_held_count) {
        start_skipped = size - (clear_hi - clear_lo);
        addr = (void *)clear_lo;
        size = clear_hi - clear_lo;
    }
    uint64_t largest;
    if ((span_base = stack_region_run(SPAN, &largest))) {
        snprintf(span_why, sizeof span_why, "stack region at %p", (void *)span_base);
    } else if (size >= SPAN + HEAP_LEFT) {
        span_base = (uintptr_t)addr;
        in_heap = 1;
        addr = (char *)addr + SPAN;
        size -= SPAN;
        snprintf(span_why, sizeof span_why, "heap at %p (the stack region's largest free run: %llu MiB)",
                 (void *)span_base, (unsigned long long)(largest >> 20));
    } else {
        snprintf(span_why, sizeof span_why, "none: stack region's largest free run %llu MiB, heap %llu MiB",
                 (unsigned long long)(largest >> 20), (unsigned long long)(size >> 20));
    }
    fake_heap_start = (char *)addr;
    fake_heap_end = (char *)addr + size;
}

/* The stack region's mappings, for host64_switch_memory_release: each one locks its heap source
   until it is unmapped, and the heap outlives this program (the loader hands it to the next). */
typedef struct { uintptr_t at; void *source; size_t size; } Mapping;
static Mapping *mappings;         /* under commit_lock */
static int mapping_count, mapping_room;

/* [lo, hi), granule-aligned and none of it committed: in the heap, zeroed; in the stack region,
   one mapping of zeroed heap memory, which the process then never touches at its own address
   (svcMapMemory makes it inaccessible there). */
static int back_run(uint64_t lo, uint64_t hi)
{
    size_t size = (size_t)(hi - lo);
    if (in_heap) {
        memset((void *)(guest_base + lo), 0, size);
        return 1;
    }
    void *source = aligned_alloc(0x1000, size);
    if (!source) return 0;
    memset(source, 0, size);
    if (mapping_count == mapping_room) {   /* each one is kept: an untracked one could not be given back */
        int room = mapping_room ? mapping_room * 2 : 64;
        Mapping *grown = realloc(mappings, (size_t)room * sizeof *grown);
        if (!grown) {
            free(source);
            return 0;
        }
        mappings = grown;
        mapping_room = room;
    }
    if (R_FAILED(svcMapMemory((void *)(guest_base + lo), source, size))) {
        free(source);
        return 0;
    }
    mappings[mapping_count].at = guest_base + lo;
    mappings[mapping_count].source = source;
    mappings[mapping_count].size = size;
    mapping_count++;
    return 1;
}

/* The Switch's exit (port/host/switch_main.c), once no thread of ours is left to touch guest
   memory: the stack region's mappings undone. Without this, every heap page that backed the guest
   stays locked (no access) after this program has returned, and the next program the loader runs
   in this process -- the Homebrew Menu, or dhd.nro again -- faults on its first write there: a
   second start crashed inside nvInitialize's tmemCreate, zeroing a fresh transfer memory
   (finding 170). The heap's own guest span needs nothing: it is plain heap. */
void host64_switch_memory_release(void)
{
    int failed = 0;
    for (int i = mapping_count - 1; i >= 0; i--) {
        if (R_FAILED(svcUnmapMemory((void *)mappings[i].at, mappings[i].source, mappings[i].size))) {
            failed++;
            continue;
        }
        free(mappings[i].source);
    }
    if (mapping_count)
        fprintf(stderr, "host64 memory: %d stack region mapping%s given back%s\n", mapping_count - failed,
                mapping_count - failed == 1 ? "" : "s", failed ? " (some failed)" : "");
    mapping_count = 0;
    if (!in_heap && span_base) {
        virtmemLock();
        virtmemRemoveReservation(span_reservation);
        virtmemUnlock();
        span_reservation = NULL;
    }
    guest_base = 0;
    owner = NULL;
}

static int commit_range(uint64_t lo, uint64_t hi)
{
    lo &= ~(uint64_t)(GRANULE - 1);
    hi = (hi + GRANULE - 1) & ~(uint64_t)(GRANULE - 1);
    if (hi > SPAN) return 0;
    int ok = 1;
    mutexLock(&commit_lock);
    for (uint64_t at = lo; at < hi;) {
        if (committed[at / GRANULE]) {
            at += GRANULE;
            continue;
        }
        uint64_t end = at;
        while (end < hi && !committed[end / GRANULE]) end += GRANULE;
        if (!back_run(at, end)) {
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
    fprintf(stderr, "host64 memory: process %llu of %llu MiB used; guest address space: %s\n",
            (unsigned long long)(info(InfoType_UsedMemorySize) >> 20),
            (unsigned long long)(info(InfoType_TotalMemorySize) >> 20), span_why);
    if (start_held_count) {
        held_log("at the start (a program before this one left them)", start_held, start_held_count);
        fprintf(stderr, "host64 memory: %llu MiB of the heap left out to stay clear of them\n",
                (unsigned long long)(start_skipped >> 20));
    }
    if (!span_base) fail("no room for the guest's address space (start in title takeover: hold R on a game)");
    if (initial * 65536 > SPAN) fail("the module's memory outgrew SPAN (guest_memory_switch.c)");
    if (!in_heap) {  /* the stack region's run, kept from thread stacks from here on */
        virtmemLock();
        span_reservation = virtmemAddReservation((void *)span_base, SPAN);
        virtmemUnlock();
        if (!span_reservation) fail("stack region reservation");
    }
    guest_base = span_base;
    if (!(committed = calloc((size_t)(SPAN / GRANULE), 1))) fail("commit map");
    owner = m;  /* host64_memory_commit's bounds */
    m->size = initial * 65536;
    if (!commit_range(0x08000000, 0x0a000000) || !commit_range(HOST64_MODULE_BASE, initial * 65536))
        fail("RAM or module commit");
    m->data = (uint8_t *)guest_base;
    m->data_end = m->data + m->size;
    m->pages = initial;
    m->max_pages = max;
    m->is64 = false;
    m->page_size = page_size;
}

uint64_t wasm_rt_grow_memory(wasm_rt_memory_t *m, uint64_t pages)
{
    if (m != owner || pages > m->max_pages - m->pages) return UINT32_MAX;
    uint64_t old = m->pages, end = (old + pages) * 65536;
    if (end > SPAN || !commit_range(m->size, end)) return UINT32_MAX;
    m->pages += pages;
    m->size = end;
    m->data_end = m->data + end;
    return old;
}

/* At the program's end only: the address space and what backs it go with the process. */
void wasm_rt_free_memory(wasm_rt_memory_t *m)
{
    if (m != owner) fail("free of non-owner");
    guest_base = 0;
    owner = NULL;
    m->data = m->data_end = NULL;
    m->size = m->pages = 0;
}
