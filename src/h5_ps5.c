/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Home5 on the console: the screen (VideoOut, drawn by the CPU), the
 * DualSense (buttons, light bar, adaptive triggers), the PS5's keyboard (the
 * IME dialog), notifications, the clock, and the network (libSceNet) for
 * h5_net.c. Everything else is the same code the host tests run.
 *
 * The console's interfaces are declared here as the PS5 uses them; the
 * linker binds them to the system's modules through the payload SDK's stubs
 * (tools/build.sh). How the frame is laid out in display memory and how the
 * title starts follow the PS5 Native App Boilerplate (GPL-3.0-or-later). */
#include "h5.h"
#include "h5_net.h"
#include "h5_ui.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ---- the console's interfaces ----------------------------------------------------------------- */

/* libkernel */
int sceKernelUsleep(uint32_t microseconds);
uint64_t sceKernelGetProcessTime(void);
size_t sceKernelGetDirectMemorySize(void);
int sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t length, size_t alignment, int type,
                                  int64_t *physical);
int sceKernelMapDirectMemory(void **address, size_t length, int protection, int flags, int64_t physical,
                             size_t alignment);
typedef struct NotifyRequest {
    char reserved[45];
    char message[3075];
} NotifyRequest;
int sceKernelSendNotificationRequest(int device, NotifyRequest *request, size_t size, int blocking);

/* libSceSystemService */
int sceSystemServiceHideSplashScreen(void);
int sceSystemServiceParamGetInt(int id, int *value);
enum { PARAM_TIME_ZONE = 4, PARAM_SUMMERTIME = 5, PARAM_ENTER_BUTTON = 1000 };

/* libSceUserService, libSceSysmodule, libSceCommonDialog */
int sceUserServiceInitialize(const void *params);
int sceUserServiceGetInitialUser(int32_t *user);
int sceUserServiceGetForegroundUser(int32_t *user);
int sceSysmoduleLoadModule(uint16_t id);
int sceCommonDialogInitialize(void);
enum { SYSMODULE_IME_DIALOG = 0x0096 };

/* libSceVideoOut */
typedef struct VideoBuffer { void *data, *metadata, *reserved[2]; } VideoBuffer;
typedef struct VideoAttribute { uint8_t reserved[80]; } VideoAttribute;
int sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void *param);
int sceVideoOutSetFlipRate(int32_t handle, int32_t rate);
void sceVideoOutSetBufferAttribute2(VideoAttribute *attribute, uint64_t format, uint32_t tiling, uint32_t width,
                                    uint32_t height, uint64_t option, uint32_t dcc_control, uint64_t dcc_clear);
int sceVideoOutRegisterBuffers2(int32_t handle, int32_t set, int32_t first, VideoBuffer *buffers, int32_t count,
                                VideoAttribute *attribute, int32_t category, void *option);
int sceVideoOutSubmitFlip(int32_t handle, int32_t index, uint32_t mode, int64_t argument);
int sceVideoOutWaitVblank(int32_t handle);

