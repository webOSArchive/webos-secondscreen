# webOS Second Screen — Plan

Use an HP TouchPad (webOS 3.0.5) as a wireless second screen for the Mac,
Duet/Sidecar-style: virtual display on the Mac, streamed over LAN to a
TouchPad client, with touch → mouse (and later keyboard) sent back.

## Architecture

- **Mac side:** Swift helper — creates a 1024×768 virtual display
  (`CGVirtualDisplay`, private-but-stable API; fallback: BetterDisplay or
  region mirroring), captures with ScreenCaptureKit, compresses frames,
  serves over TCP on the LAN.
- **TouchPad side:** PDK app — SDL 1.2 + OpenGL ES 1.1 (SDL-owned GL
  context, `SDL_GL_SwapBuffers()`, link `-lGLES_CM`, **never raw EGL** —
  avoids the 3-layer compositor touch flicker). Decodes frames, draws a
  fullscreen textured quad, sends touch events back over the same socket.

## Key constraints (drive all decisions)

- PDK apps **cannot reach the hardware H.264 decoder** — decode is CPU-bound
  on the dual-core Cortex-A8 (with NEON).
- **MJPEG is the v1 codec**: libjpeg-turbo NEON ≈ 25–35 ms/frame at
  1024×768 → ~20–25 fps on one core; second core handles network + GL
  upload. ~12–16 Mbps at moderate quality; TouchPad has dual-band 802.11n.
  Expected latency < ~150 ms.
- Touch input: webOS SDL delivers up to 5 fingers via nonstandard
  `event.button.which`. Call `PDL_GesturesEnable(PDL_FALSE)` so edge
  touches reach the app.
- Keyboard: `PDL_SetKeyboardState()` summons the system virtual keyboard;
  keys arrive as SDL key events.
- TLS 1.3 is solved on this TouchPad (July 2026) — old TLS workarounds not
  needed; Phase 0/1 use plain HTTP/TCP on LAN anyway.
- Toolchain: Linaro GCC 4.9.4 (`arm-linux-gnueabi-`), PalmPDK headers/libs,
  PalmSDK packaging tools, novacom deploy. All installed on this Mac.

## Phases

- **Phase 0 — validate with zero new code** *(DONE 2026-07-25)*: Mac
  ffmpeg (H.264 baseline mpegts over HTTP listen) → **on-device ffmpeg**
  (shipped inside the vlcplayer app; fbdev output + NEON) → `/dev/fb0`.
  Result: **21 fps sustained at 1024×768 H.264** end-to-end over WiFi
  with `-fflags nobuffer -flags low_delay` (16 fps via the VLC app path),
  ~1s from connection to first picture. Encode at 20 fps for drift-free
  playback. Latency drifts if encode fps exceeds decode ceiling — no
  frame dropping in this path; confirms Phase 1's MJPEG latest-frame-wins
  design. Scripts in `phase0/`; `touchpad-play.sh` is the working recipe.
  Audio: on-device ffmpeg ALSA output (`-f alsa default`) WORKS despite
  PulseAudio (confirmed audible tone test). A/V plan: mux AAC into the
  mpegts on the Mac (system audio captured via BlackHole 2ch), split
  on-device: `-map 0:v -f fbdev /dev/fb0 -map 0:a -f alsa default` —
  see `stream-screen-av.sh` / `touchpad-play-av.sh`.
  Findings: the VLC app itself CANNOT open network URLs (hardcodes
  localFile mode in VlcMedia, passes argv raw without JSON-unwrapping
  launcher params — launcher-passed params arrive as JSON like
  `{"target":"url"}` or `["url"]`). Launch the `start` script directly
  from a shell for clean argv. luna-send produces no output/effect via
  `novacom run` piped shell (works from a real novaterm tty). novacom
  arg passing after `--` is unreliable; pipe commands to `sh` stdin.
- **Phase 1 — the real client**: PDK SDL+GLES app + Mac Swift capture
  server, MJPEG over plain TCP, touch-to-mouse back-channel.
- **Phase 2 — polish**: virtual keyboard, scroll/right-click gestures,
  audio streaming (ScreenCaptureKit system audio → Opus → SDL audio),
  CGVirtualDisplay for a true extended desktop.
- **Phase 3 (stretch)**: software H.264 (ffmpeg NEON asm) for higher fps
  and lower bandwidth.

