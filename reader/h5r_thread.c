/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Home5 background shortcuts: the thread injected into SceShellUI that reads the DualSense while a
 * game is in front and recognises the player's combos and sequences.
 *
 * Based on EnriqueWood's proof of concept for Home5 (github.com/bornaradusin/Home5, issue "Background
 * shortcuts"), Copyright (C) 2026 Enrique Wood, GPL-3.0-or-later. The pad reading follows
 * ps5-dualsense-overlay (erickdavestech), commonfps_shellui.cpp pad_read(). Generalised here to read the
 * shortcuts Home5 writes and to fire any of them, with a light-bar flash as feedback.
 *
 * It draws nothing and hooks nothing: one thread, a pad handle, and a log in /system_tmp. The thread must
 * never return (an injected ShellUI thread that returns can destabilise the console), so the stop file
 * parks it in a sleep loop until the next reboot.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int scePadOpen(int user_id, int type, int index, const void *param);
int scePadClose(int handle);
int scePadGetHandle(int user_id, int type, int index);
int scePadReadState(int handle, void *data);
int sceUserServiceGetForegroundUser(int *user_id);
typedef struct { uint8_t r, g, b, reserved; } PadColor;
int scePadSetLightBar(int handle, const PadColor *color);
int scePadResetLightBar(int handle);

/* All in /system_tmp: SceShellUI cannot read /data, so the loader (a normal payload, which can) copies
 * the shortcuts here for the thread. Own names (home5_reader.*), so an older reader's thread can't be
 * mistaken for this one. */
#define CFG_PATH    "/system_tmp/home5_reader.cfg"
#define LOG_PATH    "/system_tmp/home5_reader.log"
#define MARKER_PATH "/system_tmp/home5_reader.pid"
#define STOP_PATH   "/system_tmp/home5_reader.stop"

#define BTN_INTERCEPTED 0x80000000u
#define BTN_MASK        0x0010ffffu          /* the buttons this reads; the PS button (0x10000) is left out */
#define POLL_US         20000                /* ~50 Hz, as the overlay reads */
#define SEQ_TIMEOUT_MS  1500
#define FLASH_MS        350
#define MAX_TRIG        32
#define MAX_LOG_LINES   20000

enum { TRIG_COMBO = 0, TRIG_SEQ = 1 };
enum { FB_BAR = 0, FB_NOTIFY = 1, FB_BOTH = 2 };

typedef struct {
    int mode, confirm, len;
    uint32_t combo;
    uint32_t seq[6];
} Trig;

static Trig g_trig[MAX_TRIG];
static int g_ntrig;
static int g_hold_ms = 150;
static int g_lines;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void log_line(const char *text)
{
    FILE *fp = fopen(LOG_PATH, g_lines >= MAX_LOG_LINES ? "w" : "a");
    if (!fp) return;
    if (g_lines >= MAX_LOG_LINES) g_lines = 0;
    fprintf(fp, "%llu %s\n", (unsigned long long)now_ms(), text);
    fclose(fp);
    g_lines++;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) *--end = 0;
    return s;
}

/* Reads the shortcuts Home5 wrote. Returns 1 when the file was read. Lines:
 *   hold_ms = 150
 *   sc = mode | confirm | domain | service | entity | data | keys | name   (keys: combo 0xMASK, seq 0xA,0xB,…) */
static int load_config(void)
{
    FILE *fp = fopen(CFG_PATH, "r");
    char line[512];
    if (!fp) return 0;
    g_ntrig = 0;
    while (fgets(line, sizeof line, fp)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(line), *val = eq + 1;
        if (!strcmp(key, "hold_ms")) {
            int h = atoi(trim(val));
            if (h >= 50 && h <= 2000) g_hold_ms = h;
            continue;
        }
        if (strcmp(key, "sc") || g_ntrig >= MAX_TRIG) continue;
        char *f[8];
        int nf = 0;
        f[nf++] = val;
        for (char *p = val; *p && nf < 8; p++)
            if (*p == '|') { *p = 0; f[nf++] = p + 1; }
        if (nf < 7) continue;
        Trig t;
        memset(&t, 0, sizeof t);
        t.mode = !strcmp(trim(f[0]), "seq") ? TRIG_SEQ : TRIG_COMBO;
        const char *cf = trim(f[1]);
        t.confirm = !strcmp(cf, "notify") ? FB_NOTIFY : !strcmp(cf, "both") ? FB_BOTH : FB_BAR;
        char *keys = trim(f[6]);
        if (t.mode == TRIG_SEQ) {
            for (char *tok = strtok(keys, ","); tok && t.len < 6; tok = strtok(NULL, ","))
                t.seq[t.len++] = (uint32_t)strtoul(tok, NULL, 16);
        } else {
            t.combo = (uint32_t)strtoul(keys, NULL, 16) & BTN_MASK;
            for (uint32_t m = t.combo; m; m &= m - 1) t.len++;
        }
        if (t.len > 0) g_trig[g_ntrig++] = t;
    }
    fclose(fp);
    return 1;
}

static void write_marker(void)
{
    FILE *fp = fopen(MARKER_PATH, "w");
    if (!fp) return;
    fprintf(fp, "%d\n", getpid());
    fclose(fp);
}

static __attribute__((noreturn)) void park(const char *why)
{
    char line[128];
    snprintf(line, sizeof line, "parked: %s", why);
    log_line(line);
    for (;;) usleep(1000000);
}

