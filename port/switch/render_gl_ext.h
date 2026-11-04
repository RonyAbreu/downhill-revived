/* Shared between render_gl.c (the device, surfaces, presentation) and render_native.c (NX2 step 1:
   the native renderer's GL half): render_gl.c's GL context lives on a DhdrSurface, made current
   lazily. The native renderer draws and builds surfaces before any dhdr_native_present call (the
   same field's native_render then native_present, gpu_rust_shared.inc), so it needs that context
   current on its own thread too, before its own first GL call: it asks for the one surface this
   single-window game creates (dhdr_gl_active_surface) and makes it current itself
   (dhdr_gl_make_current), exactly as render_gl.c's own present calls do. */
#ifndef RENDER_GL_EXT_H
#define RENDER_GL_EXT_H
#include "dhd_render.h"

/* Makes s's GL context current on this thread (creating it on the first call), loading glad and
   compiling render_gl.c's quad program. 0: no OpenGL (logged once by render_gl.c). */
int dhdr_gl_make_current(DhdrSurface *s);

/* The last surface dhdr_surface_create made, or NULL (none yet, or destroyed since). The game
   makes one surface on one window, so this is the native renderer's too. */
DhdrSurface *dhdr_gl_active_surface(void);

#endif
