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
#include "waiting_jpg.h"     /* generated from assets/waiting.jpg by build.sh */
#include "update_jpg.h"      /* generated from assets/update.jpg by build.sh */
#include "saver_icon_jpg.h"  /* generated from assets/saver-icon.jpg by build.sh */

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
/* alongside the conf, not /tmp: rename() can't cross the tmpfs boundary */
#define CONF_TMP_PATH CONF_PATH ".tmp"

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
 * back with a new IP (its sender rewrites the conf on launch).
 * Both timeouts can be overridden in the conf file (idle_secs= /
 * saver_secs=) — mostly so tests don't have to wait an hour. */
#define IDLE_EXIT_MS  (60u * 60u * 1000u)
#define CONF_POLL_MS  60000u

/* Screensaver: bounce the app icon on black after this long without a
 * connection (touch wakes it and resets the idle-exit clock) */
#define SAVER_MS      (15u * 60u * 1000u)
#define SAVER_ICON_W  128    /* must match assets/make-saver-icon.py */
#define SAVER_ICON_H  128
#define SAVER_TEX_Y   768    /* icon parked in the texture strip frames never touch */
#define SAVER_SPEED   90.0f  /* px/s */
#define SAVER_TICK_MS 33     /* ~30 fps animation */

static GLuint s_tex;
static GLfloat s_verts[8] = { 0, 0, SCREEN_W, 0, 0, SCREEN_H, SCREEN_W, SCREEN_H };
static GLfloat s_texco[8];

/* disconnect housekeeping timeouts; conf-file overridable (main thread only) */
static Uint32 s_idle_exit_ms = IDLE_EXIT_MS;
static Uint32 s_saver_ms = SAVER_MS;

/* discover=0 pins the receiver to the configured host: without it, a
 * configured address that stops answering is replaced by whatever the
 * sweep finds — and rewritten in the conf file. */
static int s_conf_discover = 1;

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
        } else if (strncmp(line, "idle_secs=", 10) == 0) {
            long v = atol(line + 10);
            if (v > 0) s_idle_exit_ms = (Uint32)v * 1000u;
        } else if (strncmp(line, "saver_secs=", 11) == 0) {
            long v = atol(line + 11);
            if (v > 0) s_saver_ms = (Uint32)v * 1000u;
        } else if (strncmp(line, "discover=", 9) == 0) {
            s_conf_discover = atoi(line + 9) != 0;
        }
    }
    fclose(f);
}

/* Persist a discovered address, rewriting only host= and leaving every
 * other key (port=, saver_secs=, idle_secs=) exactly as the user left it. */
static void save_config_host(const char *host)
{
    char line[256];
    FILE *in, *out;

    out = fopen(CONF_TMP_PATH, "w");
    if (!out) {
        fprintf(stderr, "config: cannot write %s\n", CONF_TMP_PATH);
        return;
    }
    in = fopen(CONF_PATH, "r");
    if (in) {
        int newline = 1;
        while (fgets(line, sizeof line, in)) {
            if (strncmp(line, "host=", 5) == 0) continue;
            fputs(line, out);
            newline = strchr(line, '\n') != NULL;
        }
        if (!newline) fputc('\n', out);   /* file didn't end in one */
        fclose(in);
    }
    fprintf(out, "host=%s\n", host);
    if (fclose(out) != 0 || rename(CONF_TMP_PATH, CONF_PATH) != 0)
        fprintf(stderr, "config: failed to save host=%s\n", host);
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
    glClearColor(0, 0, 0, 1);

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

static void set_quad(GLfloat x0, GLfloat y0, GLfloat x1, GLfloat y1)
{
    s_verts[0] = x0; s_verts[1] = y0;
    s_verts[2] = x1; s_verts[3] = y0;
    s_verts[4] = x0; s_verts[5] = y1;
    s_verts[6] = x1; s_verts[7] = y1;
}

static void set_texwin(GLfloat u0, GLfloat v0, GLfloat u1, GLfloat v1)
{
    s_texco[0] = u0; s_texco[1] = v0;
    s_texco[2] = u1; s_texco[3] = v0;
    s_texco[4] = u0; s_texco[5] = v1;
    s_texco[6] = u1; s_texco[7] = v1;
}

/* fullscreen quad showing the current frame (also undoes the saver's
 * icon-sized quad — every screen/frame repaint goes through here) */
static void set_frame_size(int w, int h)
{
    set_quad(0, 0, SCREEN_W, SCREEN_H);
    set_texwin(0, 0, (GLfloat)w / TEX_W, (GLfloat)h / TEX_H);
}

/* One-time upload of the saver sprite into the unused strip below the
 * frame area. Returns 1 if the saver is usable.
 *
 * The sprite gets a 1-texel black guard band: GL_LINEAR sampling at the
 * window edge blends in the neighboring texels (stale frame rows above,
 * never-uploaded garbage right/below), which flickers as a thin border
 * while the icon moves through fractional positions. */
static int saver_load_icon(void)
{
    int w, h;
    memset(s_rgb, 0, (SAVER_ICON_W + 2) * (SAVER_ICON_H + 2) * 2);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, SAVER_TEX_Y,
                    SAVER_ICON_W + 2, SAVER_ICON_H + 2,
                    GL_RGB, GL_UNSIGNED_SHORT_5_6_5, s_rgb);
    if (!decode_jpeg(saver_icon_jpg, saver_icon_jpg_len, s_rgb,
                     SCREEN_W, SCREEN_H, &w, &h) ||
        w != SAVER_ICON_W || h != SAVER_ICON_H) {
        fprintf(stderr, "saver: icon decode failed (%dx%d), saver disabled\n", w, h);
        return 0;
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 1, SAVER_TEX_Y + 1, w, h,
                    GL_RGB, GL_UNSIGNED_SHORT_5_6_5, s_rgb);
    return 1;
}

