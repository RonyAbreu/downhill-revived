/* The Nintendo Switch's start (finding 144), in place of the launcher's own process start:
   android_main.c's counterpart. The homebrew loader starts dhd.nro with argv[0] its path on the SD
   card; dhd.ini, dhd.log and the memory cards go in that folder (host_exe_path, switch_exe_path).
   The disc image is dhd.ini's `iso`, else game.iso beside the .nro. Then the shared start,
   dhd_launcher_main (port/host64/launcher.c, built with main renamed).

   The renderer is OpenGL (port/switch/render_gl.c, render_native.c; NX2 phase 2 step 1: flat
   untextured GS geometry with DHD_RENDER=native, else the movies only); dhd.ini's
   DHD_HEADLESS = 1 runs without a window, as the tools do. With nxlink (`nxlink -s dhd.nro`),
   stdout and stderr also go to the PC. */
#include <switch.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "dhd_settings.h"
#include "../switch/clocks_switch.h"

#define DEFAULT_DIR "sdmc:/switch/downhill-revived/"

int dhd_launcher_main(int argc, char **argv);

static char exe[1024], dir[1024];
static int nxlink_socket = -1;

const char *switch_exe_path(void) { return exe; }

void switch_shutdown(void)
{
    static int done;
    if (done) return;
    done = 1;
    switch_clocks_stop();
    if (nxlink_socket >= 0) {
        close(nxlink_socket);
        socketExit();
        nxlink_socket = -1;
    }
}

/* Started from the album (applet mode) the program gets a few hundred MiB, and guest memory alone
   is 2 GiB (guest_memory_switch.c): say so on the screen and end, instead of failing later. */
static int applet_mode_refused(void)
{
    AppletType type = appletGetAppletType();
    if (type == AppletType_Application || type == AppletType_SystemApplication) return 0;
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    printf("\n  Downhill Revived needs the memory of a game.\n\n"
           "  Start it from a game instead of the album: hold R while\n"
           "  starting any game, then pick dhd.nro in the Homebrew Menu.\n\n"
           "  Press + to exit.\n");
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
    return 1;
}

/* The exit, EXIT GAME's (finding 144).

   EXIT GAME used to end the process with exit() from the game thread (gs_pump_messages) while
   the other threads still ran, and libnx's teardown after the exit handlers closed the services
   under them: the crash report of the first EXIT GAME on hardware is SDL's main thread inside
   SWITCH_PollTouch -> hidGetTouchScreenStates -> diagAbortWithResult, HID already closed, and
   the clocks were never put back. Ending with svcExitProcess instead got as far as "the process
   ends" and still showed Horizon's error screen: the loader expects its program back, not a
   process gone from under it.

   So the exit is an ordinary one, made safe for libnx's teardown and for what comes after it:
   the Homebrew Menu, loaded into this same process. Every thread of ours has to be gone by
   then -- the crash reports of the next try were sphaira's (the menu), one of them a thread with
   our worker's stack size running code that was no longer mapped. The game thread ends itself
   (switch_quit); the main thread, which pumps SDL's events, exits (so nothing reads HID
   afterwards); the exit handlers join the VU and GS workers (graphics_shutdown) and the clock
   keeper; the last one here ends the render thread (gs_native_halt) and SDL's audio thread.
   DHD_SWITCH_EXIT = kernel keeps the svcExitProcess ending, for comparing. */
static volatile int quit_asked;

/* sdl_host_main's loop (the main thread) asks this every pass, and exits when it says so. */
int switch_quit_asked(void) { return __atomic_load_n(&quit_asked, __ATOMIC_ACQUIRE); }

/* The game thread's EXIT GAME: the main thread is told to exit, and this thread ends itself
   rather than waiting -- every thread of ours must be gone before libnx returns to the loader,
   because the Homebrew Menu then runs in this same process and a thread left behind wakes into
   code that is no longer there (finding 144). */
void switch_quit(void)
{
    __atomic_store_n(&quit_asked, 1, __ATOMIC_RELEASE);
    threadExit();
}

/* The last exit handler to run (registered first): after the reports and dhd.log's last lines,
   before libnx's own teardown. */