Expectations: ~20 fps at ~150 ms latency — good for Teams, video,
dashboards; not Sidecar, not for fast window-dragging.

## Status (2026-07-26): Phase 1 receiver WORKING at 27 fps

The custom PDK receiver (`receiver/`) is built, installed, and rendering
the Linux VM's test stream at **~27 fps sustained** (1024×768, decode
~30 ms + upload ~5 ms per frame). Touch back-channel implemented but not
yet human-verified. Development now happens on the **Linux VM**
(192.168.10.45, bridged LAN; TouchPad on WiFi at 192.168.10.67 — note
DHCP moved it from .90).

Key findings (receiver bring-up, 2026-07-26):

- **webOS SDL GL context**: must call
  `SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1)` and
  `SDL_SetVideoMode(0, 0, 0, SDL_OPENGL)` — requesting explicit
  width/height/bpp or skipping the version attribute fails with
  "Could not create EGL context" (see PDK samples in
  /opt/PalmPDK/share/samplecode/simple).
- **Texture upload was the bottleneck, not decode**: RGB888
  glTexSubImage2D costs ~74 ms/frame (driver-side CPU conversion);
  decoding straight to **RGB565** (libjpeg-turbo `JCS_RGB565`) and
  uploading `GL_UNSIGNED_SHORT_5_6_5` drops upload to ~5 ms. 10 fps → 27 fps.
- **PDK's bundled libjpeg 6.2 has no SIMD** (~16 ms/frame even so with
  IFAST; turbo NEON RGB565 ~30 ms — 565 conversion costs a bit more but
  wins overall). Static libjpeg-turbo 1.5.3 NEON build vendored at
  `receiver/third_party/libjpeg-turbo/` (recipe in its README).
- **Device glibc is 2.5-era**: Linaro 4.9.4-built binaries work, but
  glibc ≥2.7 symbols creep in (`__isoc99_sscanf` from turbo) — shimmed
  in `src/glibc_compat.c`; build.sh refuses binaries needing >2.5.
- Deploy loop without reinstalling the IPK: `killall secondscreen`, then
  `novacom put file:///media/cryptofs/apps/.../secondscreen` + chmod +x,
  relaunch. (novacom put fails with 'file open failed' if the app is
  still running — kill first, wait ~2 s.)
- Receiver logs to `/media/internal/com.webosarchive.secondscreen.log`; test
  server is `server-test/serve.py` (port 5959, `--x11` to mirror the VM
  display, latest-frame-wins mailbox sender). Protocol in
  `receiver/PROTOCOL.md`.

Later the same day — all Phase 1 receiver goals CONFIRMED by user:

- **Touch verified end-to-end**: taps on the TouchPad arrive at the test
  server with correct coords and finger index. Quirk: webOS SDL emits a
  `move` immediately *before* each `down` — injectors must click only on
  down/up (noted in PROTOCOL.md).
- **Real-video test**: 40 s Mandelbrot-zoom H.264 looped via
  `serve.py --file` played at **~18.5 fps** — worst-case JPEG content
  (~220 KB/frame ≈ 35 Mbps WiFi + 43 ms/frame decode). User: "the
  motion looks beautiful". Normal content runs ~27 fps.
- **VM desktop mirroring is impossible via x11grab**: the VM session is
  Wayland; the XWayland `:0` root is solid black (only X11 client
  windows would show). `serve.py --x11` therefore only works on real
  Xorg. Not worth fixing — the real capture source is the Mac.
  Regenerate a test movie with:
  `ffmpeg -f lavfi -i "mandelbrot=size=1024x768:rate=20:end_scale=0.00005" -t 40 -c:v libx264 -preset fast -pix_fmt yuv420p sample.mp4`
- **Waiting screen**: gray + icon + "Waiting for Connection..." rendered
  at build time into `receiver/assets/waiting.jpg`, embedded via
  `xxd -i` (build.sh), decoded through the normal frame path at startup
  and on disconnect. Regenerate with ffmpeg overlay+drawtext (see git
  history of assets/) if the design changes.

### Mac sender — DONE (2026-07-26): Phase 1 COMPLETE end-to-end