int main(int argc, char *argv[])
{
    char host[128] = DEFAULT_HOST;
    int port = DEFAULT_PORT;
    SDL_Surface *screen;
    uint8_t *jpg = NULL;
    size_t jpg_cap = 0;
    int have_frame = 0, running = 1, prompt = 0, arg_override = 0;
    int saver = 0, saver_ok = 0;
    GLfloat sav_x = 0, sav_y = 0, sav_vx = 0, sav_vy = 0;
    Uint32 last_anim = 0;
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
    fprintf(stderr, "timeouts: saver %us, idle exit %us\n",
            s_saver_ms / 1000u, s_idle_exit_ms / 1000u);
    fprintf(stderr, "discovery: %s\n",
            arg_override ? "off (argv target)" :
            s_conf_discover ? "on" : "off (discover=0)");

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
    saver_ok = saver_load_icon();
    show_waiting();

    /* an argv target owns the run, so it also suppresses the sweep */
    net_set_discovery(!arg_override && s_conf_discover);
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
                if (saver) {
                    /* touch wakes the saver and counts as activity for
                     * the idle-exit clock */
                    fprintf(stderr, "screensaver off (touch)\n");
                    saver = 0;
                    show_waiting();
                    last_conn = SDL_GetTicks();
                    dirty = 1;
                    break;
                }
                if (prompt) break;   /* prompt swallows touches */
                net_send_touch(ev.button.which, 0, ev.button.x, ev.button.y);
                break;
            case SDL_MOUSEMOTION:
                if (saver || prompt) break;
                net_send_touch(ev.motion.which, 1, ev.motion.x, ev.motion.y);
                break;
            case SDL_MOUSEBUTTONUP:
                if (saver) break;
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

        {
            /* the sweep found the Mac: persist it, and keep our own copy in
             * step so the conf poll below doesn't retarget back to the
             * address that just failed */
            char dh[128];
            if (net_take_discovered(dh, sizeof dh)) {
                fprintf(stderr, "discovered sender at %s, saving to config\n", dh);
                memcpy(host, dh, sizeof host);
                save_config_host(host);
            }
        }

        if (net_connected()) {
            if (saver) {
                fprintf(stderr, "screensaver off (reconnected)\n");
                saver = 0;
                show_waiting();   /* next stream frame repaints */
                dirty = 1;
            }
            last_conn = now;
        } else {
            if (now - last_conn > s_idle_exit_ms) {
                fprintf(stderr, "no connection for %us, exiting\n",
                        s_idle_exit_ms / 1000u);
                running = 0;
            }
            if (!saver && !prompt && saver_ok && now - last_conn > s_saver_ms) {
                fprintf(stderr, "screensaver on\n");
                saver = 1;
                sav_x = (SCREEN_W - SAVER_ICON_W) / 2.0f;
                sav_y = (SCREEN_H - SAVER_ICON_H) / 2.0f;
                sav_vx = SAVER_SPEED;
                sav_vy = SAVER_SPEED * 0.75f;
                last_anim = now;
                dirty = 1;
            }
            if (!arg_override && now - last_conf_poll > CONF_POLL_MS) {
                /* self-heal: the sender rewrites the conf with its IP on
                 * launch; pick it up without a receiver restart */
                char nh[128];
                int np = port;
                memcpy(nh, host, sizeof nh);
                parse_config(nh, sizeof nh, &np);
                /* discover= is re-read here too, so the sweep can be
                 * turned off (or back on) without restarting the app */
                net_set_discovery(s_conf_discover);
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
            saver = 0;   /* prompt takes the screen */
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
                saver = 0;   /* connection came back within this iteration */
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

        if (saver && now - last_anim >= SAVER_TICK_MS) {
            GLfloat dt = (now - last_anim) / 1000.0f;
            if (dt > 0.25f) dt = 0.25f;   /* clamp after a stall */
            sav_x += sav_vx * dt;
            sav_y += sav_vy * dt;
            if (sav_x < 0)                        { sav_x = 0; sav_vx = -sav_vx; }
            if (sav_x > SCREEN_W - SAVER_ICON_W)  { sav_x = SCREEN_W - SAVER_ICON_W; sav_vx = -sav_vx; }
            if (sav_y < 0)                        { sav_y = 0; sav_vy = -sav_vy; }
            if (sav_y > SCREEN_H - SAVER_ICON_H)  { sav_y = SCREEN_H - SAVER_ICON_H; sav_vy = -sav_vy; }
            last_anim = now;
            dirty = 1;
        }

        /* redraw on new frame, else at ~5 Hz keepalive */
        if (dirty || now - last_draw > 200) {
            if (saver) {
                glClear(GL_COLOR_BUFFER_BIT);
                set_quad(sav_x, sav_y,
                         sav_x + SAVER_ICON_W, sav_y + SAVER_ICON_H);
                /* +1: skip the guard band row/column */
                set_texwin((GLfloat)1 / TEX_W,
                           (GLfloat)(SAVER_TEX_Y + 1) / TEX_H,
                           (GLfloat)(1 + SAVER_ICON_W) / TEX_W,
                           (GLfloat)(SAVER_TEX_Y + 1 + SAVER_ICON_H) / TEX_H);
            }
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
