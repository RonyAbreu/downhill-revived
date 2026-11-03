/* The Nintendo Switch's start (finding 144), in place of the launcher's own process start:
   android_main.c's counterpart. The homebrew loader starts dhd.nro with argv[0] its path on the SD
   card; dhd.ini, dhd.log and the memory cards go in that folder (host_exe_path, switch_exe_path).
   The disc image is dhd.ini's `iso`, else game.iso beside the .nro. Then the shared start,
   dhd_launcher_main (port/host64/launcher.c, built with main renamed).

   Milestone NX1 has no renderer (port/switch/render_stubs.c): the game runs headless unless
   DHD_HEADLESS is set to 0 in dhd.ini. With nxlink (`nxlink -s dhd.nro`), stdout and stderr also
   go to the PC. */
#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "dhd_settings.h"

#define DEFAULT_DIR "sdmc:/switch/downhill-revived/"

int dhd_launcher_main(int argc, char **argv);

static char exe[1024], dir[1024];
static int nxlink_socket = -1;

const char *switch_exe_path(void) { return exe; }

void switch_shutdown(void)
{
    if (nxlink_socket >= 0) {
        close(nxlink_socket);
        socketExit();
        nxlink_socket = -1;
    }
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

    if (appletGetAppletType() != AppletType_Application && appletGetAppletType() != AppletType_SystemApplication)
        fprintf(stderr, "switch: started from the album (applet mode): too little memory; hold R while starting a game\n");

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
    if (!getenv("DHD_HEADLESS") && !(dhd_settings_read(ini, "DHD_HEADLESS", headless, sizeof headless) &&
                                     !strcmp(headless, "0")))
        setenv("DHD_HEADLESS", "1", 1);  /* NX1: no renderer yet */
    setenv("DHD_DEFER_READS", "0", 0);    /* no page protection (host_switch.c) */
    /* SETTINGS without the window rows: one screen, as on a phone */
    setenv("DHD_PC_SETTINGS_ROWS", "DHD_SCALE,DHD_ASPECT,DHD_PRESENT_FILTER,DHD_WIDESCREEN,DHD_SKIP_MOVIES,DHD_VIBRATION,"
           "DHD_STICK_DEAD_ZONE,DHD_PROMPTS,DHD_TEXTURES,DHD_FONT,DHD_FPS,DHD_VSYNC,DHD_PERF_COUNTER,DHD_UNLOCK_ALL", 0);
    fprintf(stderr, "switch: Downhill Revived in %s\n", dir);

    char *args[] = {exe, NULL};
    int code = dhd_launcher_main(1, args);
    fprintf(stderr, "switch: the game ended (%d)\n", code);
    switch_shutdown();
    return code;
}
