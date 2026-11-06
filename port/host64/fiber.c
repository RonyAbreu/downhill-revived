/* The EE kernel's threads (port/hle/kernel.c) run as fibers of one OS thread, the same way on
   every host. 64 slots, each with a fixed guest stack (256 KiB at 0x06000000 + (id - 1) * 256 KiB,
   its lowest guest page reserved so an overflow faults instead of running into the slot below)
   and a native stack of its own (8 MiB above one OS page of guard). The root fiber is the thread's
   own stack. A switch saves the module's __stack_pointer and wasm2c's call depth here; the native
   registers and the FP control register are the CPU's adapter (fiber_switch.h). No OS fiber API
   and no ucontext. Written for this task; no third-party code. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif !defined(__SWITCH__)
#define _GNU_SOURCE
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <stdlib.h>
#include <string.h>
#include "wasm-rt.h"
#include "core_host.h"
#include "fiber_switch.h"
#include "memory_layout.h"
#if !WASM_RT_STACK_DEPTH_COUNT
#error Build all wasm2c runtime units with WASM_RT_STACK_DEPTH_COUNT=1
#endif

enum { SLOTS=64, ROOT=65, STACK_BASE=0x06000000, STACK_BYTES=0x40000, GUARD=HOST64_PAGE };
enum { NATIVE_STACK=8*1024*1024 };
typedef struct {
    void *native;          /* the saved native stack pointer (the adapter's frame) */
    char *stack;           /* the native stack's mapping, guard page first; NULL for the root */
    void (*fn)(void *); void *arg;
    uint32_t sp, depth;    /* __stack_pointer and wasm2c's call depth while switched out */
    int active;
} Fiber;
static Fiber fibers[ROOT+1];
static uint32_t *module_sp;
static int current;
static size_t page;        /* the OS page: the native stacks' guard */
static _Thread_local int owner;
static void check_thread(void) { if (!owner) abort(); }

/* A native stack: read/write with its lowest OS page inaccessible. */
#ifdef _WIN32
static size_t os_page(void) { SYSTEM_INFO s; GetSystemInfo(&s); return s.dwPageSize; }
static char *stack_map(size_t size) {
    char *p=VirtualAlloc(NULL,size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE); DWORD old;
    if (p && !VirtualProtect(p,page,PAGE_NOACCESS,&old)) { VirtualFree(p,0,MEM_RELEASE); p=NULL; }
    return p;
}
static void stack_unmap(char *p,size_t size) { (void)size; VirtualFree(p,0,MEM_RELEASE); }
#elif defined(__SWITCH__)
/* Horizon reprotects only heap memory and a homebrew cannot place mappings: a plain allocation,
   its guard page left accessible (the guard only ever reported an overflow; finding 167). */
static size_t os_page(void) { return HOST64_PAGE; }
static char *stack_map(size_t size) { return aligned_alloc(HOST64_PAGE,size); }
static void stack_unmap(char *p,size_t size) { (void)size; free(p); }
#else
static size_t os_page(void) { return (size_t)sysconf(_SC_PAGESIZE); }
static char *stack_map(size_t size) {
    char *p=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (p==MAP_FAILED) return NULL;
    if (mprotect(p,page,PROT_NONE)<0) { munmap(p,size); p=NULL; }
    return p;
}
static void stack_unmap(char *p,size_t size) { munmap(p,size); }
#endif

void host64_fiber_bind(uint32_t *sp) {
    if (current || !sp) abort();
    module_sp=sp; owner=1; page=os_page();
}
HostFiber host_fiber_main(void) {
    check_thread(); if (!module_sp) abort(); if (current) return current;
    fibers[ROOT]=(Fiber){.active=1};
    current=ROOT; return ROOT;
}
void fiber_entry(void *p) {
    Fiber *f=p; f->fn(f->arg);
    /* EE wrappers normally never return. Returning here resumes the root. */
    host_fiber_switch(ROOT); abort();
}
HostFiber host_fiber_create(void (*fn)(void *),void *arg) {
    check_thread(); if (!current || !fn) return 0;
    for (int i=1;i<=SLOTS;i++) if (!fibers[i].active) {
        uint32_t low=STACK_BASE+(i-1)*STACK_BYTES;
        if (!host64_memory_commit(low+GUARD,STACK_BYTES-GUARD)) return 0;
        memset((void *)(guest_base+low+GUARD),0,STACK_BYTES-GUARD);
        char *stack=stack_map(page+NATIVE_STACK);
        if (!stack) return 0;
        Fiber *f=&fibers[i];
        *f=(Fiber){.stack=stack,.fn=fn,.arg=arg,.sp=low+STACK_BYTES,.active=1};
        f->native=fiber_frame((uintptr_t)stack+page,(uintptr_t)stack+page+NATIVE_STACK,f);
        return i;
    }
    return 0;
}
void host_fiber_switch(HostFiber to) {
    check_thread(); if (!current || to<1 || to>ROOT || !fibers[to].active) abort();
    if (current==to) return;
    Fiber *from=&fibers[current];
    from->sp=*module_sp; from->depth=wasm_rt_call_stack_depth;
    *module_sp=fibers[to].sp;
    wasm_rt_call_stack_depth=fibers[to].depth;
    current=to; fiber_switch_asm(&from->native,fibers[to].native);
}
void host_fiber_delete(HostFiber id) {
    check_thread(); if (id<1 || id>SLOTS || id==current) abort();
    if (fibers[id].active) stack_unmap(fibers[id].stack,page+NATIVE_STACK);
    memset(&fibers[id],0,sizeof fibers[id]);
}
HostFiber host_fiber_current(void) { check_thread(); return current; }
int host64_fiber_id(void) { return current; }  /* diagnostics: any thread */
uint32_t host64_fiber_stack_top(HostFiber id) {
    return id>=1 && id<=SLOTS ? STACK_BASE+id*STACK_BYTES : 0;
}
int host_stack_ranges(uintptr_t (**ranges)[2]) {
    static uintptr_t result[SLOTS][2]; int n=0; check_thread();
    for (int i=1;i<=SLOTS;i++) if (fibers[i].active) {
        result[n][0]=STACK_BASE+(i-1)*STACK_BYTES;
        result[n++][1]=STACK_BASE+i*STACK_BYTES;
    }
    *ranges=result; return n;
}
void host64_fiber_shutdown(void) {
    check_thread(); if (current!=ROOT) abort();
    for (int i=1;i<=SLOTS;i++) host_fiber_delete(i);
    memset(fibers,0,sizeof fibers); current=0; module_sp=NULL; owner=0;
}