The Swift capture server (`sender/`, SwiftPM, macOS 13+) is built and
streaming this Mac (192.168.10.20 — DHCP moved it from .37; TouchPad WiFi
.67, both confirmed) to the TouchPad: **sender 24.7 fps / ~12 Mbps /
~8 ms encode; receiver renders ~20 fps** on busy desktop content (device
decode ~42 ms/frame is the ceiling; simpler content runs faster). Run:
`sender/.build/release/secondscreen-sender` (flags: --fps --quality
--display --dry-run --check-permissions; see sender/README.md).

Implementation facts:

- ScreenCaptureKit scales on-GPU to aspect-fit ≤1024×768; encoder
  letterboxes onto a black 1024×768 canvas (the receiver stretches every
  frame fullscreen, so non-4:3 frames must be padded server-side).
  ImageIO JPEG (quality 0.6 ≈ 60 KB desktop frames) is baseline — safe
  for the device libjpeg-turbo.
- Latest-frame-wins = 1-slot mailbox + **SO_SNDBUF capped at 128 KB** so
  TCP backpressure drops frames at the mailbox instead of autotuning MBs
  of kernel-queue latency. SCK sends nothing while the screen is static;
  a `P` keepalive every 3 s doubles as dead-client detection.
- Touch: finger 0 → CGEvent (mouseMoved/leftMouseDragged; click strictly
  on down/up; double-tap detection sets mouseEventClickState). Mapping
  through the letterbox rect verified exact via --dry-run. `K` keys:
  unicode-string injection for printables, keycode map for specials.
- Permissions (Screen Recording + Accessibility) attach to the terminal
  app that launches the process; both were already granted on this Mac.
  CGPreflight/CGRequestScreenCaptureAccess + AXIsProcessTrustedWithOptions
  trigger the prompts on first run.
- **novacom on this Mac sees the TouchPad over USB** (`topaz-linux`) even
  though receiver dev is in the Linux VM — conf write + killall +
  relaunch (`cd <appdir> && ./secondscreen &` piped to
  `novacom run file:///bin/sh`) works from here, and the receiver's 2 s
  auto-retry latches onto a restarted sender by itself.
- Capture runs only while a client is connected; one client at a time.

Remaining validation: human look at the picture + touch feel (all
plumbing confirmed by logs). Then the polish backlog below.

### Phase 2 progress (2026-07-26): CGVirtualDisplay DONE — true second screen

The sender now creates a **virtual 1024×768 monitor by default** (private
CGVirtualDisplay API via the CVDShim ObjC target; works on macOS 15.7
Intel) and streams THAT — extended desktop, not a mirror. Verified: OS
lists it as a real display (arranged left of main at (-1024,0) by
default), WindowServer renders a real desktop + menu bar on it
(screencapture -D 2 confirmed), receiver shows it, and SCK's
change-driven capture means a static second screen streams at ~zero
bandwidth. `--mirror` / `--display N` keep the old mirroring mode
(auto-fallback if the private API ever breaks). Notes: the display lives
for the sender's process lifetime (stable vendor/serial → macOS
remembers arrangement); native 1024×768 render = no letterbox + sharper
+ cheaper decode. Injector needed zero changes (CGDisplayBounds of the
virtual display).

Gotcha (hit by user on first stop/restart, showed as a 768×768 screen):
a freshly created CGVirtualDisplay registers with transient garbage
geometry (observed 1×1 for ~1.5 s) while WindowServer mode-sets — if
capture starts inside that window, SCDisplay reports a clamped size and
the stream inherits it. Fix (in sender): after applySettings, poll
CGDisplayBounds until it reads exactly 1024×768 (re-apply settings if it
sticks), and hard-code the capture config to 1024×768 for the virtual
display instead of trusting SCDisplay's reported size. macOS reuses the
same displayID and remembers the user's arrangement across sender
restarts (stable vendor/product/serial).

Polish backlog (user: "we'll add some polish later"): app icon design,
in-app server-address UI, virtual keyboard (`PDL_SetKeyboardState`),
audio ('A' messages), scroll/right-click gestures, auto-reconnect
backoff, on-screen connection-state toast.
(2026-07-26 evening: keyboard + audio declared **v2**; current scope is
community release. Icons drawn by user in `artwork/` — 48/64/256/512 px.)

### Distribution prep DONE (2026-07-26 evening, Mac side)

