/* Sender discovery: find the Mac by sweeping our own /24 for its listener.
 *
 * Why a sweep and not mDNS or a Mac-side broadcast beacon: webOS only
 * admits inbound packets that conntrack sees as RELATED,ESTABLISHED —
 * that is why the device dials the Mac in the first place (PROTOCOL.md).
 * An unsolicited beacon never reaches us. Neither does an mDNS response:
 * conntrack keys the reply tuple on the *multicast* destination we asked
 * from, so the Mac's unicast answer doesn't match the entry and is
 * dropped. (Desktop distros paper over this with an explicit
 * "--dport 5353 -j ACCEPT" rule; webOS has no such rule.) Outbound
 * unicast TCP is the only direction that reliably works, so we knock on
 * every door on the subnet instead of listening for a knock.
 *
 * Connects are non-blocking and batched, so the whole /24 costs ~2s.
 * Anything that accepts then has to prove it is a sender — see PROTOCOL.md
 * ('Q' probe / 'Y' reply), with a fallback for pre-0.2.4 senders that
 * predate the probe and can only be recognised by their frame traffic.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "net.h"
#include "discover.h"

#define BATCH               64      /* concurrent half-open connects */
#define CONNECT_TIMEOUT_MS  500     /* generous for a busy 802.11g link */

/* A legacy sender only reveals itself by sending: a 'J' frame right away
 * if the screen is busy, else a 'P' keepalive after its 3s mailbox
 * timeout. 4s covers that, and a current sender's 'Y' lands in
 * milliseconds — behind capture startup at worst. */
#define HANDSHAKE_MS        4000

/* A service squatting on the port that simply never speaks costs a full
 * HANDSHAKE_MS to reject, so cap how many we interrogate per sweep. The
 * sweep start rotates (s_start) so squatters at low addresses can't hold
 * the cap against the real sender forever. */
#define MAX_PROBES          4

#define PROBE_MAGIC  "SSCR"    /* receiver -> sender, 'Q' payload */
#define REPLY_MAGIC  "SSND"    /* sender -> receiver, 'Y' payload */
#define MAGIC_LEN    4

#define MAX_PAYLOAD  (8u * 1024u * 1024u)

static long ms_left(const struct timeval *deadline)
{
    struct timeval now;
    long ms;
    gettimeofday(&now, NULL);
    ms = (deadline->tv_sec - now.tv_sec) * 1000L +
         (deadline->tv_usec - now.tv_usec) / 1000L;
    return ms > 0 ? ms : 0;
}

static void deadline_in(struct timeval *dl, long ms)
{
    gettimeofday(dl, NULL);
    dl->tv_sec += ms / 1000;
    dl->tv_usec += (ms % 1000) * 1000;
    if (dl->tv_usec >= 1000000) { dl->tv_sec++; dl->tv_usec -= 1000000; }
}

static void set_blocking(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
}

/* First non-loopback IPv4 interface that is up. SIOCGIFCONF rather than
 * getifaddrs(): the device glibc is 2.5-era and build.sh rejects anything
 * that drags in newer versioned symbols. */
static int local_ipv4(uint32_t *addr_h, uint32_t *mask_h)
{
    char buf[4096];
    struct ifconf ifc;
    struct ifreq *r;
    int fd, i, n, found = 0;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    memset(&ifc, 0, sizeof ifc);
    ifc.ifc_len = sizeof buf;
    ifc.ifc_buf = buf;
    if (ioctl(fd, SIOCGIFCONF, &ifc) < 0) { close(fd); return -1; }

    r = ifc.ifc_req;
    n = ifc.ifc_len / (int)sizeof(struct ifreq);
    for (i = 0; i < n && !found; i++) {
        struct ifreq q;
        uint32_t a;

        if (r[i].ifr_addr.sa_family != AF_INET) continue;
        /* stash the address before the next ioctl overwrites the union */
        a = ((struct sockaddr_in *)&r[i].ifr_addr)->sin_addr.s_addr;

        memset(&q, 0, sizeof q);
        strncpy(q.ifr_name, r[i].ifr_name, IFNAMSIZ - 1);
        if (ioctl(fd, SIOCGIFFLAGS, &q) < 0) continue;
        if (!(q.ifr_flags & IFF_UP) || (q.ifr_flags & IFF_LOOPBACK)) continue;

        memset(&q, 0, sizeof q);
        strncpy(q.ifr_name, r[i].ifr_name, IFNAMSIZ - 1);
        if (ioctl(fd, SIOCGIFNETMASK, &q) < 0) continue;

        *addr_h = ntohl(a);
        *mask_h = ntohl(((struct sockaddr_in *)&q.ifr_addr)->sin_addr.s_addr);
        found = 1;
    }
    close(fd);
    return found ? 0 : -1;
}

