/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Home5 background shortcuts reader: the network, so the reader can reuse Home5's Home Assistant HTTP
 * client (h5_net.c, h5_http). The sockets, name resolution and random bytes come from the console's
 * libSceNet, exactly as the app's h5_ps5.c provides them. */
#include "h5.h"
#include "h5_net.h"

#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/sysctl.h>

/* libSceNet (as the PS5 uses it; the linker binds these to the console's module). */
typedef struct NetEvent { uint32_t events, padding; uint64_t ident, data; } NetEvent;
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
int sceNetResolverStartNtoa(int resolver, const char *host, struct in_addr *address, int timeout_us, int retries, int flags);
int sceNetResolverDestroy(int resolver);
enum { NET_IN = 0x1, NET_OUT = 0x2, NET_CTL_ADD = 1, NET_NBIO = 0x1200 };

static int net_errno(void)
{
    int *code = sceNetErrnoLoc();
    return code ? *code : 0;
}

static int net_wait(int sock, uint32_t events, int timeout_ms)
{
    NetEvent interest = { events, 0, 0, 0 }, ready = { 0, 0, 0, 0 };
    int epoll = sceNetEpollCreate("home5r", 0), found;
    if (epoll < 0) return -1;
    if (sceNetEpollControl(epoll, NET_CTL_ADD, sock, &interest) < 0) { sceNetEpollDestroy(epoll); return -1; }
    found = sceNetEpollWait(epoll, &ready, 1, timeout_ms < 0 ? -1 : timeout_ms * 1000);
    sceNetEpollDestroy(epoll);
    if (found < 0) return -1;
    return found == 0 ? H5_NET_AGAIN : 0;
}

static int net_resolve(const char *host, uint32_t *address)
{
    struct in_addr found;
    int pool = sceNetPoolCreate("home5r-dns", 16 * 1024, 0), status = -1;
    if (pool < 0) return -1;
    int resolver = sceNetResolverCreate("home5r-dns", pool, 0);
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
    int sock = sceNetSocket("home5r", AF_INET, SOCK_STREAM, 0), on = 1, error = 0;
    socklen_t length = sizeof(error);
    if (sock < 0) return -1;
    memset(&to, 0, sizeof(to));
    to.sin_len = sizeof(to);
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = address;
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

/* The console's roots, plus any .crt the user put in the app's assets folder (as the app trusts them). */
static const char *const ca_files[] = { "/system/common/cert/CA_LIST.cer", NULL };
static const char *const ca_dirs[] = { "/data/homebrew/PPSA11019/assets", NULL };

int h5r_net_start(void)
{
    static const H5NetOps ops = { net_resolve, net_connect, net_send, net_recv, net_close, net_random, ca_files,
                                  NULL, ca_dirs };
    int rc = sceNetInit();
    h5_net_init(&ops);
    return rc;
}