- **USB auto-launch**: sender startup checks `novacom -l`; if a TouchPad
  is on USB it ALWAYS rewrites secondscreen.conf with the sender's
  current IP (the receiver binary hard-codes the dev VM as its default
  host — Linux-side report 2026-07-26 evening — so the conf override is
  the mechanism that points fresh installs at the right machine), then
  launches the receiver only if not running (`killall -0` probe).
  Running receivers are never restarted (dev sessions); they read the
  refreshed conf on next launch.
  Honors PLAN gotchas: script piped to `/bin/sh` stdin, no luna-send,
  binary launched directly. `--no-autolaunch` to disable. NOTE: when the
  Linux VM holds the USB passthrough, novacom on the Mac sees nothing —
  expected, not a bug.
- **App bundle**: `sender/package-app.sh` → signed
  `dist/webOS Second Screen.app` (Developer ID: webOS Archive
  Z97JEYX9UJ, hardened runtime, icns from artwork/, LSUIElement
  menu-bar app with state + Arrange Displays… + Quit). Finder launches
  log to ~/Library/Logs/webOSSecondScreen.log. Notarize with
  `NOTARIZE=1 ./package-app.sh` after one-time
  `xcrun notarytool store-credentials notary ...`.
- **Bundled TCC identity**: the .app prompts for its own Screen
  Recording + Accessibility grants (separate from the terminal's).
  Screen Recording granted + verified streaming; missing-permission
  Finder launch shows an NSAlert that opens the Settings pane.
- Receiver-side icon/polish work happening in the Linux VM (user).

### Community release 0.2.2 SHIPPED (2026-07-26 night)

Both sides released as **0.2.2**; user published the GitHub release and
pushed. Update channels are deliberately split:

- **Sender**: GitHub Releases (`webOSArchive/webos-secondscreen`). Startup
  check in `sender/.../UpdateCheck.swift` hits `releases/latest`, compares
  tags numerically (`v` prefix ok; pre-releases invisible to that
  endpoint), and is notify-only — a log line plus an "Update Available"
  menu-bar item opening the release page. No self-updating.
- **Receiver**: App Museum II (`receiver/src/updater.c`) — ancient
  platform, different mechanism. Versions kept in sync BY HAND (user).

`appVersion` in UpdateCheck.swift is the single source of truth;
package-app.sh reads it for Info.plist (no more hardcoded VERSION).
Update check verified against a temporary fake 1.0.0 release. Notarization
credentials are stored (profile "notary") and the full
`NOTARIZE=1 ./package-app.sh` build→sign→notarize→staple flow works; the
stapled zip in `sender/dist/` is the release artifact.

Also shipped: touch-offset-after-rearrange fix (01bd215 — Injector now
reads CGDisplayBounds per event). Lesson from its "regression": the fix
was committed but the running .app predated it — after committing sender
changes, rebuild AND reinstall to /Applications (compare binary mtime vs
commit time before debugging "the fix didn't work").

### Receiver: idle-exit fix + screensaver (2026-07-27, unreleased)

The one-hour idle exit never fired overnight — NOT a kill/exit problem.
A sleeping Mac leaves TCP half-open (no FIN/RST), the receiver's `recv`
blocked forever, `net_connected()` stayed true, so the timer never
started counting (log evidence: no `net: disconnected` line all night).
Lesson: on this path silence is the only dead-peer signal.

- **Dead-peer detection** (`net.c`): 10 s `SO_RCVTIMEO` on the stream
  socket; sender guarantees a frame or `P` ping every 3 s (now a
  liveness rule in PROTOCOL.md). Timeout flows into the normal
  disconnect path: waiting screen → screensaver → idle exit.
- **Screensaver**: DVD-style bouncing icon on black after 15 min
  disconnected. Sprite `assets/saver-icon.jpg` (make-saver-icon.py,
  128×128, must match `SAVER_ICON_*`) parked in the texture strip below
  row 768. Touch wakes it and resets the idle clock; reconnect or the
  update prompt also ends it.
- **Conf-overridable timeouts**: `saver_secs=` (900) / `idle_secs=`
  (3600) in secondscreen.conf — testing doesn't have to wait an hour.
- GL lesson: a sprite windowed out of a shared texture needs a 1-texel
  guard band — `GL_LINEAR` at the window edge samples the neighboring
  texels (stale frame rows / never-uploaded garbage), visible as a
  flickering border at fractional positions.

Verified on-device with a silent accept-then-hang server + 30/90 s conf:
connect → 10 s drop → saver on → exit, process gone. Awaiting an
overnight run at default timing before release (would be receiver 0.2.3;
museum versions synced by hand).

## Status (2026-07-25) and immediate next steps

Phase 0 video is DONE and repeatable: real Mac screen mirrored to the
TouchPad at 20 fps (test pattern measured 21 fps sustained), ~0.5–1s
latency — "totally ok for a Teams meeting" (user). Too laggy for video
with LOCAL Mac audio, hence the A/V plan: send audio to the TouchPad too
so it stays in sync with the mirrored video.

- [x] Video pipeline validated end-to-end (21 fps, 1024×768)
- [x] TouchPad ALSA audio confirmed audible (tone test via ffmpeg)
- [x] BlackHole 2ch installed and working (post-reboot)
- [x] A/V pipeline debugged and running stably: UDP push (Mac →
      udp://192.168.10.90:5000), MP2 audio, split avfoundation inputs.
      Device player ran without errors; server held exactly 1.0x for
      17+ min. Audio *audibility* on the TouchPad speakers NOT yet
      human-confirmed (user was remote; camera check inconclusive).
- [ ] **NEXT (user back in room):** rerun `phase0/stream-screen-av.sh` +
      `phase0/touchpad-play-av.sh "" 600`, confirm audio is audible,
      then play real video content and judge A/V sync over ~10 min.
      If audio drifts: try `-af aresample=async=1` on the device side.
      REMEMBER: Mac sound output must be "BlackHole 2ch" while testing;
      switch back to MacBook Pro Speakers afterward.
- [ ] Then: Phase 1 scaffolding (Mac Swift capture server + PDK SDL/GLES
      MJPEG client with latest-frame-wins and touch back-channel).

### VLC-Qt patch work — TODO on the Linux VM (Intel Mac)

The vlc-qt build environment lives in a **Linux VM on the other (Intel)
Mac** at `~/Projects/vlc-qt` (the on-device ffmpeg 2.8.22 was built there:
prefix `$HOME/Projects/vlc-qt/webos/vlc-arm`, Linaro GCC 4.9.4).
When picking this up:

1. **Network MRL fix (the essential one).** In the player app's
   MainWindow (openFile/playFile path), VlcMedia is constructed with
   `localFile=true` unconditionally. Change to:
   `bool local = !input.contains("://");` and pass that. Result: http/udp
   URLs open directly and play in seconds — no more multi-minute retry
   roulette. (Today VLC only ever played via an undocumented delayed
   internal retry; direct opens always fail with the mangled
   `file:////http...` MRL. FIFO trick does NOT work around it: libVLC
   2.2's file access sees the FIFO's st_size=0 and fails with
   "cannot pre fill buffer".)
2. **Launcher param parsing (nice-to-have).** The app passes argv[1] raw
   to openFile. Launcher-delivered params arrive as JSON
   (`["url"]` from luna-send params array, `{"target":"url"}` from
   palm-launch -p). Add QupZilla-style unwrapping (detect leading `[` or
   `{`, extract the string, unescape `\/`) so `palm-launch -p` works and
   novaterm isn't needed to launch with a URL.
3. Rebuild the app (not ffmpeg/libVLC — only the Qt app layer changed),
   repackage the IPK, `palm-install`, and test with
   `phase0/stream-screen-av-http.sh` on this Mac (HTTP pull; on-device
   feeder ffmpeg confirmed the device parses the full A/V mpegts —
   H.264 1024×666@15 + MP2 44.1k stereo — over HTTP).

### Session findings 2026-07-25 (evening) — transport & environment

- **webOS firewall: INPUT policy DROP.** All unsolicited inbound traffic
  is dropped — inbound UDP silently never arrives (zero packets, zero
  drops on the socket; ffmpeg blocks forever in probe, looking exactly
  like a hang). Fix: `iptables -I INPUT -p udp --dport 5000 -j ACCEPT`
  (not persistent across reboot). Device-pull over TCP (HTTP) needs no
  rule — RELATED,ESTABLISHED is allowed.
- **WiFi power management shreds sustained streams when the screen is
  off/idle** (ping goes 7–300ms erratic). `iwconfig eth0 power off`
  (also not persistent). With screen on + power mgmt off, UDP A/V ran
  end-to-end: user confirmed picture AND audio on the TouchPad
  ("acceptable"), with dropouts every 5–10s (chirp + resume) = the raw
  path's 1MB UDP fifo overflowing — no player-side buffering. 15 fps +
  `-threads 2` reduced but did not eliminate it.
- **Raw /dev/fb0 writing is not viable for real use**: Exhibition (the
  active lock screen) takes the display back, and there's no UI surface.
  A real app (VLC now, custom receiver later) holds a wake lock and owns
  the screen — this is the decided direction.

### For the custom receiver app (Phase 1) — carry-over from VLC findings

- **Network MRL patch**: the VLC port's MainWindow/playFile constructs
  VlcMedia with `localFile=true` unconditionally, mangling any URL into
  `file:////http...`. The custom receiver (and any patched VLC build)
  must detect `://` in the input and open network MRLs as network
  location (VlcMedia localFile=false), not as a file path.
- An app shell (vs raw /dev/fb0 writes) is REQUIRED for real use: it
  provides the wake lock (blocks Exhibition/lock screen takeover), a
  place for UI controls, and proper lifecycle. Raw fb writing loses the
  screen to Exhibition and can't show UI.
- FIFO trick (until then): `mknod /tmp/stream.ts p` (busybox has no
  mkfifo), feed it with on-device ffmpeg `-c copy` from HTTP, hand VLC
  the FIFO path — its file-mode open works and it never knows it's
  playing a network stream. /tmp is tmpfs: never write a REGULAR file
  there from a stream (fills RAM).

### A/V debugging lessons (hard-won, do not rediscover)

- avfoundation with screen+audio in ONE input ("2:BlackHole 2ch")
  silently drops ALL audio frames — the screen input owns the session
  clock. Use two `-f avfoundation` inputs and `-map 0:v -map 1:a`.
- BlackHole delivers no frames when no app is playing audio — a stream
  started in silence has zero audio packets, and the device probe then
  fails with `abuffer time_base 1/0` (params never fill in). Keep audio
  playing, or expect that error at player start.
- ffmpeg `-listen 1` (HTTP serve) buffers capture while waiting for a
  client, then bursts the backlog at 40x+ on connect, wrecking A/V
  interleave; each ffprobe also consumes the single connection. UDP push
  (`udp://<tp-ip>:5000?pkt_size=1316` → `udp://0.0.0.0:5000` with
  fifo_size/overrun_nonfatal on the device) avoids all of it and paces
  the encoder at exactly 1.0x.
- MP2 audio over mpegts probes more reliably than AAC on the device's
  ffmpeg 2.8 and decodes cheaper on the Cortex-A8.
- Device ffmpeg quirks: `-fflags nobuffer` starves stream probing for
  audio (fine for video-only); busybox `ps` shows the player as `ld.so`,
  not `ffmpeg`; the device pipeline's `| tail -5` hides all output until
  process exit — write to a device file and cat it when debugging.

## Phase 0 runbook

Mac LAN IP: `192.168.10.37`. Stream URL for VLC: `http://192.168.10.37:8080/tp.ts`

1. Connect the TouchPad over USB; verify with `novacom -l`.
2. `phase0/inspect-vlc.sh` — find the VLC port's app id + appinfo.json
   (binary name, whether it takes a URL launch param).
3. `phase0/stream-test.sh` — serve a synthetic 1024×768 test pattern
   (validates decode without screen-capture permissions).
4. `phase0/launch-vlc.sh <app-id> [url]` — launch VLC on-device with the
   stream URL via `luna-send` application manager (PDK apps get params as
   argv JSON).
5. `phase0/stream-screen.sh` — stream the real Mac screen ("Capture
   screen 0" = avfoundation device index 2). Requires Screen Recording
   permission for the terminal app (grant + relaunch).

## Useful device facts

- App installs: `/media/cryptofs/apps/usr/palm/applications/<app-id>/` (read-only)
- User storage: `/media/internal/` (writable; Qt5 apps log here — stderr
  does NOT reach /var/log/messages for Qt5 apps)
- Kill PDK apps with `killall` via novacom (`palm-launch -c` doesn't work)
- VLC port stack: nizovn Qt5 5.9.7 / glibc / OpenSSL packages, jailed via
  `qt5sdk` key in appinfo.json
