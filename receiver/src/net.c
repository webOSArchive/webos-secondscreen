/* Network client: connects to the capture server, receives framed JPEG
 * frames (latest-frame-wins), sends touch/key events back. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>

#include "net.h"
#include "discover.h"

#define MAX_PAYLOAD (8u * 1024u * 1024u)
#define RETRY_MS 2000

/* A stale conf often points somewhere unroutable, where a blocking
 * connect() burns ~130s of SYN retries before giving up — long enough to
 * stall discovery for minutes in exactly the case it exists for. */
#define DIAL_TIMEOUT_MS 4000

/* Sweep the subnet once the configured target has clearly gone stale,
 * then only occasionally: a sweep is a SYN per host and ~2s of work, and
 * the conf-file self-heal may still be about to hand us the right one. */
#define DISCOVER_AFTER_FAILS 2
#define DISCOVER_EVERY_FAILS 5

/* The server sends something at least every 3s ('P' ping when the screen
 * is static). A sleeping Mac leaves the TCP link half-open — no FIN, no
 * RST, recv blocks forever — so silence is the only dead-peer signal. */
#define RECV_TIMEOUT_S 10

/* dial target — s_target_mx guarded: the main thread may retarget it
 * (config self-heal) while the net thread is between dial attempts */
static pthread_mutex_t s_target_mx = PTHREAD_MUTEX_INITIALIZER;
static char s_host[128];
static int  s_port;

static volatile int s_discover = 1;
static pthread_mutex_t s_disc_mx = PTHREAD_MUTEX_INITIALIZER;
static char s_disc_host[128];
static int  s_disc_pending;

static volatile int s_run;
static volatile int s_connected;
static volatile int s_fd = -1;
static pthread_t s_thread;

static pthread_mutex_t s_frame_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_send_mx  = PTHREAD_MUTEX_INITIALIZER;

/* when the last complete message landed — see net_rx_age_ms() */
static pthread_mutex_t s_rx_mx = PTHREAD_MUTEX_INITIALIZER;
static struct timeval s_last_rx;
static int s_have_rx;

static void mark_rx(void)
{
    pthread_mutex_lock(&s_rx_mx);
    gettimeofday(&s_last_rx, NULL);
    s_have_rx = 1;
    pthread_mutex_unlock(&s_rx_mx);
}

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
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                fprintf(stderr, "net: no traffic for %ds, assuming dead link\n",
                        RECV_TIMEOUT_S);
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

static void tune_socket(int fd)
{
    int one = 1;
    struct timeval tv;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    tv.tv_sec = RECV_TIMEOUT_S; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

/* connect() with a bounded wait, leaving the socket blocking on success */
static int connect_timeout(int fd, const struct sockaddr *sa, socklen_t salen,
                           int ms)
{
    struct pollfd pf;
    int fl, err = 0, r;
    socklen_t elen = sizeof err;

    fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return -1;

    r = connect(fd, sa, salen);
    if (r < 0) {
        if (errno != EINPROGRESS) return -1;
        pf.fd = fd; pf.events = POLLOUT; pf.revents = 0;
        do { r = poll(&pf, 1, ms); } while (r < 0 && errno == EINTR);
        if (r <= 0) return -1;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0)
            return -1;
    }
    return fcntl(fd, F_SETFL, fl) < 0 ? -1 : 0;
}

static int dial(const char *host, int port)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[16];
    int fd = -1;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof portstr, "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0)
        return -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect_timeout(fd, ai->ai_addr, ai->ai_addrlen,
                            DIAL_TIMEOUT_MS) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) tune_socket(fd);
    return fd;
}

static int should_sweep(int fails)
{
    if (!s_discover || fails < DISCOVER_AFTER_FAILS) return 0;
    return (fails - DISCOVER_AFTER_FAILS) % DISCOVER_EVERY_FAILS == 0;
}

static void *net_thread(void *arg)
{
    uint8_t *scratch = NULL;
    size_t scratch_cap = 0;
    int fails = 0;
    (void)arg;

    while (s_run) {
        char host[128];
        int port, fd;

        pthread_mutex_lock(&s_target_mx);
        memcpy(host, s_host, sizeof host);
        port = s_port;
        pthread_mutex_unlock(&s_target_mx);

        fd = dial(host, port);
        if (fd < 0) {
            fails++;
            fprintf(stderr, "net: connect %s:%d failed, retrying\n", host, port);
            if (should_sweep(fails)) {
                char found[128];
                fd = discover_sweep(port, found, sizeof found);
                if (fd >= 0) {
                    /* Adopt the socket the sweep validated: the sender began
                     * capturing when it accepted, so redialling would cost it
                     * a stop/start for nothing. */
                    tune_socket(fd);
                    net_set_target(found, port);
                    memcpy(host, found, sizeof host);
                    pthread_mutex_lock(&s_disc_mx);
                    memcpy(s_disc_host, found, sizeof s_disc_host);
                    s_disc_pending = 1;
                    pthread_mutex_unlock(&s_disc_mx);
                }
            }
        }
        if (fd < 0) {
            usleep(RETRY_MS * 1000);
            continue;
        }
        fails = 0;
        fprintf(stderr, "net: connected to %s:%d\n", host, port);
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

            /* whatever it was, something arrived: the sender is awake */
            mark_rx();

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

void net_set_target(const char *host, int port)
{
    pthread_mutex_lock(&s_target_mx);
    strncpy(s_host, host, sizeof s_host - 1);
    s_host[sizeof s_host - 1] = 0;
    s_port = port;
    pthread_mutex_unlock(&s_target_mx);
}

void net_set_discovery(int enabled)
{
    s_discover = enabled;
}

int net_take_discovered(char *host, size_t hostlen)
{
    int got;
    pthread_mutex_lock(&s_disc_mx);
    got = s_disc_pending;
    if (got) {
        strncpy(host, s_disc_host, hostlen - 1);
        host[hostlen - 1] = 0;
        s_disc_pending = 0;
    }
    pthread_mutex_unlock(&s_disc_mx);
    return got;
}

void net_start(const char *host, int port)
{
    net_set_target(host, port);
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

uint32_t net_rx_age_ms(void)
{
    struct timeval last, now;
    long sec, ms;
    int have;

    pthread_mutex_lock(&s_rx_mx);
    have = s_have_rx;
    last = s_last_rx;
    pthread_mutex_unlock(&s_rx_mx);
    if (!have) return NET_RX_NEVER;

    gettimeofday(&now, NULL);
    sec = now.tv_sec - last.tv_sec;
    if (sec < 0) return 0;                  /* clock stepped backwards */
    if (sec > 1000000L) return NET_RX_NEVER - 1;   /* ~11 days: don't overflow */
    ms = sec * 1000L + (now.tv_usec - last.tv_usec) / 1000L;
    return ms > 0 ? (uint32_t)ms : 0;
}

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
