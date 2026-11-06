/* The Switch's stand-ins for the Rust crates other than the renderer (finding 167): there is no
   Rust std on Horizon. dhd_settings is reimplemented in C over the generated table
   (port/hle/settings_table.h: each dhd.ini key, its DHD_* variable and its built-in value);
   online play (dhd_net), its disc check (dhd_disc) and Discord (dhd_presence) are off, as on
   Android.

   dhd_settings_load differs from the crate in one way for now: with a dhd.ini, keys it does not
   name keep the game's built-in value rather than the crate's "player's default" (that column is
   in port/render/settings/src/table.rs, not in the generated C table yet). */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "dhd_settings.h"
#include "dhd_disc.h"
#include "dhd_presence.h"
#include "../net/ws.h"
#include "../hle/settings_table.h"

#define SETTINGS_COUNT ((int)(sizeof setting_table / sizeof setting_table[0]))

/* One line "key = value" of dhd.ini: key and value trimmed; 0 for blank lines, comments and
   sections. */
static int ini_line(char *line, char **key, char **value)
{
    char *eq, *end;
    while (isspace((unsigned char)*line)) line++;
    if (!*line || *line == '#' || *line == ';' || *line == '[') return 0;
    eq = strchr(line, '=');
    if (!eq) return 0;
    *eq = 0;
    for (end = eq; end > line && isspace((unsigned char)end[-1]); end--) end[-1] = 0;
    *key = line;
    for (line = eq + 1; isspace((unsigned char)*line); line++) {}
    for (end = line + strlen(line); end > line && isspace((unsigned char)end[-1]); end--) end[-1] = 0;
    if (*line == '"' && end > line + 1 && end[-1] == '"') { line++; end[-1] = 0; }
    *value = line;
    return **key != 0;
}

/* The table row of a dhd.ini key: its key or its DHD_* variable's name, without case. */
static const SettingInfo *setting_find(const char *key)
{
    for (int i = 0; i < SETTINGS_COUNT; i++)
        if (!strcasecmp(setting_table[i].key, key) || (*setting_table[i].env && !strcasecmp(setting_table[i].env, key)))
            return &setting_table[i];
    return NULL;
}

int dhd_settings_load(const char *path, DhdSettingsGet get, DhdSettingsSet set, DhdSettingsUnknown unknown)
{
    char overrides[2048] = ",", line[1024];
    for (int i = 0; i < SETTINGS_COUNT; i++) {
        const char *env = setting_table[i].env;
        if (*env && get(env) && strlen(overrides) + strlen(env) + 2 < sizeof overrides) {
            strcat(overrides, env);
            strcat(overrides, ",");
        }
    }
    set("DHD_ENV_OVERRIDES", overrides);
    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        char *key, *value;
        if (!ini_line(line, &key, &value) || !*value) continue;
        const SettingInfo *s = setting_find(key);
        const char *env = s ? s->env : (!strncasecmp(key, "DHD_", 4) ? key : NULL);
        if (env && *env) {
            if (!get(env)) set(env, value);
        } else if (!s && unknown) {
            unknown(key);
        }
    }
    fclose(f);
    return 1;
}

int dhd_settings_read(const char *path, const char *key, char *out, size_t size)
{
    char line[1024];
    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f) return 0;
    int found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        char *k, *v;
        if (ini_line(line, &k, &v) && !strcasecmp(k, key)) {
            snprintf(out, size, "%s", v);
            found = 1;
        }
    }
    fclose(f);
    return found;
}

/* Rewrites dhd.ini with key = value in place of its first line for key (its other lines dropped),
   every other line kept; appended when the file has none. */
int dhd_settings_save(const char *path, const char *key, const char *value)
{
    if (!path) return 0;
    const char *disabled = getenv("DHD_SETTINGS");
    if (disabled && !strcasecmp(disabled, "none")) return 0;
    char temp[1024], line[1024], copy[1024];
    snprintf(temp, sizeof temp, "%s.tmp", path);
    FILE *in = fopen(path, "r"), *out = fopen(temp, "w");
    if (!out) {
        if (in) fclose(in);
        return 0;
    }
    int written = 0;
    while (in && fgets(line, sizeof line, in)) {
        char *k, *v;
        snprintf(copy, sizeof copy, "%s", line);
        if (ini_line(copy, &k, &v) && !strcasecmp(k, key)) {
            if (!written) fprintf(out, "%s = %s\n", key, value);
            written = 1;
            continue;
        }
        fputs(line, out);
        if (!strchr(line, '\n')) fputc('\n', out);
    }
    if (!written) fprintf(out, "%s = %s\n", key, value);
    if (in) fclose(in);
    if (fclose(out)) {
        remove(temp);
        return 0;
    }
    remove(path);
    return rename(temp, path) == 0;
}

/* ---- online and Discord: off on the Switch ---- */
int dhd_disc_check(const char *iso, const char *cache, uint8_t digest[32], char *note, size_t size)
{
    (void)iso; (void)cache; (void)digest;
    if (size) snprintf(note, size, "no online check on the Switch");
    return -1;
}

int dhd_presence_start(const char *setting, DhdPresenceLog log) { (void)setting; (void)log; return 0; }
void dhd_presence_set(int kind, const char *track, int place, int riders, const char *room, int members, int max_members)
{ (void)kind; (void)track; (void)place; (void)riders; (void)room; (void)members; (void)max_members; }
void dhd_presence_stop(void) {}

WsConn *ws_start(const char *url, const WsHandler *h) { (void)url; (void)h; return NULL; }
void ws_send(WsConn *ws, int binary, const void *data, size_t n) { (void)ws; (void)binary; (void)data; (void)n; }
void ws_stop(WsConn *ws) { (void)ws; }
int http_get(const char *url, char *out, size_t cap) { (void)url; if (cap) out[0] = 0; return -1; }
