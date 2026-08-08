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

/* Dial-retry backoff. A flat 2s retry meant one idle hour opposite a
 * sleeping Mac cost ~900 dials; doubling the gap keeps a brief outage
 * cheap to ride out without hammering the network for the rest of it. */
#define RETRY_MS_MIN  2000u
#define RETRY_MS_MAX 60000u

/* A host that accepts and then never speaks is a sleeping Mac at an
 * address we already know is right (PROTOCOL.md liveness: a live sender
 * owes us a frame or a 'P' every 3s). There is nothing to discover, so
 * wait quietly for it to wake rather than redialling every 2s. */
#define SILENT_RETRY_MS 30000u

/* A stale conf often points somewhere unroutable, where a blocking
 * connect() burns ~130s of SYN retries before giving up — long enough to
 * stall discovery for minutes in exactly the case it exists for. */
#define DIAL_TIMEOUT_MS 4000

/* Start searching after the first failed dial. A dial to a dead address
 * already costs DIAL_TIMEOUT_MS to fail, so this is several seconds of
 * evidence rather than a hair trigger — and waiting for a second failure
 * pushed the first sweep out to ~14s, which cost the fast phase a third
 * of its budget in the minute it matters most. */
#define DISCOVER_AFTER_FAILS 1

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

/* Sweep suppression, written by the main thread. s_sweep_epoch is bumped
 * when the screensaver is dismissed: the net thread notices, refills the
 * sweep budget and cuts short whatever backoff it was sitting in, so a
 * touch retries at once instead of waiting out a 60s gap. Single writer,
 * and a stale read only costs one retry cycle. */
static volatile int s_sweep_paused;
static volatile unsigned s_sweep_epoch;

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

/* Wall clock in ms, truncated to 32 bits. Only ever used for
 * differences, which stay correct across the ~49-day wrap. */
static uint32_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)((uint32_t)tv.tv_sec * 1000u + (uint32_t)(tv.tv_usec / 1000));
}

/* How long to leave between sweeps, as a function of how long this
 * disconnection has lasted. Front-loaded, because a sweep only pays off
 * when the Mac is coming up right about now; decaying afterwards,
 * because a sweep SYNs all 254 addresses on the /24 and a router running
 * IDS reads a sustained stream of those as a port scan — which is what
 * got this device blocked off the network (198 sweeps in one idle hour,
 * ~50k SYNs, measured 2026-08-08).
 *
 * The fast phase runs a full four minutes rather than one. A sleeping
 * Mac that still accepts out of its listen backlog holds us in the
 * silent-retry path for minutes at a stretch without sweeping at all
 * (measured: two minutes of it, which under a one-minute fast phase left
 * the search already decayed to 30s by the time the address actually
 * went dead). Four minutes means the head of the schedule survives that.
 *
 * 15s and not 10s for that head: a four-minute run at 6 sweeps/min is a
 * higher instantaneous rate than the 198-per-hour (3.3/min) that got the
 * device blocked in the first place. Total volume is not the only thing
 * an IDS scores — sustained rate is — so the fast phase stays below the
 * rate we already know was noticed.
 *
 * The tail is what matters for the IDS: 1/min for as long as anyone is
 * watching, and the screensaver stops sweeping altogether. Dismissing it
 * restarts this clock, so someone who has just walked back to the device
 * gets the fast phase again. */
static unsigned sweep_interval_ms(uint32_t elapsed)
{
    if (elapsed < 240000u) return 15000u;   /* 4/min, minutes 1-4 */
    if (elapsed < 420000u) return 30000u;   /* 2/min, minutes 5-7 */
    return 60000u;                          /* 1/min from 8 on    */
}

static int should_sweep(int fails, uint32_t now, uint32_t ep_start,
                        uint32_t last_sweep, int swept)
{
    if (!s_discover || s_sweep_paused) return 0;
    if (fails < DISCOVER_AFTER_FAILS) return 0;
    if (!swept) return 1;                   /* first of the episode */
    return now - last_sweep >= sweep_interval_ms(now - ep_start);
}

static unsigned backoff_ms(int fails)
{
    unsigned ms = RETRY_MS_MIN;
    int i;
    for (i = 1; i < fails && ms < RETRY_MS_MAX; i++) ms <<= 1;
    return ms > RETRY_MS_MAX ? RETRY_MS_MAX : ms;
}