/* libScePad: one reading (ScePadData, 120 bytes). */
typedef struct PadData {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2, analog_reserved[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    uint8_t touch[24];
    uint8_t connected;
    uint8_t reserved0[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count, reserved1[2], unique_length, unique[12];
} PadData;
_Static_assert(sizeof(PadData) == 120, "ScePadData is 120 bytes");
_Static_assert(offsetof(PadData, connected) == 76, "ScePadData.connected is at 76");
typedef struct LightBar { uint8_t r, g, b, reserved; } LightBar;
/* The adaptive triggers (ScePadTriggerEffectParam): which triggers, then a
 * command for L2 and one for R2. Feedback: where it starts (0-9), how firm (1-8). */
typedef struct TriggerCommand { uint32_t mode; uint8_t padding[4]; uint8_t data[48]; } TriggerCommand;
typedef struct TriggerParam { uint8_t mask; uint8_t padding[7]; TriggerCommand command[2]; } TriggerParam;
_Static_assert(sizeof(TriggerParam) == 120, "ScePadTriggerEffectParam is 120 bytes");
int scePadInit(void);
int scePadOpen(int32_t user, int32_t type, int32_t index, const void *param);
int scePadRead(int32_t handle, PadData *data, int32_t count);
int scePadSetLightBar(int32_t handle, const LightBar *light);
int scePadResetLightBar(int32_t handle);
int scePadSetTriggerEffect(int32_t handle, const TriggerParam *param);
enum {
    PAD_OPTIONS = 0x8u, PAD_UP = 0x10u, PAD_RIGHT = 0x20u, PAD_DOWN = 0x40u, PAD_LEFT = 0x80u, PAD_L2 = 0x100u,
    PAD_R2 = 0x200u, PAD_L1 = 0x400u, PAD_R1 = 0x800u, PAD_TRIANGLE = 0x1000u, PAD_CIRCLE = 0x2000u,
    PAD_CROSS = 0x4000u, PAD_SQUARE = 0x8000u,
};
#define PAD_INTERCEPTED 0x80000000u   /* the system has the controller */

/* libSceImeDialog (SceImeDialogParam, 96 bytes; its result, 16). Text is UTF-16. */
typedef struct ImeParam {
    int32_t user;
    uint32_t type;
    uint64_t languages;
    uint32_t enter_label, input_method;
    void *filter;
    uint32_t option, max_length;
    uint16_t *text;
    float x, y;
    uint32_t horizontal, vertical;
    const uint16_t *placeholder, *title;
    int8_t reserved[16];
} ImeParam;
_Static_assert(sizeof(ImeParam) == 96, "SceImeDialogParam is 96 bytes");
typedef struct ImeResult { int32_t outcome; int8_t reserved[12]; } ImeResult;
int sceImeDialogInit(const ImeParam *param, const void *extended);
int sceImeDialogGetStatus(void);
int sceImeDialogGetResult(ImeResult *result);
int sceImeDialogTerm(void);
enum { IME_DEFAULT = 0, IME_BASIC_LATIN = 1, IME_URL = 2, IME_NUMBER = 4 };
enum { IME_NO_CAPITALS = 0x2, IME_PASSWORD = 0x4, IME_NO_LEARNING = 0x20 };
enum { IME_RUNNING = 1, IME_FINISHED = 2 };

/* libSceNet: sockets, its epoll, and the system's name resolver. */
typedef struct NetEvent {
    uint32_t events, padding;
    uint64_t ident;
    uint64_t data;
} NetEvent;
_Static_assert(sizeof(NetEvent) == 24, "SceNetEpollEvent is 24 bytes");
int sceNetInit(void);
int sceNetSocket(const char *name, int family, int type, int protocol);
int sceNetConnect(int sock, const struct sockaddr *address, socklen_t length);
int sceNetSend(int sock, const void *data, size_t size, int flags);
int sceNetRecv(int sock, void *data, size_t size, int flags);
int sceNetSetsockopt(int sock, int level, int option, const void *value, socklen_t length);
int sceNetGetsockopt(int sock, int level, int option, void *value, socklen_t *length);
int sceNetSocketClose(int sock);
int sceNetEpollCreate(const char *name, int flags);
int sceNetEpollControl(int epoll, int op, int sock, NetEvent *event);
int sceNetEpollWait(int epoll, NetEvent *events, int count, int timeout_us);
int sceNetEpollDestroy(int epoll);
int *sceNetErrnoLoc(void);
int sceNetPoolCreate(const char *name, int size, int flags);
int sceNetPoolDestroy(int pool);
int sceNetResolverCreate(const char *name, int pool, int flags);
int sceNetResolverStartNtoa(int resolver, const char *host, struct in_addr *address, int timeout_us, int retries,
                            int flags);
int sceNetResolverDestroy(int resolver);
enum { NET_IN = 0x1, NET_OUT = 0x2, NET_ERR = 0x8, NET_HUP = 0x10, NET_CTL_ADD = 1, NET_NBIO = 0x1200 };

/* ---- the log -----------------------------------------------------------------------------------
 * <data>/home5.log, new each start; also on a USB drive that has an (empty)
 * home5-log.txt in its top folder, for when something goes wrong on the PS5. */

static const char data_dir[] = "/download0/home5";
static const char *const usb_roots[] = { "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3", "/mnt/usb4",
                                         "/mnt/usb5", "/mnt/usb6", "/mnt/usb7", NULL };
static int log_fd = -1, usb_log_fd = -1;

static void log_line(const char *line)
{
    char stamp[24];
    uint64_t ms = sceKernelGetProcessTime() / 1000;
    int n = snprintf(stamp, sizeof(stamp), "%5llu.%03llu ", (unsigned long long)(ms / 1000),
                     (unsigned long long)(ms % 1000));
    for (int i = 0; i < 2; i++) {
        int fd = i ? usb_log_fd : log_fd;
        if (fd < 0) continue;
        if (write(fd, stamp, (size_t)n) < 0 || write(fd, line, strlen(line)) < 0 || write(fd, "\n", 1) < 0) {
            /* A full or pulled drive: no more log there. */
            close(fd);
            if (i) usb_log_fd = -1; else log_fd = -1;
        }
    }
}

static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    log_line(line);
}

static void open_logs(void)
{
    char path[H5_PATH];
    h5_mkdirs(data_dir);
    if (!h5_join(path, sizeof(path), data_dir, "home5.log"))
        log_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    for (const char *const *root = usb_roots; *root && usb_log_fd < 0; root++)
        if (!h5_join(path, sizeof(path), *root, "home5-log.txt") && h5_is_file(path))
            usb_log_fd = open(path, O_WRONLY | O_APPEND);
}

/* ---- notifications, the clock --------------------------------------------------------------------- */

static void notify_text(const char *text)
{
    static NotifyRequest request;
    memset(&request, 0, sizeof(request));
    h5_copy(request.message, sizeof(request.message), text);
    int rc = sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
    if (rc) say("H5 ps5 notification=%d", rc);
}

static void notify(void *context, const char *title, const char *message)
{
    char text[3075];
    (void)context;
    if (title && title[0] && message && message[0]) snprintf(text, sizeof(text), "%s\n%s", title, message);
    else h5_copy(text, sizeof(text), title && title[0] ? title : message ? message : "");
    notify_text(text);
}

/* Stops here for good: a title must not return from main. */
static void halt(const char *message)
{
    say("H5 ps5 halt: %s", message);
    notify_text(message);
    for (;;) sceKernelUsleep(1000000);
}

static uint64_t clock_ms(void)
{
    return sceKernelGetProcessTime() / 1000;
}

static int64_t unix_time(void)
{
    time_t now = time(NULL);
    return now < 1600000000 ? 0 : (int64_t)now;
}

/* ---- the network (h5_net.h) ------------------------------------------------------------------------ */

static int net_errno(void)
{
    int *code = sceNetErrnoLoc();
    return code ? *code : 0;
}

/* 0 when sock is ready for events, H5_NET_AGAIN when timeout_ms passed, -1 on an error. */
static int net_wait(int sock, uint32_t events, int timeout_ms)
{
    NetEvent interest = { events, 0, 0, 0 }, ready = { 0, 0, 0, 0 };
    int epoll = sceNetEpollCreate("home5", 0), found;
    if (epoll < 0) return -1;
    if (sceNetEpollControl(epoll, NET_CTL_ADD, sock, &interest) < 0) {
        sceNetEpollDestroy(epoll);
        return -1;
    }
    found = sceNetEpollWait(epoll, &ready, 1, timeout_ms < 0 ? -1 : timeout_ms * 1000);
    sceNetEpollDestroy(epoll);
    if (found < 0) return -1;
    if (found == 0) return H5_NET_AGAIN;
    /* An error or a hang-up shows up as the next send or receive failing. */
    return 0;
}

static int net_resolve(const char *host, uint32_t *address)
{
    struct in_addr found;
    int pool = sceNetPoolCreate("home5-dns", 16 * 1024, 0), status = -1;
    if (pool < 0) return -1;
    int resolver = sceNetResolverCreate("home5-dns", pool, 0);
    if (resolver >= 0) {
        if (sceNetResolverStartNtoa(resolver, host, &found, 5 * 1000 * 1000, 2, 0) >= 0) {
            *address = found.s_addr;
            status = 0;
        }
        sceNetResolverDestroy(resolver);
    }
    sceNetPoolDestroy(pool);
    return status;
}

static int net_connect(uint32_t address, uint16_t port, int timeout_ms)
{
    struct sockaddr_in to;
    int sock = sceNetSocket("home5", AF_INET, SOCK_STREAM, 0), on = 1, error = 0;
    socklen_t length = sizeof(error);
    if (sock < 0) return -1;
    memset(&to, 0, sizeof(to));
    to.sin_len = sizeof(to);
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = address;
    /* Without blocking, so every wait has its limit. */
    if (sceNetSetsockopt(sock, SOL_SOCKET, NET_NBIO, &on, sizeof(on)) < 0) goto fail;
    if (sceNetConnect(sock, (const struct sockaddr *)&to, sizeof(to)) < 0) {
        int code = net_errno();
        if (code != EINPROGRESS && code != EAGAIN) goto fail;
        if (net_wait(sock, NET_OUT, timeout_ms)) goto fail;
        if (sceNetGetsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &length) < 0 || error) goto fail;
    }
    return sock;
fail:
    sceNetSocketClose(sock);
    return -1;
}

