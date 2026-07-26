/* Second Screen receiver — HP TouchPad (webOS 3.0.5) PDK app.
 *
 * MJPEG-over-TCP client: pulls frames from the capture server, renders
 * fullscreen with SDL 1.2 + OpenGL ES 1.1 (SDL owns the GL context —
 * never raw EGL, which flickers under the 3-layer compositor), and
 * sends touch events back on the same socket. See PROTOCOL.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>

#include <SDL.h>
#include <PDL.h>
#include <GLES/gl.h>

#include "net.h"
#include "decode.h"
#include "updater.h"
#include "waiting_jpg.h"   /* generated from assets/waiting.jpg by build.sh */
#include "update_jpg.h"    /* generated from assets/update.jpg by build.sh */

/* Update-prompt button hit rects — must match assets/make-update-jpg.py */
#define BTN_Y0   596
#define BTN_Y1   660
#define UPD_X0   222
#define UPD_X1   482
#define LATER_X0 542
#define LATER_X1 802

#define APP_ID    "org.webosarchive.secondscreen"
#define LOG_PATH  "/media/internal/" APP_ID ".log"
#define CONF_PATH "/media/internal/secondscreen.conf"

#define SCREEN_W 1024
#define SCREEN_H 768
#define TEX_W 1024
#define TEX_H 1024
#define DEFAULT_HOST "192.168.10.45"
#define DEFAULT_PORT 5959

/* re-arm the power activity well before the previous one lapses */
#define WAKE_INTERVAL_MS 240000
#define WAKE_PAYLOAD "{\"id\":\"" APP_ID ".stream\",\"duration_ms\":300000}"

/* Disconnected housekeeping: exit after an hour with no connection (the
 * wake activity would otherwise keep the display on all night if the Mac
 * sleeps), and re-read the config file every minute in case the Mac came
 * back with a new IP (its sender rewrites the conf on launch). */
#define IDLE_EXIT_MS  (60u * 60u * 1000u)
#define CONF_POLL_MS  60000u

static GLuint s_tex;
static GLfloat s_verts[8] = { 0, 0, SCREEN_W, 0, 0, SCREEN_H, SCREEN_W, SCREEN_H };
static GLfloat s_texco[8];

/* Launcher-launched PDK apps get no terminal and no /var/log/messages —
 * a log file on /media/internal is the only way to see anything. */
static void log_redirect(void)
{
    FILE *lf = fopen(LOG_PATH, "w");
    if (lf) {
        dup2(fileno(lf), 1);
        dup2(fileno(lf), 2);
        fclose(lf);
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}

static void parse_config(char *host, size_t hostlen, int *port)
{
    char line[256];
    FILE *f = fopen(CONF_PATH, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (strncmp(line, "host=", 5) == 0 && line[5]) {
            strncpy(host, line + 5, hostlen - 1);
            host[hostlen - 1] = 0;
        } else if (strncmp(line, "port=", 5) == 0) {
            int p = atoi(line + 5);
            if (p > 0) *port = p;
        }
    }
    fclose(f);
}

/* The launcher passes params as raw JSON in argv ({"host":"x","port":n},
 * ["host:port"], or often just "{}"); a novacom shell passes a bare
 * host[:port]. Parse all of these defensively. */
static void parse_arg(const char *arg, char *host, size_t hostlen, int *port)
{
    char tmp[160];
    const char *p, *e;
    char *colon;

    if (!arg || !*arg) return;

    p = strstr(arg, "\"host\"");
    if (p) {
        p = strchr(p + 6, ':');
        if (!p) return;
        p = strchr(p, '"');
        if (!p) return;
        e = strchr(++p, '"');
        if (!e || (size_t)(e - p) >= hostlen) return;
        memcpy(host, p, e - p);
        host[e - p] = 0;
        p = strstr(arg, "\"port\"");
        if (p) {
            p = strchr(p + 6, ':');
            if (p) {
                int v = atoi(p + 1);
                if (v > 0) *port = v;
            }
        }
        return;
    }
    if (*arg == '{' || *arg == '[') {
        /* JSON without a host key: use its first string, if any */
        p = strchr(arg, '"');
        if (!p) return;
        e = strchr(++p, '"');
        if (!e || e == p || (size_t)(e - p) >= sizeof tmp) return;
        memcpy(tmp, p, e - p);
        tmp[e - p] = 0;
        arg = tmp;
    }
    strncpy(host, arg, hostlen - 1);
    host[hostlen - 1] = 0;
    colon = strchr(host, ':');
    if (colon) {
        *colon = 0;
        if (atoi(colon + 1) > 0) *port = atoi(colon + 1);
    }
}

static void gl_setup(void)
{
    glViewport(0, 0, SCREEN_W, SCREEN_H);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(0, SCREEN_W, SCREEN_H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glColor4f(1, 1, 1, 1);

    glGenTextures(1, &s_tex);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    /* GLES 1.1 requires power-of-two textures: allocate 1024x1024,
     * upload frames as a sub-image and window the texcoords */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, TEX_W, TEX_H, 0,
                 GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, s_verts);
    glTexCoordPointer(2, GL_FLOAT, 0, s_texco);
}

static uint8_t *s_rgb;   /* decode target, SCREEN_W*SCREEN_H*2 */

static void set_frame_size(int w, int h);

/* Decode an embedded screen (waiting / update prompt) onto the texture —
 * same path as a stream frame, so no text/UI code needed. */
static void show_screen(const unsigned char *jpg, unsigned int len)
{
    int w, h;
    if (decode_jpeg(jpg, len, s_rgb, SCREEN_W, SCREEN_H, &w, &h)) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                        GL_RGB, GL_UNSIGNED_SHORT_5_6_5, s_rgb);
        set_frame_size(w, h);
    }
}

