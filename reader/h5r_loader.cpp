/*
 * Home5 background shortcuts reader: the payload you load. It injects the SceShellUI thread
 * (h5r_thread.c, embedded here), then follows the thread's log and, when a shortcut fires, calls Home
 * Assistant over its REST API and shows a console notification.
 *
 * Based on EnriqueWood's proof of concept for Home5 (the controller payload), Copyright (C) 2026 Enrique
 * Wood, GPL-3.0-or-later. The injector is from ps5-dualsense-overlay (erickdavestech, derived from Common
 * FPS for PS5 by porhe911), with ptrace/ELF-loader helpers from shsrv (John Törnblom). The Home Assistant
 * call reuses Home5's own client (h5_net.c).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <unistd.h>

#include "stable_shellui_injector.hpp"

extern "C" {
#include <ps5/klog.h>
int sceNotificationSend(int user_id, bool is_logged, const char *payload);
#include "h5.h"
#include "h5_net.h"
int h5r_net_start(void);
/* the SceShellUI thread ELF, embedded by the build (reader/build.sh) */
extern const unsigned char h5r_thread_elf[];
extern const unsigned int h5r_thread_elf_len;
}

namespace {

constexpr const char *kDir = "/data/homebrew/PPSA11019";
constexpr const char *kCfg = "/data/homebrew/PPSA11019/shortcuts.cfg";
constexpr const char *kLogPath = "/data/homebrew/PPSA11019/reader.log";
constexpr const char *kStopPath = "/data/homebrew/PPSA11019/stop";
constexpr const char *kParkPath = "/data/homebrew/PPSA11019/park";
constexpr const char *kThreadLog = "/system_tmp/h5combo.log";
constexpr const char *kThreadMarker = "/system_tmp/h5combo.pid";
constexpr const char *kThreadStop = "/system_tmp/h5combo.stop";
constexpr int kMaxMinutes = 120;
constexpr int kMaxShortcuts = 32;

struct Shortcut {
    char domain[24], service[24], entity[128], data[96], name[48];
    int confirm;   /* 0 bar, 1 notify, 2 both */
};

Shortcut g_sc[kMaxShortcuts];
int g_nsc;
char g_url[400], g_access[2048];

void say(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    klog_printf("[home5-reader] %s\n", line);
    if (FILE *fp = std::fopen(kLogPath, "a")) {
        std::fprintf(fp, "%ld %s\n", (long)time(nullptr), line);
        std::fclose(fp);
    }
}

void notify(const char *msg, const char *sub) {
    char json[1024];
    snprintf(json, sizeof json,
             "{\"rawData\":{\"viewTemplateType\":\"InteractiveToastTemplateB\",\"channelType\":\"Downloads\","
             "\"useCaseId\":\"IDC\",\"toastOverwriteType\":\"No\",\"isImmediate\":true,\"priority\":100,"
             "\"viewData\":{\"message\":{\"body\":\"%s\"},\"subMessage\":{\"body\":\"%s\"}},"
             "\"platformViews\":{\"previewDisabled\":{\"viewData\":{\"icon\":{\"type\":\"Predefined\","
             "\"parameters\":{\"icon\":\"download\"}},\"message\":{\"body\":\"%s\"}}}}},"
             "\"createdDateTime\":\"2026-11-09T00:00:00.000Z\",\"localNotificationId\":\"%d\"}",
             msg, sub, msg, (int)(time(nullptr) & 0x7fffffff));
    sceNotificationSend(0xFE, true, json);
}

char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + std::strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) *--end = 0;
    return s;
}

/* Reads shortcuts.cfg: url/access and one Shortcut per sc line, in the same order and under the same test
 * (a non-empty trigger) the thread uses, so the thread's "FIRE <i>" indexes this array. */