/* Kick off a non-blocking connect. Returns the fd (connecting or already
 * connected) or -1. */
static int start_connect(uint32_t ip_h, int port)
{
    struct sockaddr_in sa;
    int fd, fl;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) { close(fd); return -1; }

    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(ip_h);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0 &&
        errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    return fd;
}

static int recv_deadline(int fd, void *p, size_t n, const struct timeval *dl)
{
    uint8_t *b = p;
    while (n > 0) {
        struct timeval tv;
        long ms = ms_left(dl);
        ssize_t got;
        if (ms <= 0) return -1;
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        got = recv(fd, b, n, 0);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return -1;
        b += got;
        n -= (size_t)got;
    }
    return 0;
}

/* Consume and discard a payload we don't care about, keeping the stream
 * framed for whoever adopts the socket next. */
static int skip_deadline(int fd, uint32_t n, const struct timeval *dl)
{
    uint8_t sink[4096];
    while (n > 0) {
        uint32_t chunk = n > sizeof sink ? (uint32_t)sizeof sink : n;
        if (recv_deadline(fd, sink, chunk, dl) < 0) return -1;
        n -= chunk;
    }
    return 0;
}

static int send_probe(int fd)
{
    uint8_t msg[5 + MAGIC_LEN + 1];
    msg[0] = 'Q';
    msg[1] = 0; msg[2] = 0; msg[3] = 0; msg[4] = MAGIC_LEN + 1;
    memcpy(msg + 5, PROBE_MAGIC, MAGIC_LEN);
    msg[5 + MAGIC_LEN] = PROTO_VERSION;
    return send(fd, msg, sizeof msg, MSG_NOSIGNAL) == (ssize_t)sizeof msg ? 0 : -1;
}

/* Ask "are you a second-screen sender?" and wait for proof.
 * Returns 1 on a 'Y' reply, 2 when only frame traffic identified it
 * (pre-0.2.4 sender), -1 otherwise. */
static int handshake(int fd, const char *ip)
{
    struct timeval dl;
    uint8_t reply[128];

    set_blocking(fd);
    deadline_in(&dl, HANDSHAKE_MS);
    if (send_probe(fd) < 0) return -1;

    while (ms_left(&dl) > 0) {
        uint8_t hdr[5];
        uint32_t len;

        if (recv_deadline(fd, hdr, 5, &dl) < 0) return -1;
        len = ((uint32_t)hdr[1] << 24) | ((uint32_t)hdr[2] << 16) |
              ((uint32_t)hdr[3] << 8) | hdr[4];
        if (len > MAX_PAYLOAD) return -1;   /* not our framing at all */

        if (hdr[0] == 'Y') {
            uint32_t take = len > sizeof reply ? (uint32_t)sizeof reply : len;
            if (recv_deadline(fd, reply, take, &dl) < 0) return -1;
            if (skip_deadline(fd, len - take, &dl) < 0) return -1;
            if (take < MAGIC_LEN + 1 ||
                memcmp(reply, REPLY_MAGIC, MAGIC_LEN) != 0) {
                fprintf(stderr, "discover: %s answered with bad magic\n", ip);
                return -1;
            }
            if (take >= MAGIC_LEN + 2) {
                uint32_t nlen = reply[MAGIC_LEN + 1];
                if (nlen > take - (MAGIC_LEN + 2)) nlen = take - (MAGIC_LEN + 2);
                fprintf(stderr, "discover: %s is \"%.*s\" (sender protocol v%u)\n",
                        ip, (int)nlen, (char *)reply + MAGIC_LEN + 2,
                        reply[MAGIC_LEN]);
            } else {
                fprintf(stderr, "discover: %s confirmed (sender protocol v%u)\n",
                        ip, reply[MAGIC_LEN]);
            }
            return 1;
        }

        if (hdr[0] == 'J' || hdr[0] == 'P') {
            /* Never answered the probe but is speaking our protocol —
             * a sender from before 'Q' existed. */
            if (skip_deadline(fd, len, &dl) < 0) return -1;
            fprintf(stderr, "discover: %s sent '%c', treating as a pre-0.2.4 sender\n",
                    ip, hdr[0]);
            return 2;
        }

        if (skip_deadline(fd, len, &dl) < 0) return -1;   /* unknown: keep waiting */
    }
    return -1;   /* caller logs the rejection */
}

/* Where the next sweep starts within the range. Rotating means a handful
 * of unresponsive squatters sitting at low addresses can't spend the probe
 * cap every single sweep and hide the real sender indefinitely. */
