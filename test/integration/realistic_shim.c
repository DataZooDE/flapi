/* LD_PRELOAD shim for the realistic differential / soak suite (#241).
 *
 * Two independent jobs, each switched by an environment variable:
 *
 *  FLAPI_SHIM_NODELAY=1   set TCP_NODELAY on every socket passed to listen(). Accepted sockets
 *                         inherit it on Linux, so a reference binary that predates the
 *                         TCP_NODELAY fix (fc47cf3) answers small keep-alive requests without the
 *                         40 ms delayed-ACK stall and can be compared for speed.
 *
 *  FLAPI_SHIM_SINK_PORT=N keep the process on the box: every name lookup that is not a loopback
 *                         name resolves to 127.0.0.1 (no DNS query leaves), and every connect() to a
 *                         non-loopback address is redirected to 127.0.0.1:N, a local sink that
 *                         accepts and never answers. Telemetry (PostHog, which has no host override
 *                         in the environment) then sees an unreachable, hanging ingestion host.
 *                         Each redirected connect is appended to FLAPI_SHIM_LOG if set.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int sink_port(void) {
    const char *v = getenv("FLAPI_SHIM_SINK_PORT");
    return v ? atoi(v) : 0;
}

int listen(int fd, int backlog) {
    static int (*real_listen)(int, int);
    if (!real_listen) real_listen = dlsym(RTLD_NEXT, "listen");
    const char *v = getenv("FLAPI_SHIM_NODELAY");
    if (v && v[0] == '1') {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    return real_listen(fd, backlog);
}

static int is_loopback_name(const char *node) {
    return node == NULL || strcmp(node, "localhost") == 0 || strncmp(node, "127.", 4) == 0 ||
           strcmp(node, "::1") == 0 || strcmp(node, "0.0.0.0") == 0 || strcmp(node, "::") == 0;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                struct addrinfo **res) {
    static int (*real_gai)(const char *, const char *, const struct addrinfo *, struct addrinfo **);
    if (!real_gai) real_gai = dlsym(RTLD_NEXT, "getaddrinfo");
    if (sink_port() && !is_loopback_name(node)) {
        struct addrinfo h;
        memset(&h, 0, sizeof(h));
        if (hints) h = *hints;
        h.ai_family = AF_INET;
        h.ai_flags &= ~AI_CANONNAME;
        return real_gai("127.0.0.1", service, &h, res);
    }
    return real_gai(node, service, hints, res);
}

int connect(int fd, const struct sockaddr *addr, socklen_t len) {
    static int (*real_connect)(int, const struct sockaddr *, socklen_t);
    if (!real_connect) real_connect = dlsym(RTLD_NEXT, "connect");
    int port = sink_port();
    if (port && addr && addr->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
        uint32_t ip = ntohl(in->sin_addr.s_addr);
        int loop = (ip >> 24) == 127;
        if (!loop || ntohs(in->sin_port) == 443) {   /* 443 never serves a flapi test */
            struct sockaddr_in to = *in;
            to.sin_addr.s_addr = htonl(0x7f000001);
            to.sin_port = htons((uint16_t)port);
            const char *log = getenv("FLAPI_SHIM_LOG");
            if (log) {
                FILE *f = fopen(log, "a");
                if (f) {
                    char buf[INET_ADDRSTRLEN] = "?";
                    inet_ntop(AF_INET, &in->sin_addr, buf, sizeof(buf));
                    fprintf(f, "%s:%d\n", buf, ntohs(in->sin_port));
                    fclose(f);
                }
            }
            return real_connect(fd, (const struct sockaddr *)&to, sizeof(to));
        }
    }
    if (port && addr && addr->sa_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)addr;
        if (!IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr)) {
            errno = ENETUNREACH;
            return -1;
        }
    }
    return real_connect(fd, addr, len);
}