static int net_send(int sock, const void *data, size_t size, int timeout_ms)
{
    for (;;) {
        int sent = sceNetSend(sock, data, size, 0);
        if (sent >= 0) return sent;
        if (net_errno() != EAGAIN) return -1;
        int wait = net_wait(sock, NET_OUT, timeout_ms);
        if (wait) return wait;
    }
}

static int net_recv(int sock, void *data, size_t size, int timeout_ms)
{
    for (;;) {
        int got = sceNetRecv(sock, data, size, 0);
        if (got >= 0) return got;
        if (net_errno() != EAGAIN) return -1;
        int wait = net_wait(sock, NET_IN, timeout_ms);
        if (wait) return wait;
    }
}

static void net_close(int sock)
{
    if (sock >= 0) sceNetSocketClose(sock);
}

/* The kernel's generator (kern.arandom), up to 256 bytes a call; the CPU's
 * RDRAND when that is refused. */
static int net_random(void *data, size_t size)
{
    uint8_t *out = data;
    size_t done = 0;
    while (done < size) {
        int mib[2] = { CTL_KERN, KERN_ARND };
        size_t want = size - done > 256 ? 256 : size - done, got = want;
        if (sysctl(mib, 2, out + done, &got, NULL, 0) || !got) break;
        done += got;
    }
    while (done < size) {
        unsigned long long value;
        unsigned char ok = 0;
        for (int tries = 0; tries < 10 && !ok; tries++) __asm__ volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(ok));
        if (!ok || value == ~0ull) return -1;
        size_t take = size - done < 8 ? size - done : 8;
        memcpy(out + done, &value, take);
        done += take;
    }
    return 0;
}