__attribute__((noreturn, visibility("default"))) void *elf_main(void *payload_args)
{
    (void)payload_args;
    g_lines = 0;
    {
        FILE *fp = fopen(LOG_PATH, "w");
        if (fp) {
            fprintf(fp, "%llu home5 shortcuts reader thread up pid=%d\n", (unsigned long long)now_ms(), getpid());
            fclose(fp);
        }
    }
    write_marker();
    {
        char line[64];
        int got = load_config();
        snprintf(line, sizeof line, "config: %s, %d shortcuts, hold=%dms", got ? "read" : "missing", g_ntrig, g_hold_ms);
        log_line(line);
    }

    int handle = -1, pad_user = -1, read_failures = 0, user_check = 0, init_wait = 0, init_logs = 0;
    uint32_t last_buttons = 0xffffffffu, prev_real = 0;
    int last_connected = -1;
    uint64_t combo_since[MAX_TRIG] = { 0 }, seq_last[MAX_TRIG] = { 0 }, cfg_check = 0, flash_until = 0;
    int combo_fired[MAX_TRIG] = { 0 }, seq_prog[MAX_TRIG] = { 0 };
    time_t cfg_mtime = 0;

    for (;;) {
        if (access(STOP_PATH, F_OK) == 0) {
            if (handle >= 0) scePadClose(handle);
            park("stop file");
        }

        uint64_t t = now_ms();
        /* Pick up a shortcut the player changed in the app (every ~2 s, on the file's time). */
        if (t - cfg_check > 2000) {
            struct stat st;
            cfg_check = t;
            if (stat(CFG_PATH, &st) == 0 && st.st_mtime != cfg_mtime) {
                cfg_mtime = st.st_mtime;
                load_config();
                memset(combo_since, 0, sizeof combo_since);
                memset(combo_fired, 0, sizeof combo_fired);
                memset(seq_prog, 0, sizeof seq_prog);
            }
        }
        if (flash_until && t >= flash_until && handle >= 0) {
            scePadResetLightBar(handle);
            flash_until = 0;
        }

        if (handle < 0) {
            if (++init_wait < 25) { usleep(POLL_US); continue; }
            init_wait = 0;
            int user = -1;
            if (sceUserServiceGetForegroundUser(&user) < 0 || user == -1) { usleep(POLL_US); continue; }
            int h = scePadOpen(user, 0, 0, NULL), opened = h;
            if (h < 0) h = scePadGetHandle(user, 0, 0);
            char line[128];
            if (h >= 0) {
                handle = h;
                pad_user = user;
                read_failures = 0;
                snprintf(line, sizeof line, "pad open: user=0x%x scePadOpen=0x%x handle=0x%x", user, opened, h);
                log_line(line);
            } else if (++init_logs <= 5) {
                snprintf(line, sizeof line, "pad open failed: user=0x%x scePadOpen=0x%x GetHandle=0x%x", user, opened, h);
                log_line(line);
            }
            usleep(POLL_US);
            continue;
        }

        if (++user_check > 150) {
            user_check = 0;
            int fg = -1;
            if (sceUserServiceGetForegroundUser(&fg) == 0 && fg != -1 && fg != pad_user) {
                scePadClose(handle);
                handle = -1;
                continue;
            }
        }

        unsigned char raw[256];
        memset(raw, 0, sizeof raw);
        int rc = scePadReadState(handle, raw);
        if (rc < 0) {
            if (++read_failures > 15) {
                scePadClose(handle);
                handle = -1;
                read_failures = 0;
            }
            usleep(POLL_US);
            continue;
        }
        read_failures = 0;

        uint32_t buttons;
        int32_t connected;
        memcpy(&buttons, raw + 0x00, 4);
        memcpy(&connected, raw + 0x4c, 4);
        if (buttons != last_buttons || connected != last_connected) {
            char line[256];
            snprintf(line, sizeof line, "btn=0x%08x conn=%d", buttons, connected);
            log_line(line);
            last_buttons = buttons;
            last_connected = connected;
        }

        int live = connected && !(buttons & BTN_INTERCEPTED);
        uint32_t real = live ? (buttons & BTN_MASK) : 0;
        uint32_t fresh = real & ~prev_real;

        for (int i = 0; i < g_ntrig; i++) {
            Trig *g = &g_trig[i];
            int fire = 0;
            if (g->mode == TRIG_COMBO) {
                int held = live && (real & g->combo) == g->combo;
                if (!held) { combo_since[i] = 0; combo_fired[i] = 0; }
                else if (!combo_since[i]) combo_since[i] = t;
                else if (!combo_fired[i] && t - combo_since[i] >= (uint64_t)g_hold_ms) { combo_fired[i] = 1; fire = 1; }
            } else {
                if (seq_prog[i] > 0 && t - seq_last[i] > SEQ_TIMEOUT_MS) seq_prog[i] = 0;
                if (fresh & g->seq[seq_prog[i]]) {
                    seq_prog[i]++;
                    seq_last[i] = t;
                    if (seq_prog[i] >= g->len) { seq_prog[i] = 0; fire = 1; }
                } else if (fresh && seq_prog[i] > 0) {
                    seq_prog[i] = (fresh & g->seq[0]) ? 1 : 0;
                    seq_last[i] = t;
                }
            }
            if (fire) {
                char line[32];
                snprintf(line, sizeof line, "FIRE %d", i);
                log_line(line);
                if ((g->confirm == FB_BAR || g->confirm == FB_BOTH) && handle >= 0) {
                    PadColor amber = { 0xff, 0xc3, 0x5a, 0 };
                    scePadSetLightBar(handle, &amber);
                    flash_until = t + FLASH_MS;
                }
            }
        }
        prev_real = real;
        usleep(POLL_US);
    }
}