static void show_waiting(void) { show_screen(waiting_jpg, waiting_jpg_len); }

static void set_frame_size(int w, int h)
{
    GLfloat u = (GLfloat)w / TEX_W;
    GLfloat v = (GLfloat)h / TEX_H;
    s_texco[0] = 0; s_texco[1] = 0;
    s_texco[2] = u; s_texco[3] = 0;
    s_texco[4] = 0; s_texco[5] = v;
    s_texco[6] = u; s_texco[7] = v;
}

int main(int argc, char *argv[])
{
    char host[128] = DEFAULT_HOST;
    int port = DEFAULT_PORT;
    SDL_Surface *screen;
    uint8_t *jpg = NULL;
    size_t jpg_cap = 0;
    int have_frame = 0, running = 1, prompt = 0, arg_override = 0;
    Uint32 next_wake = 0, last_draw = 0, fps_t0;
    Uint32 last_conn, last_conf_poll;
    int fps_frames = 0;
    Uint32 stat_decode = 0, stat_upload = 0, stat_swap = 0;

    log_redirect();
    signal(SIGPIPE, SIG_IGN);
    fprintf(stderr, "secondscreen receiver starting\n");

    parse_config(host, sizeof host, &port);
    {
        /* an explicit argv target (dev workflow) wins for the whole run:
         * it disables the config self-heal poll below */
        char pre_host[128];
        int pre_port = port;
        memcpy(pre_host, host, sizeof pre_host);
        if (argc > 1) parse_arg(argv[1], host, sizeof host, &port);
        arg_override = strcmp(pre_host, host) != 0 || pre_port != port;
    }
    fprintf(stderr, "server: %s:%d%s\n", host, port,
            arg_override ? " (from argv; config polling off)" : "");

    s_rgb = malloc(SCREEN_W * SCREEN_H * 2);
    if (!s_rgb) return 1;

    /* PDL before SDL — GPU access and system integration depend on it */
    PDL_Init(0);
    PDL_SetTouchAggression(PDL_AGGRESSION_MORETOUCHES);
    PDL_GesturesEnable(PDL_FALSE);   /* edge touches reach the app */
    PDL_CustomPauseUiEnable(PDL_TRUE);

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    /* webOS SDL: the GLES context version must be requested explicitly,
     * and 0,0,0 lets the compositor pick the native mode (1024x768) */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    screen = SDL_SetVideoMode(0, 0, 0, SDL_OPENGL);
    if (!screen) {
        fprintf(stderr, "SDL_SetVideoMode: %s\n", SDL_GetError());
        return 1;
    }
    fprintf(stderr, "video mode: %dx%d\n", screen->w, screen->h);
    SDL_ShowCursor(SDL_DISABLE);
    gl_setup();
    show_waiting();

    net_start(host, port);
    updater_check_start();   /* App Museum II version check, background */
    fps_t0 = SDL_GetTicks();
    last_conn = last_conf_poll = fps_t0;

    while (running) {
        SDL_Event ev;
        size_t len;
        int dirty = 0;
        Uint32 now;

        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_MOUSEBUTTONDOWN:
                if (prompt) break;   /* prompt swallows touches */
                net_send_touch(ev.button.which, 0, ev.button.x, ev.button.y);
                break;
            case SDL_MOUSEMOTION:
                if (prompt) break;
                net_send_touch(ev.motion.which, 1, ev.motion.x, ev.motion.y);
                break;
            case SDL_MOUSEBUTTONUP:
                if (prompt) {
                    int x = ev.button.x, y = ev.button.y;
                    if (y >= BTN_Y0 && y <= BTN_Y1) {
                        if (x >= UPD_X0 && x <= UPD_X1) {
                            if (updater_install()) running = 0;
                            prompt = 0;
                        } else if (x >= LATER_X0 && x <= LATER_X1) {
                            updater_dismiss();
                            prompt = 0;
                            show_waiting();   /* next stream frame repaints */
                            have_frame = 0;
                            dirty = 1;
                        }
                    }
                    break;
                }
                net_send_touch(ev.button.which, 2, ev.button.x, ev.button.y);
                break;
            case SDL_KEYDOWN:
                if (ev.key.keysym.sym == SDLK_ESCAPE) running = 0;
                else net_send_key(ev.key.keysym.sym, 1);
                break;
            case SDL_KEYUP:
                net_send_key(ev.key.keysym.sym, 0);
                break;
            case SDL_QUIT:
                running = 0;
                break;
            }
        }

        now = SDL_GetTicks();
        if (now >= next_wake) {
            /* keep the display on / block Exhibition while streaming */
            PDL_ServiceCall("palm://com.palm.power/com/palm/power/activityStart",
                            WAKE_PAYLOAD);
            next_wake = now + WAKE_INTERVAL_MS;
        }

        if (net_connected()) {
            last_conn = now;
        } else {
            if (now - last_conn > IDLE_EXIT_MS) {
                fprintf(stderr, "no connection for an hour, exiting\n");
                running = 0;
            }
            if (!arg_override && now - last_conf_poll > CONF_POLL_MS) {
                /* self-heal: the sender rewrites the conf with its IP on
                 * launch; pick it up without a receiver restart */
                char nh[128];
                int np = port;
                memcpy(nh, host, sizeof nh);
                parse_config(nh, sizeof nh, &np);
                if (strcmp(nh, host) != 0 || np != port) {
                    fprintf(stderr, "config changed: %s:%d -> %s:%d\n",
                            host, port, nh, np);
                    memcpy(host, nh, sizeof host);
                    port = np;
                    net_set_target(host, port);
                }
                last_conf_poll = now;
            }
        }

        if (!prompt && updater_has_update()) {
            fprintf(stderr, "showing update prompt (version %s)\n",
                    updater_get_version());
            prompt = 1;
            show_screen(update_jpg, update_jpg_len);
            dirty = 1;
        }

        /* while the prompt is up, leave frames in the net mailbox
         * (latest-wins: no backlog) so it isn't painted over */
        len = prompt ? 0 : net_take_frame(&jpg, &jpg_cap);
        if (len > 0) {
            int w, h;
            Uint32 t0 = SDL_GetTicks(), t1;
            if (decode_jpeg(jpg, len, s_rgb, SCREEN_W, SCREEN_H, &w, &h)) {
                t1 = SDL_GetTicks();
                stat_decode += t1 - t0;
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                                GL_RGB, GL_UNSIGNED_SHORT_5_6_5, s_rgb);
                stat_upload += SDL_GetTicks() - t1;
                set_frame_size(w, h);
                have_frame = 1;
                dirty = 1;
                if (++fps_frames == 100) {
                    Uint32 dt = SDL_GetTicks() - fps_t0;
                    fprintf(stderr, "fps: %.1f decode: %ums upload: %ums swap: %ums (per 100)\n",
                            100000.0 / (dt ? dt : 1),
                            stat_decode, stat_upload, stat_swap);
                    fps_frames = 0;
                    stat_decode = stat_upload = stat_swap = 0;
                    fps_t0 = SDL_GetTicks();
                }
            }
        }

        /* a dead connection means the last stream frame is stale —
         * fall back to the embedded waiting screen */
        if (!prompt && have_frame && !net_connected()) {
            show_waiting();
            have_frame = 0;
            dirty = 1;
        }

        /* redraw on new frame, else at ~5 Hz keepalive */
        if (dirty || now - last_draw > 200) {
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            {
                Uint32 ts = SDL_GetTicks();
                SDL_GL_SwapBuffers();
                stat_swap += SDL_GetTicks() - ts;
            }
            last_draw = now;
        } else {
            SDL_Delay(3);
        }
    }

    fprintf(stderr, "shutting down\n");
    net_stop();
    free(jpg);
    free(s_rgb);
    SDL_Quit();
    PDL_Quit();
    return 0;
}