/* ---- the screen ------------------------------------------------------------------------------------
 * Two 1920x1080 buffers in display memory, A8B8G8R8 in the GPU's tiled
 * layout: 128x128-pixel tiles of 64 KiB, a row of 15 across, the pixels in a
 * tile in a fixed swizzle. The UI draws into ordinary memory; a frame is
 * copied over in the order the display memory is laid out, red and blue
 * swapped. */

enum { W = 1920, H = 1080, TILE = 128, TILES_X = W / TILE, TILES_Y = (H + TILE - 1) / TILE };
enum { BUFFER_BYTES = 0x1000000, DISPLAY_ALIGN = 0x200000 };

static int video = -1, back;
static uint32_t *buffers[2];
static uint16_t tile_x[TILE * TILE], tile_y[TILE * TILE];   /* a tile's pixel n: where it is in the tile */

/* Where pixel (x, y) of a tile is, in bytes from the tile's start. */
static uint32_t swizzle(uint32_t x, uint32_t y)
{
    return ((y << 4) & 0x70u) ^ ((y << 5) & 0xf00u) ^ ((y << 9) & 0x1000u) ^ ((y << 8) & 0x4000u) ^ ((x << 2) & 0xcu) ^
           ((x << 5) & 0x380u) ^ ((x << 4) & 0x400u) ^ ((x << 6) & 0x800u) ^ ((x << 9) & 0xa000u);
}

