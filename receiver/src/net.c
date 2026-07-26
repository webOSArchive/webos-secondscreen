/* Network client: connects to the capture server, receives framed JPEG
 * frames (latest-frame-wins), sends touch/key events back. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>

#include "net.h"

#define MAX_PAYLOAD (8u * 1024u * 1024u)
#define RETRY_MS 2000

#define PROTO_VERSION 1

static char s_host[128];
static int  s_port;

static volatile int s_run;
static volatile int s_connected;
static volatile int s_fd = -1;
static pthread_t s_thread;

static pthread_mutex_t s_frame_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_send_mx  = PTHREAD_MUTEX_INITIALIZER;

/* newest complete frame, handed off by pointer swap */
static uint8_t *s_frame;
static size_t   s_frame_len, s_frame_cap;
static uint32_t s_seq, s_taken_seq;

static int read_full(int fd, void *p, size_t n)
{
    uint8_t *b = p;
    while (n > 0) {
        ssize_t r = recv(fd, b, n, 0);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return -1;
        }
        b += r; n -= (size_t)r;
    }
    return 0;
}

static void send_msg(uint8_t type, const uint8_t *payload, uint32_t len)
{
    uint8_t hdr[5];
    int fd;
    hdr[0] = type;
    hdr[1] = (uint8_t)(len >> 24);
    hdr[2] = (uint8_t)(len >> 16);
    hdr[3] = (uint8_t)(len >> 8);
    hdr[4] = (uint8_t)(len);
    pthread_mutex_lock(&s_send_mx);
    fd = s_fd;
    if (fd >= 0) {
        /* touch/key messages are tiny; a short/failed send just means the
         * read side will notice the dead socket and reconnect */
        if (send(fd, hdr, 5, MSG_NOSIGNAL) == 5 && len > 0)
            send(fd, payload, len, MSG_NOSIGNAL);
    }
    pthread_mutex_unlock(&s_send_mx);
}

static void send_hello(void)
{
    uint8_t p[5];
    p[0] = 1024 >> 8; p[1] = 1024 & 0xff;
    p[2] = 768 >> 8;  p[3] = 768 & 0xff;
    p[4] = PROTO_VERSION;
    send_msg('H', p, 5);
}

static int dial(void)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[16];
    int fd = -1;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof portstr, "%d", s_port);
    if (getaddrinfo(s_host, portstr, &hints, &res) != 0)
        return -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
    return fd;
}

static void *net_thread(void *arg)
{
    uint8_t *scratch = NULL;
    size_t scratch_cap = 0;
    (void)arg;

    while (s_run) {
        int fd = dial();
        if (fd < 0) {
            fprintf(stderr, "net: connect %s:%d failed, retrying\n", s_host, s_port);
            usleep(RETRY_MS * 1000);
            continue;
        }
        fprintf(stderr, "net: connected to %s:%d\n", s_host, s_port);
        pthread_mutex_lock(&s_send_mx);
        s_fd = fd;
        pthread_mutex_unlock(&s_send_mx);
        s_connected = 1;
        send_hello();

        while (s_run) {
            uint8_t hdr[5];
            uint32_t len;
            if (read_full(fd, hdr, 5) < 0) break;
            len = ((uint32_t)hdr[1] << 24) | ((uint32_t)hdr[2] << 16) |
                  ((uint32_t)hdr[3] << 8) | hdr[4];
            if (len > MAX_PAYLOAD) {
                fprintf(stderr, "net: bogus payload length %u, dropping link\n", len);
                break;
            }
            if (len > scratch_cap) {
                uint8_t *nb = realloc(scratch, len);
                if (!nb) break;
                scratch = nb; scratch_cap = len;
            }
            if (len > 0 && read_full(fd, scratch, len) < 0) break;

            if (hdr[0] == 'J') {
                uint8_t *tb; size_t tc;
                pthread_mutex_lock(&s_frame_mx);
                tb = s_frame; tc = s_frame_cap;
                s_frame = scratch; s_frame_cap = scratch_cap; s_frame_len = len;
                scratch = tb; scratch_cap = tc;
                s_seq++;
                pthread_mutex_unlock(&s_frame_mx);
            }
            /* 'P' ping and unknown types: ignored */
        }

        s_connected = 0;
        pthread_mutex_lock(&s_send_mx);
        s_fd = -1;
        pthread_mutex_unlock(&s_send_mx);
        close(fd);
        fprintf(stderr, "net: disconnected\n");
        if (s_run) usleep(RETRY_MS * 1000);
    }
    free(scratch);
    return NULL;
}

void net_start(const char *host, int port)
{
    strncpy(s_host, host, sizeof s_host - 1);
    s_host[sizeof s_host - 1] = 0;
    s_port = port;
    s_run = 1;
    pthread_create(&s_thread, NULL, net_thread, NULL);
}

void net_stop(void)
{
    int fd = s_fd;
    s_run = 0;
    if (fd >= 0) shutdown(fd, SHUT_RDWR);
    pthread_join(s_thread, NULL);
}

int net_connected(void) { return s_connected; }

size_t net_take_frame(uint8_t **buf, size_t *cap)
{
    size_t len = 0;
    pthread_mutex_lock(&s_frame_mx);
    if (s_seq != s_taken_seq && s_frame) {
        uint8_t *tb = *buf; size_t tc = *cap;
        *buf = s_frame; *cap = s_frame_cap; len = s_frame_len;
        s_frame = tb; s_frame_cap = tc; s_frame_len = 0;
        s_taken_seq = s_seq;
    }
    pthread_mutex_unlock(&s_frame_mx);
    return len;
}

void net_send_touch(int finger, int action, int x, int y)
{
    uint8_t p[6];
    p[0] = (uint8_t)finger;
    p[1] = (uint8_t)action;
    p[2] = (uint8_t)(x >> 8); p[3] = (uint8_t)x;
    p[4] = (uint8_t)(y >> 8); p[5] = (uint8_t)y;
    send_msg('T', p, 6);
}

void net_send_key(int sym, int down)
{
    uint8_t p[3];
    p[0] = (uint8_t)(sym >> 8); p[1] = (uint8_t)sym;
    p[2] = (uint8_t)(down ? 1 : 0);
    send_msg('K', p, 3);
}