int load_config() {
    FILE *fp = std::fopen(kCfg, "r");
    char line[512];
    if (!fp) return 0;
    g_nsc = 0;
    g_url[0] = g_access[0] = 0;
    while (std::fgets(line, sizeof line, fp)) {
        char *eq = std::strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(line), *val = eq + 1;
        if (!std::strcmp(key, "url")) { h5_copy(g_url, sizeof g_url, trim(val)); continue; }
        if (!std::strcmp(key, "access")) { h5_copy(g_access, sizeof g_access, trim(val)); continue; }
        if (std::strcmp(key, "sc") || g_nsc >= kMaxShortcuts) continue;
        char *f[8];
        int nf = 0;
        f[nf++] = val;
        for (char *p = val; *p && nf < 8; p++)
            if (*p == '|') { *p = 0; f[nf++] = p + 1; }
        if (nf < 7) continue;
        int len = 0;
        char keys[128];
        h5_copy(keys, sizeof keys, trim(f[6]));
        if (!std::strcmp(trim(f[0]), "seq")) {
            for (char *tok = std::strtok(keys, ","); tok; tok = std::strtok(nullptr, ",")) len++;
        } else {
            for (uint32_t m = (uint32_t)std::strtoul(keys, nullptr, 16); m; m &= m - 1) len++;
        }
        if (len == 0) continue;   /* same test as the thread: skip triggers with no buttons */
        Shortcut &s = g_sc[g_nsc++];
        std::memset(&s, 0, sizeof s);
        const char *cf = trim(f[1]);
        s.confirm = !std::strcmp(cf, "notify") ? 1 : !std::strcmp(cf, "both") ? 2 : 0;
        h5_copy(s.domain, sizeof s.domain, trim(f[2]));
        h5_copy(s.service, sizeof s.service, trim(f[3]));
        h5_copy(s.entity, sizeof s.entity, trim(f[4]));
        h5_copy(s.data, sizeof s.data, trim(f[5]));
        if (nf > 7) h5_copy(s.name, sizeof s.name, trim(f[7]));
    }
    std::fclose(fp);
    return 1;
}

int ha_fire(const Shortcut &s) {
    H5Url url;
    if (h5_url_parse(g_url, &url)) { say("bad url: %s", g_url); return -1; }
    char path[256], body[256];
    H5Buf out = { 0 };
    int status = 0;
    char err[256] = "";
    snprintf(path, sizeof path, "/api/services/%s/%s", s.domain, s.service);
    if (s.data[0]) snprintf(body, sizeof body, "{\"entity_id\":\"%s\",%s}", s.entity, s.data);
    else snprintf(body, sizeof body, "{\"entity_id\":\"%s\"}", s.entity);
    int rc = h5_http(&url, "POST", path, "application/json", body, g_access, &out, &status, err, sizeof err);
    h5_buf_free(&out);
    if (rc) { say("HA %s.%s failed: %s", s.domain, s.service, err); return -1; }
    say("HA %s.%s on %s -> %d", s.domain, s.service, s.entity, status);
    return (status >= 200 && status < 300) ? 0 : -1;
}

/* --- finding and injecting SceShellUI (from EnriqueWood's PoC) --- */

pid_t find_shellui() {
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0 };
    size_t size = 0;
    if (sysctl(mib, 4, nullptr, &size, nullptr, 0) != 0) { say("sysctl size failed errno=%d", errno); return -1; }
    size += size / 4;
    auto *buf = static_cast<uint8_t *>(std::malloc(size));
    if (!buf || sysctl(mib, 4, buf, &size, nullptr, 0) != 0) { say("sysctl failed errno=%d", errno); std::free(buf); return -1; }
    pid_t by_comm = -1, by_offset = -1;
    int n_comm = 0, n_offset = 0;
    for (uint8_t *p = buf; p + sizeof(int) <= buf + size;) {
        auto *ki = reinterpret_cast<struct kinfo_proc *>(p);
        int rs = ki->ki_structsize;
        if (rs <= 0 || p + rs > buf + size) break;
        if (std::strncmp(ki->ki_comm, "SceShellUI", sizeof ki->ki_comm) == 0) { by_comm = ki->ki_pid; n_comm++; }
        if (rs > 447 + 11 && std::memcmp(p + 447, "SceShellUI", 11) == 0) {
            pid_t pid;
            std::memcpy(&pid, p + 72, sizeof pid);
            by_offset = pid;
            n_offset++;
        }
        p += rs;
    }
    std::free(buf);
    if (n_comm == 1 && (n_offset == 0 || (n_offset == 1 && by_offset == by_comm))) return by_comm;
    if (n_comm == 0 && n_offset == 1) return by_offset;
    say("SceShellUI not found exactly once, refusing to inject");
    return -1;
}

int read_marker() {
    FILE *fp = std::fopen(kThreadMarker, "r");
    if (!fp) return -1;
    int pid = -1;
    if (std::fscanf(fp, "%d", &pid) != 1) pid = -1;
    std::fclose(fp);
    return pid;
}

bool other_injector_present() {
    DIR *dir = opendir("/system_tmp");
    if (!dir) return false;
    bool found = false;
    while (dirent *e = readdir(dir))
        if (std::strncmp(e->d_name, "padoverlay", 10) == 0 || std::strncmp(e->d_name, "commonfps", 9) == 0 ||
            std::strncmp(e->d_name, "CFPS", 4) == 0 || std::strncmp(e->d_name, "fps_sample", 10) == 0) {
            say("other injector file: /system_tmp/%s", e->d_name);
            found = true;
        }
    closedir(dir);
    return found;
}