static int make_tile_map(void)
{
    static uint8_t seen[TILE * TILE];
    for (uint32_t y = 0; y < TILE; y++)
        for (uint32_t x = 0; x < TILE; x++) {
            uint32_t n = swizzle(x, y) >> 2;
            if (n >= TILE * TILE || seen[n]) return -1;   /* not one pixel to one place */
            seen[n] = 1;
            tile_x[n] = (uint16_t)x;
            tile_y[n] = (uint16_t)y;
        }
    return 0;
}

static void flush(const void *address, size_t size)
{
    for (const uint8_t *at = address, *end = at + size; at < end; at += 64) __asm__ volatile("clflush (%0)" ::"r"(at) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}

static int open_screen(void)
{
    int64_t physical = 0;
    void *memory = NULL;
    video = sceVideoOutOpen(0xff, 0, 0, NULL);
    if (video < 0) return say("H5 ps5 VideoOut open=%#x", (unsigned)video), -1;
    int rc = sceKernelAllocateDirectMemory(0, (int64_t)sceKernelGetDirectMemorySize(), 2 * BUFFER_BYTES, DISPLAY_ALIGN,
                                           3 /* write-combined */, &physical);
    if (rc < 0) return say("H5 ps5 display memory=%#x", (unsigned)rc), -1;
    rc = sceKernelMapDirectMemory(&memory, 2 * BUFFER_BYTES, 0x33, 0, physical, DISPLAY_ALIGN);
    if (rc < 0) return say("H5 ps5 display map=%#x", (unsigned)rc), -1;
    memset(memory, 0, 2 * BUFFER_BYTES);
    flush(memory, 2 * BUFFER_BYTES);
    buffers[0] = memory;
    buffers[1] = (uint32_t *)((uint8_t *)memory + BUFFER_BYTES);
    VideoBuffer list[2] = { { buffers[0], NULL, { NULL, NULL } }, { buffers[1], NULL, { NULL, NULL } } };
    VideoAttribute attribute;
    memset(&attribute, 0, sizeof(attribute));
    sceVideoOutSetFlipRate(video, 0);
    sceVideoOutSetBufferAttribute2(&attribute, 0x8000000022000000ull, 0, W, H, 0, 0, 0);
    rc = sceVideoOutRegisterBuffers2(video, 0, 0, list, 2, &attribute, 0, NULL);
    if (rc < 0) return say("H5 ps5 VideoOut buffers=%#x", (unsigned)rc), -1;
    return 0;
}

static void present(const H5Canvas *canvas)
{
    uint32_t *out = buffers[back];
    for (int ty = 0; ty < TILES_Y; ty++)
        for (int tx = 0; tx < TILES_X; tx++) {
            const uint32_t *source = canvas->px + (size_t)ty * TILE * (size_t)canvas->stride + (size_t)tx * TILE;
            int rows = H - ty * TILE < TILE ? H - ty * TILE : TILE;
            for (int n = 0; n < TILE * TILE; n++) {
                uint32_t p = 0;
                if (tile_y[n] < rows) {
                    p = source[(size_t)tile_y[n] * (size_t)canvas->stride + tile_x[n]];
                    p = (p & 0xff00ff00u) | ((p >> 16) & 0xffu) | ((p & 0xffu) << 16);
                }
                *out++ = p;
            }
        }
    flush(buffers[back], (size_t)TILES_X * TILES_Y * TILE * TILE * 4);
    int rc = sceVideoOutSubmitFlip(video, back, 1, 0);
    if (rc < 0) say("H5 ps5 flip=%#x", (unsigned)rc);
    sceVideoOutWaitVblank(video);
    back ^= 1;
}

/* ---- the controller ---------------------------------------------------------------------------------- */

static int32_t user = -1;
static int pad = -1;
static uint64_t pad_retry_at;

static void open_pad(uint64_t now)
{
    if (pad >= 0 || now < pad_retry_at) return;
    pad_retry_at = now + 2000;
    if (sceUserServiceGetInitialUser(&user) < 0 || user < 0) sceUserServiceGetForegroundUser(&user);
    if (user < 0) return;
    pad = scePadOpen(user, 0, 0, NULL);
    if (pad < 0) say("H5 ps5 pad open user=%d rc=%#x", (int)user, (unsigned)pad);
    else say("H5 ps5 pad open user=%d", (int)user);
}

/* The buttons held now, as H5_KEY bits (the left stick moves too). */
static uint32_t read_pad(void)
{
    static PadData records[64];
    static uint32_t held;
    if (pad < 0) return 0;
    int count = scePadRead(pad, records, 64);
    if (count < 0) {
        say("H5 ps5 pad read=%#x", (unsigned)count);
        pad = -1;
        return held = 0;
    }
    if (count == 0) return held;   /* nothing new: as it was */
    const PadData *now = &records[count - 1];
    uint32_t b = now->buttons;
    held = 0;
    /* Off while the system has the controller (the PS menu, the keyboard). */
    if (!now->connected || (b & PAD_INTERCEPTED)) return held;
    static const struct { uint32_t pad, key; } map[] = {
        { PAD_UP, H5_KEY_UP }, { PAD_DOWN, H5_KEY_DOWN }, { PAD_LEFT, H5_KEY_LEFT }, { PAD_RIGHT, H5_KEY_RIGHT },
        { PAD_CROSS, H5_KEY_CROSS }, { PAD_CIRCLE, H5_KEY_CIRCLE }, { PAD_TRIANGLE, H5_KEY_TRIANGLE },
        { PAD_SQUARE, H5_KEY_SQUARE }, { PAD_OPTIONS, H5_KEY_OPTIONS }, { PAD_L1, H5_KEY_L1 }, { PAD_R1, H5_KEY_R1 },
        { PAD_L2, H5_KEY_L2 }, { PAD_R2, H5_KEY_R2 },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (b & map[i].pad) held |= map[i].key;
    /* Past a third of the stick's travel. */
    if (now->left_x < 0x40) held |= H5_KEY_LEFT;
    if (now->left_x > 0xc0) held |= H5_KEY_RIGHT;
    if (now->left_y < 0x40) held |= H5_KEY_UP;
    if (now->left_y > 0xc0) held |= H5_KEY_DOWN;
    return held;
}

/* Edges, and repeats while a direction (or L2/R2, for brightness and the
 * temperature) stays held. */
static uint32_t pressed_keys(uint32_t held, uint64_t now)
{
    static uint32_t before;
    static uint64_t next[16];
    uint32_t pressed = held & ~before;
    for (int bit = 0; bit < 16; bit++) {
        uint32_t key = 1u << bit;
        int direction = key & (H5_KEY_UP | H5_KEY_DOWN | H5_KEY_LEFT | H5_KEY_RIGHT);
        if (!(key & (H5_KEY_UP | H5_KEY_DOWN | H5_KEY_LEFT | H5_KEY_RIGHT | H5_KEY_L2 | H5_KEY_R2))) continue;
        if (pressed & key) next[bit] = now + (direction ? 380 : 320);
        else if ((held & key) && now >= next[bit]) {
            pressed |= key;
            next[bit] = now + (direction ? 90 : 140);
        }
    }
    before = held;
    return pressed;
}

static void controller(const H5Ui *ui)
{
    static int32_t shown = -2;
    static int pushing = -1;
    if (pad < 0) {
        shown = -2;
        pushing = -1;
        return;
    }
    int32_t light = h5_ui_light_bar(ui);
    if (light != shown) {
        LightBar bar = { (uint8_t)(light >> 16), (uint8_t)(light >> 8), (uint8_t)light, 0 };
        int rc = light < 0 ? scePadResetLightBar(pad) : scePadSetLightBar(pad, &bar);
        if (rc) say("H5 ps5 light bar=%#x", (unsigned)rc);
        shown = light;
    }
    int push = h5_ui_triggers(ui) != 0;
    if (push != pushing) {
        TriggerParam param;
        memset(&param, 0, sizeof(param));
        param.mask = 0x3;   /* L2 and R2 */
        for (int i = 0; i < 2 && push; i++) {
            param.command[i].mode = 1;      /* feedback */
            param.command[i].data[0] = 2;   /* from a little way in */
            param.command[i].data[1] = 3;   /* gently */
        }
        int rc = scePadSetTriggerEffect(pad, &param);
        if (rc) say("H5 ps5 triggers=%#x", (unsigned)rc);
        pushing = push;
    }
}

/* ---- the keyboard -----------------------------------------------------------------------------------
 * The IME dialog, over the screen; h5_ui_text gets its answer. */

enum { IME_TEXT = 256 };
static H5Ui *the_ui;
static int ime_open;
static uint16_t ime_text[IME_TEXT + 1], ime_title[129];

/* UTF-8 into UTF-16 (to the 0 or max units), bad bytes as U+FFFD. */
static void to_utf16(const char *in, uint16_t *out, size_t max)
{
    const uint8_t *s = (const uint8_t *)in;
    size_t n = 0;
    while (*s && n < max) {
        uint32_t c = *s++, extra = 0;
        if (c >= 0xf0 && c < 0xf8) c &= 0x07, extra = 3;
        else if (c >= 0xe0) c &= 0x0f, extra = 2;
        else if (c >= 0xc0) c &= 0x1f, extra = 1;
        else if (c >= 0x80) c = 0xfffd;
        for (; extra && (*s & 0xc0) == 0x80; extra--) c = (c << 6) | (*s++ & 0x3f);
        if (extra) c = 0xfffd;
        if (c >= 0x10000) {
            if (n + 2 > max) break;
            c -= 0x10000;
            out[n++] = (uint16_t)(0xd800 | (c >> 10));
            out[n++] = (uint16_t)(0xdc00 | (c & 0x3ff));
        } else {
            out[n++] = (uint16_t)c;
        }
    }
    out[n] = 0;
}

static void to_utf8(const uint16_t *in, char *out, size_t size)
{
    size_t n = 0;
    for (; *in; in++) {
        uint32_t c = *in;
        if (c >= 0xd800 && c < 0xdc00 && in[1] >= 0xdc00 && in[1] < 0xe000) c = 0x10000 + ((c - 0xd800) << 10) + (*++in - 0xdc00);
        else if (c >= 0xd800 && c < 0xe000) c = 0xfffd;
        size_t len = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (n + len >= size) break;
        if (len == 1) out[n++] = (char)c;
        else {
            out[n++] = (char)(len == 2 ? 0xc0 | (c >> 6) : len == 3 ? 0xe0 | (c >> 12) : 0xf0 | (c >> 18));
            for (size_t k = len - 1; k > 0; k--) out[n++] = (char)(0x80 | ((c >> (6 * (k - 1))) & 0x3f));
        }
    }
    out[n] = 0;
}

static int keyboard(void *context, const char *title, const char *text, int kind)
{
    ImeParam param;
    (void)context;
    if (ime_open || user < 0) return -1;
    memset(&param, 0, sizeof(param));
    to_utf16(title, ime_title, 128);
    param.user = user;
    param.max_length = kind == H5_TEXT_NUMBER ? 8 : kind == H5_TEXT_NAME ? 60 : kind == H5_TEXT_ADDRESS ? 200 : 128;
    to_utf16(kind == H5_TEXT_PASSWORD ? "" : text, ime_text, param.max_length);
    param.type = kind == H5_TEXT_ADDRESS ? IME_URL : kind == H5_TEXT_NUMBER ? IME_NUMBER
               : kind == H5_TEXT_PASSWORD ? IME_BASIC_LATIN : IME_DEFAULT;
    param.option = kind == H5_TEXT_NAME ? 0 : kind == H5_TEXT_PASSWORD ? IME_PASSWORD | IME_NO_LEARNING : IME_NO_CAPITALS;
    param.text = ime_text;
    param.x = W / 2.0f;
    param.y = H / 2.0f;
    param.horizontal = 1;   /* centred */
    param.vertical = 1;
    param.title = ime_title;
    int rc = sceImeDialogInit(&param, NULL);
    if (rc < 0) {
        say("H5 ps5 keyboard=%#x", (unsigned)rc);
        return -1;
    }
    ime_open = 1;
    return 0;
}

static void keyboard_poll(void)
{
    char text[IME_TEXT * 3 + 1];
    ImeResult result;
    if (!ime_open) return;
    int status = sceImeDialogGetStatus();
    if (status == IME_RUNNING) return;
    memset(&result, 0, sizeof(result));
    int ok = status == IME_FINISHED && sceImeDialogGetResult(&result) >= 0 && result.outcome == 0;
    to_utf8(ime_text, text, sizeof(text));
    sceImeDialogTerm();
    ime_open = 0;
    h5_ui_text(the_ui, ok, ok ? text : "");
    memset(ime_text, 0, sizeof(ime_text));   /* a password goes no further */
    memset(text, 0, sizeof(text));
}

/* ---- the title ----------------------------------------------------------------------------------------- */

int main(void)
{
    int time_zone = 0, summer = 0, enter = 1, rc;

    open_logs();
    say("H5 ps5 Home5 %s (%s)", H5_VERSION, H5_TITLE_ID);
    if (sceSystemServiceParamGetInt(PARAM_TIME_ZONE, &time_zone) || time_zone < -720 || time_zone > 840) time_zone = 0;
    if (sceSystemServiceParamGetInt(PARAM_SUMMERTIME, &summer)) summer = 0;
    if (sceSystemServiceParamGetInt(PARAM_ENTER_BUTTON, &enter)) enter = 1;
    if ((rc = sceUserServiceInitialize(NULL)) < 0) say("H5 ps5 user service=%#x", (unsigned)rc);
    if ((rc = scePadInit()) < 0) say("H5 ps5 pad init=%#x", (unsigned)rc);
    if ((rc = sceNetInit()) < 0) say("H5 ps5 net init=%#x", (unsigned)rc);
    if ((rc = sceSysmoduleLoadModule(SYSMODULE_IME_DIALOG)) < 0) say("H5 ps5 keyboard module=%#x", (unsigned)rc);
    if ((rc = sceCommonDialogInitialize()) < 0) say("H5 ps5 common dialog=%#x", (unsigned)rc);   /* already: fine */
    say("H5 ps5 zone=%d summer=%d enter=%d", time_zone, summer, enter);

    static const char *const roots[] = { "/system/common/cert/CA_LIST.cer", "/app0/assets/ca-certificates.crt", NULL };
    static const char *const ca_dirs[] = { "/data/homebrew/PPSA11019/assets", "/app0/assets", NULL };
    const H5NetOps net = { net_resolve, net_connect, net_send, net_recv, net_close, net_random, roots, log_line,
                           ca_dirs };
    h5_net_init(&net);

    if (make_tile_map()) halt("Home5: the screen layout is wrong.");
    if (open_screen()) halt("Home5 couldn't open the screen.");
    H5Canvas canvas = { h5_alloc((size_t)W * H * 4), W, H, W };
    if (!canvas.px) halt("Home5 is out of memory.");

    const H5Platform platform = { NULL, clock_ms, unix_time, time_zone * 60 + (summer ? 3600 : 0), keyboard, notify,
                                  log_line, enter == 0, data_dir, usb_roots };
    the_ui = h5_ui_create(&platform, "/app0/assets/fonts.bin");
    if (!the_ui) halt("Home5 is out of memory.");

    uint64_t frames = 0, drawn = 0, draw_ms = 0, last_report = clock_ms();
    for (int first = 1;; first = 0) {
        uint64_t now = clock_ms();
        open_pad(now);
        H5Input input = { pressed_keys(read_pad(), now) };
        keyboard_poll();
        int dirty = h5_ui_update(the_ui, &input);
        controller(the_ui);
        if (dirty || first) {
            uint64_t before = clock_ms();
            h5_ui_draw(the_ui, &canvas);
            present(&canvas);
            draw_ms += clock_ms() - before;
            drawn++;
            if (first) sceSystemServiceHideSplashScreen();
        } else {
            sceKernelUsleep(12000);
        }
        frames++;
        if (now - last_report >= 60000) {
            say("H5 ps5 frames=%llu drawn=%llu draw_ms=%.1f screen=%s", (unsigned long long)frames,
                (unsigned long long)drawn, drawn ? (double)draw_ms / (double)drawn : 0.0, h5_ui_screen(the_ui));
            frames = drawn = draw_ms = 0;
            last_report = now;
        }
    }
}
