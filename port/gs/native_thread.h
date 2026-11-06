/* The platform under the native renderer's shared half (native_core.inc): its locks, condition
   variable, render thread, atomic stores, short waits, bit scan and process exit. native_core.inc
   calls only these, so it compiles wherever this header has an implementation.

     native         SDL3 (port/tools/sdl3.py, linked on every native platform): SDL_Mutex,
                    SDL_Condition, SDL_Thread, SDL_DelayNS (a high-resolution timer where the OS
                    has one).
     NATIVE_INLINE  one thread, nothing to lock (the browser, finding 085): the locks are no-ops,
                    there is no render thread (native_core.inc draws each field at its own vsync),
                    and a wait that would need another thread to end it stops the process instead
                    of hanging. The browser has no SDL.

   NativeWindow is present.c's window, opaque here (HWND on Windows, ANativeWindow on Android). */
#ifndef PORT_GS_NATIVE_THREAD_H
#define PORT_GS_NATIVE_THREAD_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef void *NativeWindow;
#define NATIVE_PATH_MAX 4096

/* A store the other threads see whole (native surface counts and extents only grow). */
static inline void native_atomic_store(volatile long *target, long value) { __atomic_store_n(target, value, __ATOMIC_SEQ_CST); }
/* The lowest set bit's index; bits is never 0. */
static inline unsigned native_bit_first(uint32_t bits) { return (unsigned)__builtin_ctz(bits); }
/* Leaves at once, without atexit handlers (the exit report would wait on the render thread). */
static inline void native_exit_now(int code) { _Exit(code); }

#if defined(NATIVE_INLINE)

#include <strings.h>

typedef int NativeMutex;
typedef int NativeCond;

static inline void native_mutex_init(NativeMutex *m) { (void)m; }
static inline void native_mutex_lock(NativeMutex *m) { (void)m; }
static inline void native_mutex_unlock(NativeMutex *m) { (void)m; }
static inline void native_cond_init(NativeCond *c) { (void)c; }
static inline void native_cond_broadcast(NativeCond *c) { (void)c; }
/* Only another thread could end this wait, and there is none. */
static inline void native_cond_wait(NativeCond *c, NativeMutex *m)
{
    (void)c; (void)m;
    fprintf(stderr, "gs native: a wait with no render thread (NATIVE_INLINE)\n");
    fflush(stderr);
    abort();
}
static inline void native_sleep_ns(uint64_t ns) { (void)ns; }
static inline void native_cpu_relax(void) {}
static inline int native_stricmp(const char *a, const char *b) { return strcasecmp(a, b); }

#else

#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>

typedef SDL_Mutex *NativeMutex;
typedef SDL_Condition *NativeCond;

static inline void native_mutex_init(NativeMutex *m) { *m = SDL_CreateMutex(); }
static inline void native_mutex_lock(NativeMutex *m) { SDL_LockMutex(*m); }
static inline void native_mutex_unlock(NativeMutex *m) { SDL_UnlockMutex(*m); }
static inline void native_cond_init(NativeCond *c) { *c = SDL_CreateCondition(); }
static inline void native_cond_broadcast(NativeCond *c) { SDL_BroadcastCondition(*c); }
static inline void native_cond_wait(NativeCond *c, NativeMutex *m) { SDL_WaitCondition(*c, *m); }

/* The render thread, detached (it runs until the process ends). It raises its own priority
   (native_thread) so a picture is not held up by the EE's threads. 1 when it started.

   On the Switch it is kept instead, and joined at the exit (native_thread_join): libnx has no
   pthread_detach, so a detached thread's kernel object and stack mapping outlive it, and the
   Homebrew Menu then runs in this same process with them still there (finding 146). */
#ifdef __SWITCH__
static SDL_Thread *native_thread_handle;
#endif
static inline int native_thread_start(SDL_ThreadFunction proc, void *argument)
{
    SDL_Thread *thread = SDL_CreateThread(proc, "native", argument);
    if (!thread) return 0;
#ifdef __SWITCH__
    native_thread_handle = thread;
#else
    SDL_DetachThread(thread);
#endif
    return 1;
}
#ifdef __SWITCH__
#include <SDL3/SDL_video.h>
/* The render thread's last act on the Switch: its OpenGL context made current nowhere. A context
   left current on a thread that has ended stays bound for good, and EGL keeps a bound context
   (and the GPU device under it) through eglTerminate: the exit's SDL_Quit could then not close
   nvdrv (finding 146). */
static inline void native_thread_release_gl(void)
{
    SDL_Window *window = SDL_GL_GetCurrentWindow();
    if (window || SDL_GL_GetCurrentContext()) SDL_GL_MakeCurrent(window, NULL);
}
/* The render thread's end, once it has returned: its handle closed. */
static inline void native_thread_join(void)
{
    if (native_thread_handle) SDL_WaitThread(native_thread_handle, NULL);
    native_thread_handle = NULL;
}
#endif

static inline void native_sleep_ns(uint64_t ns) { SDL_DelayNS(ns); }
static inline void native_cpu_relax(void) { SDL_CPUPauseInstruction(); }
static inline int native_stricmp(const char *a, const char *b) { return SDL_strcasecmp(a, b); }

#endif

#endif