static void switch_exit_now(void)
{
    int gs_native_halt(unsigned ms);   /* port/gs/native_core.inc */
    void host_freeze_watch_stop(unsigned ms);   /* port/hle/host.c: a thread waking every second */
    int halted = gs_native_halt(1000);
    host_freeze_watch_stop(1500);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    switch_shutdown();
    const char *how = getenv("DHD_SWITCH_EXIT");
    int kernel = how && !strcmp(how, "kernel");
    fprintf(stderr, "switch: the process ends (%s%s)\n", kernel ? "svcExitProcess" : "back to the loader",
            halted ? "" : "; the render thread did not end");
    fflush(NULL);
    if (kernel) svcExitProcess();
}

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int main(int argc, char **argv)
{
    /* nxlink's host is the PC that sent the .nro; without it this fails quietly */
    if (R_SUCCEEDED(socketInitializeDefault())) {
        nxlink_socket = nxlinkStdio();
        if (nxlink_socket < 0) socketExit();
    }
    const char *self = argc > 0 && argv[0] && !strncmp(argv[0], "sdmc:/", 6) ? argv[0] : DEFAULT_DIR "dhd.nro";
    snprintf(exe, sizeof exe, "%s", self);
    snprintf(dir, sizeof dir, "%s", exe);
    char *slash = strrchr(dir, '/');
    if (slash) slash[1] = 0;
    mkdir(dir, 0777);
    atexit(switch_exit_now);   /* first, so it runs last */
    int refused = applet_mode_refused();  /* on the screen: before stdout goes to dhd.log */
    /* dhd.log before anything else prints, as android_main.c does: a failure this early is logged too */
    void host_console_init(void);
    host_console_init();
    if (refused) {
        fprintf(stderr, "switch: started from the album (applet mode): too little memory\n");
        switch_shutdown();
        return 1;
    }

    /* The disc: dhd.ini's iso, else game.iso beside the .nro (DHD_ISO, as on the other targets). */
    char ini[1100], iso[1100];
    snprintf(ini, sizeof ini, "%sdhd.ini", dir);
    if (!getenv("DHD_ISO")) {
        if (!dhd_settings_read(ini, "iso", iso, sizeof iso)) snprintf(iso, sizeof iso, "%sgame.iso", dir);
        if (file_exists(iso)) setenv("DHD_ISO", iso, 1);
        else fprintf(stderr, "switch: no disc image at %s (put your Downhill Domination (USA) .iso there "
                             "as game.iso, or name it in dhd.ini: iso = ...)\n", iso);
    }
    char headless[8];
    if (!getenv("DHD_HEADLESS") && dhd_settings_read(ini, "DHD_HEADLESS", headless, sizeof headless) &&
        strcmp(headless, "0"))
        setenv("DHD_HEADLESS", "1", 1);
    setenv("DHD_DEFER_READS", "0", 0);    /* no page protection (host_switch.c) */
    /* SETTINGS without the window rows: one screen, as on a phone */
    setenv("DHD_PC_SETTINGS_ROWS", "DHD_SCALE,DHD_ASPECT,DHD_PRESENT_FILTER,DHD_WIDESCREEN,DHD_SKIP_MOVIES,DHD_VIBRATION,"
           "DHD_STICK_DEAD_ZONE,DHD_PROMPTS,DHD_TEXTURES,DHD_FONT,DHD_FPS,DHD_VSYNC,DHD_PERF_COUNTER,DHD_UNLOCK_ALL", 0);
    fprintf(stderr, "switch: Downhill Revived in %s\n", dir);
    /* The clocks before the game starts: Horizon hands a homebrew the handheld defaults, and at
       GPU 307.2 MHz the native renderer is fill-rate bound (finding 144, NX3). */
    char clocks[16];
    if (!dhd_settings_read(ini, "DHD_SWITCH_CLOCKS", clocks, sizeof clocks)) clocks[0] = 0;
    switch_clocks_start(getenv("DHD_SWITCH_CLOCKS") ? getenv("DHD_SWITCH_CLOCKS") : clocks);
    /* DHD_SWITCH_CORES the same way, and for a reason of its own: switch_thread_core decides on
       the first thread it is told about, which is named before the launcher has put dhd.ini's
       settings into the environment, so `DHD_SWITCH_CORES = on` in dhd.ini alone came too late
       and nothing was ever pinned. */
    char cores[8];
    if (!getenv("DHD_SWITCH_CORES") && dhd_settings_read(ini, "DHD_SWITCH_CORES", cores, sizeof cores))
        setenv("DHD_SWITCH_CORES", cores, 1);

    char *args[] = {exe, NULL};
    int code = dhd_launcher_main(1, args);
    fprintf(stderr, "switch: the game ended (%d)\n", code);
    switch_shutdown();
    return code;
}