bool inject(pid_t pid) {
    say("inject start pid=%d elf=%u bytes", pid, h5r_thread_elf_len);
    const auto r = common_fps::ps5::inject_shellui_stable(pid, h5r_thread_elf, h5r_thread_elf_len);
    say("inject: attach=%d load=%d thread=%d rc=%d detach=%d imports=%u/%u first_unresolved=%s", r.attached,
        r.elf_loaded, r.pthread_create_ok, r.pthread_create_rc, r.detached, r.imports_resolved, r.imports_unresolved,
        r.first_unresolved && *r.first_unresolved ? r.first_unresolved : "-");
    return r.pthread_create_ok;
}

void follow(pid_t pid) {
    long offset = 0;
    if (FILE *fp = std::fopen(kThreadLog, "r")) { std::fseek(fp, 0, SEEK_END); offset = std::ftell(fp); std::fclose(fp); }
    const time_t until = time(nullptr) + kMaxMinutes * 60;
    while (time(nullptr) < until) {
        if (access(kStopPath, F_OK) == 0 || access(kParkPath, F_OK) == 0) { say("stop or park file found"); return; }
        if (FILE *fp = std::fopen(kThreadLog, "r")) {
            std::fseek(fp, 0, SEEK_END);
            long end = std::ftell(fp);
            if (end < offset) offset = 0;
            std::fseek(fp, offset, SEEK_SET);
            char line[256];
            while (std::fgets(line, sizeof line, fp)) {
                char *fire = std::strstr(line, "FIRE ");
                if (!fire) continue;
                int i = std::atoi(fire + 5);
                load_config();   /* always the latest shortcuts and token */
                if (i < 0 || i >= g_nsc) { say("fired %d but only %d shortcuts", i, g_nsc); continue; }
                const Shortcut &s = g_sc[i];
                say("shortcut %d (%s) fired", i, s.name);
                ha_fire(s);
                if (s.confirm == 1 || s.confirm == 2) {
                    char body[96];
                    snprintf(body, sizeof body, "%s", s.name[0] ? s.name : "Shortcut");
                    notify("Home5", body);
                }
            }
            offset = std::ftell(fp);
            std::fclose(fp);
        }
        if (read_marker() != pid) { say("thread marker gone (ShellUI restarted?), stopping"); return; }
        usleep(100000);
    }
    say("time limit of %d min reached", kMaxMinutes);
}

} // namespace

int main() {
    mkdir(kDir, 0777);
    unlink(kStopPath);
    unlink(kParkPath);
    say("=== home5 shortcuts reader pid=%d ===", getpid());

    if (!load_config() || g_nsc == 0) {
        say("no shortcuts configured (open Home5 and set some up)");
        notify("Home5 shortcuts", "No shortcuts set up yet");
        return 1;
    }
    say("%d shortcuts, url=%s token=%d", g_nsc, g_url, g_access[0] != 0);
    h5r_net_start();

    const pid_t pid = find_shellui();
    if (pid <= 0) return 1;

    if (read_marker() == pid) {
        say("thread already running in ShellUI pid=%d, following it", pid);
    } else {
        if (other_injector_present()) {
            say("another ShellUI injector is present, refusing to inject");
            notify("Home5 shortcuts", "Another ShellUI tool is running: not started");
            return 1;
        }
        unlink(kThreadStop);
        if (!inject(pid)) { notify("Home5 shortcuts", "Could not start, see reader.log"); return 1; }
        int waited = 0;
        while (read_marker() != pid && waited < 150) { usleep(100000); waited++; }
        if (read_marker() != pid) { notify("Home5 shortcuts", "Reader did not start, see reader.log"); return 1; }
        say("thread up in ShellUI pid=%d after %d ms", pid, waited * 100);
    }

    notify("Home5 shortcuts are listening", "Your combos work while you play");
    follow(pid);

    if (access(kParkPath, F_OK) == 0) {
        if (FILE *fp = std::fopen(kThreadStop, "w")) std::fclose(fp);
        say("asked the thread to park; reader exits");
        notify("Home5 shortcuts", "Stopped, reader parked");
    } else {
        say("reader exits; the thread keeps reading (load the reader again to follow it)");
        notify("Home5 shortcuts", "Stopped");
    }
    return 0;
}