static unsigned s_start;

int discover_sweep(int port, char *host, size_t hostlen)
{
    uint32_t self, mask, base, span;
    unsigned offset, nhosts;
    int b, probes = 0, prefix;
    struct in_addr shown;

    if (local_ipv4(&self, &mask) < 0) {
        fprintf(stderr, "discover: no usable network interface\n");
        return -1;
    }
    /* Never sweep wider than a /24: a /16 (some 10.x networks hand those
     * out) is far too much ground, and the Mac shares the TouchPad's wire
     * in every setup this app targets. A narrower mask is honoured as-is —
     * addresses outside our own subnet wouldn't route anyway. */
    mask |= 0xffffff00u;
    base = self & mask;
    span = ~mask;                 /* 255 on a /24 */
    if (span < 2) {
        fprintf(stderr, "discover: subnet too small to sweep\n");
        return -1;
    }
    nhosts = span - 1;            /* drop the broadcast address */
    for (prefix = 0; prefix < 32 && !(mask & (1u << prefix)); prefix++) ;
    prefix = 32 - prefix;

    shown.s_addr = htonl(base);
    fprintf(stderr, "discover: sweeping %s/%d (%u hosts) for a sender on port %d\n",
            inet_ntoa(shown), prefix, nhosts, port);

    offset = s_start;
    s_start = (s_start + BATCH) % nhosts;

    for (b = 0; b < (int)nhosts && probes < MAX_PROBES; b += BATCH) {
        int fds[BATCH], done[BATCH];
        uint32_t ips[BATCH];
        struct timeval dl;
        int n = (int)nhosts - b < BATCH ? (int)nhosts - b : BATCH;
        int nf = 0, k, i, pending;

        for (k = 0; k < n; k++) {
            /* host part 1..nhosts, rotated; a batch may wrap the range,
             * which costs nothing since the order is arbitrary */
            uint32_t ip = base | (1u + (((unsigned)(b + k) + offset) % nhosts));
            int fd;
            if (ip == self) continue;
            fd = start_connect(ip, port);
            if (fd < 0) continue;
            fds[nf] = fd; ips[nf] = ip; done[nf] = 0;
            nf++;
        }
        if (nf == 0) continue;

        deadline_in(&dl, CONNECT_TIMEOUT_MS);
        pending = nf;
        while (pending > 0 && ms_left(&dl) > 0) {
            struct pollfd pf[BATCH];
            int map[BATCH], m = 0, r;

            for (i = 0; i < nf; i++) {
                if (done[i]) continue;
                pf[m].fd = fds[i];
                pf[m].events = POLLOUT;
                pf[m].revents = 0;
                map[m] = i;
                m++;
            }
            r = poll(pf, (nfds_t)m, (int)ms_left(&dl));
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;

            for (i = 0; i < m; i++) {
                int idx, err = 0;
                socklen_t elen = sizeof err;
                if (!pf[i].revents) continue;
                idx = map[i];
                done[idx] = 1;
                pending--;
                if (getsockopt(fds[idx], SOL_SOCKET, SO_ERROR, &err, &elen) < 0)
                    err = errno;
                if (err != 0) { close(fds[idx]); fds[idx] = -1; continue; }
                /* left open: interrogated below */
            }
        }

        /* Anything still pending never completed its connect. */
        for (i = 0; i < nf; i++) {
            if (!done[i]) { close(fds[i]); fds[i] = -1; }
        }

        for (i = 0; i < nf; i++) {
            struct in_addr a;
            char ip[INET_ADDRSTRLEN];
            int ok;

            if (fds[i] < 0) continue;
            if (probes >= MAX_PROBES) { close(fds[i]); continue; }

            a.s_addr = htonl(ips[i]);
            strncpy(ip, inet_ntoa(a), sizeof ip - 1);
            ip[sizeof ip - 1] = 0;
            probes++;

            ok = handshake(fds[i], ip);
            if (ok < 0) fprintf(stderr, "discover: %s is not a sender\n", ip);
            if (ok > 0) {
                int j;
                for (j = i + 1; j < nf; j++)
                    if (fds[j] >= 0) close(fds[j]);
                strncpy(host, ip, hostlen - 1);
                host[hostlen - 1] = 0;
                return fds[i];
            }
            close(fds[i]);
            fds[i] = -1;
        }
    }

    if (probes >= MAX_PROBES)
        fprintf(stderr, "discover: gave up after %d candidates\n", probes);
    else
        fprintf(stderr, "discover: no sender found\n");
    return -1;
}
