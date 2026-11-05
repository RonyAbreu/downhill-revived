/* The Switch's CPU, GPU and memory clocks (finding 144, NX3).

   Horizon starts a homebrew at the handheld defaults: CPU 1020 MHz, GPU 307.2 MHz, EMC
   1331.2 MHz. The first measurement of the native renderer on hardware (DHD_GL_TIMES) put the
   whole field in the draws and tracked the pixel count almost exactly -- about 150 Mpixel/s of
   effective fill -- which is what a Tegra X1 gives at 307 MHz with a shader this long. The rates
   below are the ones Horizon itself uses for its own boost modes, not an overclock past stock.

   dhd.ini (`DHD_` keys reach the environment whatever the settings table holds, rust_shims.c):

       DHD_SWITCH_CLOCKS = on     CPU 1785, GPU 768, EMC 1600 MHz (the default)
       DHD_SWITCH_CLOCKS = max    CPU 1785, GPU 921.6, EMC 1600 MHz
       DHD_SWITCH_CLOCKS = off    leave every clock alone

   Raised clocks mean more heat and a shorter battery; `off` is also what to use when sys-clk is
   managing this title, so the two do not write over each other.

   The rates are set again every two seconds from a thread of its own, because docking, the HOME
   menu and sleep all put them back, and the original rates are restored at exit. */
#include <switch.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "clocks_switch.h"

typedef struct Clock {
    PcvModuleId module;        /* clkrst, 8.0.0 and later */
    PcvModule legacy;          /* pcv, before 8.0.0 */
    const char *name;
    uint32_t want, was;
    int open;
    ClkrstSession session;
} Clock;

static Clock g_clocks[] = {
    { PcvModuleId_CpuBus, PcvModule_CpuBus, "CPU", 0, 0, 0, {0} },
    { PcvModuleId_GPU,    PcvModule_GPU,    "GPU", 0, 0, 0, {0} },
    { PcvModuleId_EMC,    PcvModule_EMC,    "EMC", 0, 0, 0, {0} },
};
enum { CLOCK_COUNT = (int)(sizeof g_clocks / sizeof g_clocks[0]) };

static int g_legacy;           /* before 8.0.0: pcv rather than clkrst */
static int g_started;
static SDL_AtomicInt g_stop;
static SDL_Thread *g_keeper;

static int clock_get(Clock *c, uint32_t *hz)
{
    if (g_legacy) return R_SUCCEEDED(pcvGetClockRate(c->legacy, hz));
    return c->open && R_SUCCEEDED(clkrstGetClockRate(&c->session, hz));
}

static int clock_set(Clock *c, uint32_t hz)
{
    if (g_legacy) return R_SUCCEEDED(pcvSetClockRate(c->legacy, hz));
    return c->open && R_SUCCEEDED(clkrstSetClockRate(&c->session, hz));
}

/* Docking, the HOME menu and sleep all put the clocks back, and there is no one event for the
   three, so they are simply written again every two seconds. */
static int keep_clocks(void *unused)
{
    (void)unused;
    while (!SDL_GetAtomicInt(&g_stop)) {
        for (int i = 0; i < 20 && !SDL_GetAtomicInt(&g_stop); i++) SDL_Delay(100);
        if (SDL_GetAtomicInt(&g_stop)) break;
        for (int i = 0; i < CLOCK_COUNT; i++) {
            uint32_t now = 0;
            if (g_clocks[i].want && clock_get(&g_clocks[i], &now) && now != g_clocks[i].want)
                clock_set(&g_clocks[i], g_clocks[i].want);
        }
    }
    return 0;
}

void switch_clocks_start(const char *mode)
{
    if (g_started) return;
    if (!mode || !*mode) mode = "on";
    if (!strcasecmp(mode, "off") || !strcmp(mode, "0")) {
        fprintf(stderr, "switch: clocks left as Horizon set them (DHD_SWITCH_CLOCKS = %s)\n", mode);
        return;
    }
    int most = !strcasecmp(mode, "max");
    g_clocks[0].want = 1785000000u;                        /* CPU: Horizon's own boost rate */
    g_clocks[1].want = most ? 921600000u : 768000000u;     /* GPU: 921.6 is the boost one, 768 docked */
    g_clocks[2].want = 1600000000u;                        /* EMC: Horizon's own boost rate */

    g_legacy = hosversionBefore(8, 0, 0);
    if (g_legacy) {
        if (R_FAILED(pcvInitialize())) {
            fprintf(stderr, "switch: no pcv: the clocks stay as they are\n");
            return;
        }
    } else {
        if (R_FAILED(clkrstInitialize())) {
            fprintf(stderr, "switch: no clkrst: the clocks stay as they are\n");
            return;
        }
        for (int i = 0; i < CLOCK_COUNT; i++)
            g_clocks[i].open = R_SUCCEEDED(clkrstOpenSession(&g_clocks[i].session, g_clocks[i].module, 3));
    }
    g_started = 1;
    for (int i = 0; i < CLOCK_COUNT; i++) {
        Clock *c = &g_clocks[i];
        if (!clock_get(c, &c->was)) c->was = 0;
        if (!clock_set(c, c->want)) {
            fprintf(stderr, "switch: the %s clock would not take %u MHz (it stays at %u MHz)\n",
                    c->name, c->want / 1000000u, c->was / 1000000u);
            c->want = 0;   /* nothing to keep, and nothing to restore */
            continue;
        }
        uint32_t now = 0;
        clock_get(c, &now);
        fprintf(stderr, "switch: %s clock %u -> %u MHz\n", c->name, c->was / 1000000u,
                (now ? now : c->want) / 1000000u);
    }
    SDL_SetAtomicInt(&g_stop, 0);
    g_keeper = SDL_CreateThread(keep_clocks, "dhd-clocks", NULL);
}

void switch_clocks_stop(void)
{
    if (!g_started) return;
    g_started = 0;
    SDL_SetAtomicInt(&g_stop, 1);
    if (g_keeper) { SDL_WaitThread(g_keeper, NULL); g_keeper = NULL; }
    for (int i = 0; i < CLOCK_COUNT; i++) {
        Clock *c = &g_clocks[i];
        if (c->want && c->was) clock_set(c, c->was);
        if (c->open) { clkrstCloseSession(&c->session); c->open = 0; }
    }
    if (g_legacy) pcvExit(); else clkrstExit();
    fprintf(stderr, "switch: the clocks are back as they were\n");
}