/* How long to sleep before the next attempt. Sweeps only ever happen
 * after a failed dial, so the dial backoff sets the ceiling on how often
 * one can fire — without this the 60s backoff would starve the "six in
 * the first minute" phase down to one. Redialling a single known address
 * every 10s costs nothing; it is the /24 sweep that has to be rationed. */
static unsigned next_wait_ms(int fails, uint32_t now, uint32_t ep_start,
                             uint32_t last_sweep, int swept, uint32_t dial_ms)
{
    unsigned wait = backoff_ms(fails);

    if (s_discover && !s_sweep_paused && swept) {
        unsigned iv = sweep_interval_ms(now - ep_start);
        unsigned since = now - last_sweep;
        unsigned due = since >= iv ? 0 : iv - since;
        /* A sweep can only fire after the next dial has already failed,
         * so wake early enough to absorb that dial — otherwise every
         * period silently grows by DIAL_TIMEOUT_MS and the fast phase
         * delivers four sweeps a minute instead of six. */
        due = due > dial_ms ? due - dial_ms : 0;
        if (due < wait) wait = due;
    }
    /* never spin: the dial itself can return instantly on a RST */
    return wait < 500u ? 500u : wait;
}

/* Wait between dials, in slices: a screensaver touch (which bumps the
 * epoch) must not have to sit out a 60s backoff, and net_stop() should
 * not either. */
static void retry_wait(unsigned ms, unsigned epoch)
{
    while (ms > 0 && s_run && s_sweep_epoch == epoch) {
        unsigned slice = ms > 200u ? 200u : ms;
        usleep(slice * 1000u);
        ms -= slice;
    }
}

static void *net_thread(void *arg)
{
    uint8_t *scratch = NULL;
    size_t scratch_cap = 0;
    int fails = 0, swept = 0;
    uint32_t ep_start = now_ms(), last_sweep = 0;
    unsigned epoch = s_sweep_epoch;
    (void)arg;

    while (s_run) {
        char host[128];
        int port, fd, carried = 0;
        uint32_t now, dial_start, dial_ms;

        if (s_sweep_epoch != epoch) {
            /* dismissed the screensaver: somebody is back at the device,
             * so restart the episode and run the fast phase again */
            epoch = s_sweep_epoch;
            fails = 0;
            swept = 0;
            ep_start = now_ms();
        }

        pthread_mutex_lock(&s_target_mx);
        memcpy(host, s_host, sizeof host);
        port = s_port;
        pthread_mutex_unlock(&s_target_mx);

        dial_start = now_ms();
        fd = dial(host, port);
        now = now_ms();
        dial_ms = now - dial_start;
        if (fd < 0) {
            fails++;
            fprintf(stderr, "net: connect %s:%d failed, retrying\n", host, port);
            if (should_sweep(fails, now, ep_start, last_sweep, swept)) {
                char found[128];
                last_sweep = now;
                swept = 1;
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
            retry_wait(next_wait_ms(fails, now_ms(), ep_start, last_sweep,
                                    swept, dial_ms), epoch);
            continue;
        }
        /* Note: no fails/sweeps reset here. A sleeping Mac still completes
         * the handshake out of its listen backlog, and treating that as
         * success re-armed the sweep schedule every few seconds — which is
         * how one idle hour turned into 198 subnet scans. Only traffic
         * ends the episode; see the carried check below. */
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
            carried = 1;

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

        if (carried) {
            /* a link that really carried frames: the address is good and
             * the sender was genuinely there, so the next outage starts
             * from a clean slate — quick retry, fast sweep phase again */
            fails = 0;
            swept = 0;
            ep_start = now_ms();
            fprintf(stderr, "net: disconnected\n");
            if (s_run) retry_wait(RETRY_MS_MIN, epoch);
        } else {
            /* accepted us and said nothing at all: the address is right,
             * the Mac is asleep. Sweeping the subnet would only find the
             * same silent host, so sit out a long wait instead. */
            fails++;
            fprintf(stderr, "net: %s:%d accepted but never spoke, waiting %us\n",
                    host, port, SILENT_RETRY_MS / 1000u);
            if (s_run) retry_wait(SILENT_RETRY_MS, epoch);
        }
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

void net_set_sweep_paused(int paused)
{
    if (s_sweep_paused && !paused) s_sweep_epoch++;
    s_sweep_paused = paused;
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
