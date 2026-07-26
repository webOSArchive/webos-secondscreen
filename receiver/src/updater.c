/* App Museum II update check. Convention (webOSArchive/webos-common
 * UpdaterExample): GET getLatestVersionInfo.php?app=<name>/<version>,
 * compare major.minor.build, install the returned downloadURI via
 * Preware through com.palm.applicationManager. Plain HTTP — the device
 * has no usable TLS stack.
 *
 * No sscanf here: %d would pull in __isoc99_sscanf (GLIBC_2.7) and the
 * device glibc is 2.5 (build.sh enforces this). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/time.h>

#include <PDL.h>

#include "updater.h"

#define UPDATE_HOST "appcatalog.webosarchive.org"
#define UPDATE_PORT 80
/* Museum app name is "webOS Second Screen"; spaces must be %-encoded in
 * the raw request line */
#define UPDATE_PATH_FMT "/WebService/getLatestVersionInfo.php?app=webOS%20Second%20Screen/"

#define HTTP_BUF_SIZE 8192
#define VERSION_SIZE  32
#define NOTE_SIZE     512
#define URI_SIZE      512

#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif

/* written by the check thread, then a barrier, then s_update_available —
 * the main thread polls the flag and reads the strings after it */
static volatile int s_update_available;
static volatile int s_update_dismissed;
static volatile int s_check_started;

static char s_new_version[VERSION_SIZE];
static char s_download_uri[URI_SIZE];

static pthread_t s_thread;

/* HTTP/1.0 GET, Connection: close (no chunked encoding to deal with).
 * Returns body length in buf, or -1. */
static int http_get(const char *host, int port, const char *path,
                    char *buf, int buf_size)
{
    struct addrinfo hints, *res = NULL, *ai;
    struct timeval tv;
    char portstr[16], req[512], raw[HTTP_BUF_SIZE];
    int fd = -1, raw_len = 0, req_len, sent = 0, n;
    char *body;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof portstr, "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0)
        return -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        tv.tv_sec = 10; tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return -1;

    req_len = snprintf(req, sizeof req,
                       "GET %s HTTP/1.0\r\n"
                       "Host: %s\r\n"
                       "Connection: close\r\n"
                       "\r\n", path, host);
    while (sent < req_len) {
        n = send(fd, req + sent, req_len - sent, 0);
        if (n <= 0) { close(fd); return -1; }
        sent += n;
    }

    while (raw_len < (int)sizeof raw - 1) {
        n = recv(fd, raw + raw_len, sizeof raw - 1 - raw_len, 0);
        if (n <= 0) break;
        raw_len += n;
    }
    close(fd);
    raw[raw_len] = 0;
    if (raw_len == 0 || strncmp(raw, "HTTP/", 5) != 0)
        return -1;

    body = strstr(raw, "\r\n\r\n");
    if (!body) return -1;
    body += 4;
    n = raw_len - (int)(body - raw);
    if (n <= 0) return -1;
    if (n >= buf_size) n = buf_size - 1;
    memcpy(buf, body, n);
    buf[n] = 0;
    return n;
}

/* copy the value of "key":"value", unescaping \n \t \" \\ \/ */
static int json_get_string(const char *json, const char *key,
                           char *out, int out_size)
{
    char pattern[64];
    const char *p;
    int len = 0;

    snprintf(pattern, sizeof pattern, "\"%s\"", key);
    p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;

    while (*p && *p != '"' && len < out_size - 1) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n':  out[len++] = '\n'; break;
            case 't':  out[len++] = '\t'; break;
            case 'r':  break;
            default:   out[len++] = *p; break;
            }
        } else {
            out[len++] = *p;
        }
        p++;
    }
    out[len] = 0;
    return 1;
}

static int parse_version(const char *s, long v[3])
{
    char *end;
    int i;
    for (i = 0; i < 3; i++) {
        v[i] = strtol(s, &end, 10);
        if (end == s) return 0;
        if (i < 2) {
            if (*end != '.') return 0;
            s = end + 1;
        }
    }
    return 1;
}

static int is_version_newer(const char *local, const char *remote)
{
    long l[3], r[3];
    int i;
    if (!parse_version(local, l) || !parse_version(remote, r))
        return 0;
    for (i = 0; i < 3; i++) {
        if (r[i] > l[i]) return 1;
        if (r[i] < l[i]) return 0;
    }
    return 0;
}

static void *check_thread(void *arg)
{
    char buf[HTTP_BUF_SIZE];
    char path[256], version[VERSION_SIZE], note[NOTE_SIZE], uri[URI_SIZE];
    (void)arg;

    snprintf(path, sizeof path, "%s%s", UPDATE_PATH_FMT, APP_VERSION);
    if (http_get(UPDATE_HOST, UPDATE_PORT, path, buf, sizeof buf) <= 0) {
        fprintf(stderr, "updater: check failed (offline?)\n");
        return NULL;
    }
    if (!json_get_string(buf, "version", version, sizeof version)) {
        fprintf(stderr, "updater: no version in museum response\n");
        return NULL;
    }
    if (!is_version_newer(APP_VERSION, version)) {
        fprintf(stderr, "updater: up to date (local %s, museum %s)\n",
                APP_VERSION, version);
        return NULL;
    }
    if (!json_get_string(buf, "downloadURI", uri, sizeof uri) || !uri[0]) {
        fprintf(stderr, "updater: update %s found but no downloadURI\n", version);
        return NULL;
    }
    if (json_get_string(buf, "versionNote", note, sizeof note) && note[0])
        fprintf(stderr, "updater: %s notes: %s\n", version, note);

    strncpy(s_new_version, version, VERSION_SIZE - 1);
    strncpy(s_download_uri, uri, URI_SIZE - 1);
    fprintf(stderr, "updater: update available %s -> %s (%s)\n",
            APP_VERSION, version, uri);
    __sync_synchronize();
    s_update_available = 1;
    return NULL;
}

void updater_check_start(void)
{
    if (s_check_started) return;
    s_check_started = 1;
    /* detached: quitting must not wait out the 10s socket timeouts */
    if (pthread_create(&s_thread, NULL, check_thread, NULL) == 0)
        pthread_detach(s_thread);
    else
        s_check_started = 0;
}

int updater_has_update(void)
{
    return s_update_available && !s_update_dismissed;
}

const char *updater_get_version(void)
{
    return s_new_version;
}

int updater_install(void)
{
    char params[URI_SIZE + 128];

    if (!s_update_available || !s_download_uri[0]) return 0;
    snprintf(params, sizeof params,
             "{\"id\":\"org.webosinternals.preware\","
             "\"params\":{\"type\":\"install\",\"file\":\"%s\"}}",
             s_download_uri);
    fprintf(stderr, "updater: handing off to Preware: %s\n", s_download_uri);
    PDL_ServiceCall("palm://com.palm.applicationManager/open", params);
    s_update_dismissed = 1;
    /* card away + a beat for webOS to process the launch before we exit */
    PDL_Minimize();
    usleep(1500000);
    return 1;
}

void updater_dismiss(void)
{
    s_update_dismissed = 1;
}
