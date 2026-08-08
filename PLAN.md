# webOS Second Screen — Plan

Use an HP TouchPad (webOS 3.0.5) as a wireless second screen for the Mac,
Duet/Sidecar-style: a virtual display on the Mac, streamed over LAN to a
TouchPad client, with touch (and keyboard) sent back.

## Architecture

- **Mac side** (`sender/`, SwiftPM, macOS 13+): creates a 1024×768 virtual
  display (`CGVirtualDisplay`, private-but-stable API), captures with
  ScreenCaptureKit, JPEG-encodes, serves over TCP (`Server.swift`).
- **TouchPad side** (`receiver/`, PDK/C): SDL 1.2 + OpenGL ES 1.1 app
  (SDL-owned GL context, `SDL_GL_SwapBuffers()`, link `-lGLES_CM` — never
  raw EGL, which causes a 3-layer compositor touch flicker). Decodes MJPEG
  frames, draws a fullscreen textured quad, sends touch/key events back
  over the same socket.
- **Protocol**: framed TCP messages, documented in `receiver/PROTOCOL.md`.

## Key constraints (why the design is what it is)

- PDK apps cannot reach the hardware H.264 decoder — decode has to be
  CPU-bound (dual-core Cortex-A8 + NEON).
- **MJPEG is the codec**: libjpeg-turbo NEON gets ~20–27 fps at 1024×768
  on-device. Decoding straight to RGB565 (not RGB888) for texture upload
  was the fix that got there — RGB888's driver-side conversion was the
  real bottleneck (10 fps → 27 fps).
- Touch: webOS SDL delivers up to 5 fingers via nonstandard
  `event.button.which`; `PDL_GesturesEnable(PDL_FALSE)` lets edge touches
  reach the app. Keyboard: `PDL_SetKeyboardState()` summons the system
  virtual keyboard.
- Toolchain: Linaro GCC 4.9.4 (`arm-linux-gnueabi-`), PalmPDK/PalmSDK,
  novacom deploy — installed on the Mac used for receiver builds. Device
  glibc is 2.5-era; builds must avoid glibc ≥2.7 symbols.
- Liveness detection (both directions) must be **bytes received**, never
  socket/connect state — a sleeping peer's kernel keeps completing TCP
  handshakes on its own, and half-open sockets never trigger an error
  path. This exact bug bit the project three separate times (see history)
  before every liveness check was traffic-gated.

## Current status (2026-08-08)

Both sides released as **0.3.0** — sender notarized zip in `sender/dist/`,
receiver on App Museum II. Core product is feature-complete and stable for
daily use:

- Extended virtual desktop (not mirroring), ~20–27 fps MJPEG depending on
  content, touch → mouse.
- Auto-discovery both ways: USB novacom auto-config + WiFi subnet sweep,
  rate-limited to avoid tripping router-side scan protection.
- Robustness: dead-peer detection on both sender and receiver, accept-loop
  backoff on capture failure (interruptible immediately on Mac wake),
  idle-exit + screensaver on the receiver, startup update checks on both
  sides (GitHub Releases for sender, App Museum II for receiver — versions
  kept in sync by hand).
- Machine/IP details and control recipes live in memory
  (`secondscreen-network-map.md`), not duplicated here since they change
  often (DHCP).

Not yet done: backlog below. Project is otherwise "at rest" — pick up from
new bug reports or backlog demand.

## Brief history

- **2026-07-25 — Phase 0 (proof of concept, since superseded):** validated
  the idea using off-the-shelf VLC + ffmpeg on the TouchPad — 20 fps H.264
  over WiFi, ~1s latency. Established that a real app shell (not raw
  `/dev/fb0`) is required to hold the screen against Exhibition. The
  hard-won ffmpeg/avfoundation/webOS-firewall lessons from this phase are
  preserved in git history (`git log -p -- PLAN.md`, late July) in case
  audio-streaming work ever revisits that pipeline.
- **2026-07-26 — Phase 1, the real product:** built the custom PDK
  receiver and Mac Swift sender from scratch — MJPEG over TCP,
  latest-frame-wins, touch back-channel. Working end-to-end the same day,
  then extended same day to a true `CGVirtualDisplay` second screen
  instead of mirroring.
- **2026-07-26/27 — Distribution:** signed + notarized Mac app bundle,
  GitHub Releases update-check (sender) / App Museum II (receiver), USB
  auto-config. Shipped as community release **0.2.2**, then **0.2.3**
  (conf-preserving auto-config; receiver idle-exit + screensaver fix for
  the "silent half-open socket" liveness bug, round one).
- **2026-08-03 — 0.2.4:** receiver-initiated WiFi subnet discovery; sender
  answers a discovery probe with its hostname.
- **2026-08-07 — 0.2.5:** receiver's idle/screensaver timers switched from
  connection-based to traffic-based (liveness bug, round two — this time
  on the receiver's read side); sender accept-loop backoff on repeated
  capture failure; sender TCP keepalive to detect a silently-vanished
  client.
- **2026-08-08 — 0.3.0:** receiver-side discovery-sweep rate limiting (was
  aggressive enough to trip router-side protection); sender's backoff wait
  now interrupts immediately on Mac wake instead of finishing out a stale
  interval (`NSWorkspace.didWakeNotification` → `DispatchSemaphore`),
  verified on real hardware.

## Future ideas / backlog

- **v2 scope** (deferred from initial release): keyboard support (`K`
  messages — protocol exists, no receiver-side UI yet), audio streaming
  (ScreenCaptureKit system audio → TouchPad ALSA, `A` messages),
  scroll/right-click gestures, in-app server-address UI instead of
  conf-file editing, on-screen connection-state toast.
- **Stretch:** software H.264 (ffmpeg NEON asm) for higher fps / lower
  bandwidth than MJPEG, if the CPU budget allows.
- **VLC-Qt patch** (low priority; Linux VM on the other Intel Mac,
  `~/Projects/vlc-qt`): the VLC port's `MainWindow` still can't open
  network URLs (hardcodes `localFile=true`) — only matters if the project
  ever goes back to a VLC-based player instead of the custom receiver.

## Useful device facts

- App installs: `/media/cryptofs/apps/usr/palm/applications/<app-id>/`
  (read-only). User storage: `/media/internal/` (writable; receiver logs
  to `org.webosarchive.secondscreen.log` here).
- Kill PDK apps via novacom `killall` (`palm-launch -c` doesn't work).
- Deploy loop without reinstalling the IPK: `killall secondscreen`, then
  `novacom put file:///media/cryptofs/apps/.../secondscreen` + chmod +x,
  relaunch (kill first — `novacom put` fails "file open failed" against a
  running binary).
- novacom `run` arg-passing after `--` is unreliable — pipe commands to
  `/bin/sh` stdin instead.
- After committing sender changes: rebuild AND reinstall to
  /Applications before testing — a stale running `.app` looks exactly
  like a regression.
