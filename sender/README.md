# secondscreen-sender — Mac capture server

Turns the TouchPad into a true second monitor: creates a **virtual
1024×768 display** via the private `CGVirtualDisplay` API (the OS treats
it as real — arrange it in System Settings → Displays, drag windows onto
it), captures it with ScreenCaptureKit, and streams it to the TouchPad
receiver (`../receiver/`) per `../receiver/PROTOCOL.md`: baseline JPEG →
framed `J` messages over TCP :5959 with latest-frame-wins. `--mirror`
falls back to mirroring an existing display (aspect-fit + letterbox).

Receiver `T` touch messages are injected as CGEvent mouse events aimed at
the captured display's global bounds (finger 0 only; move = position,
click strictly on down/up, double-tap → double-click); `K` key messages
type via `keyboardSetUnicodeString` (printables) or keycode map (arrows,
return, …).

## Build & run

```sh
swift build -c release
./.build/release/secondscreen-sender            # listens on :5959
```

Flags: `--port`, `--fps` (default 25), `--quality` (0–1, default 0.6),
`--mirror` / `--display N` / `--list-displays`, `--no-autolaunch`,
`--dry-run` (log injection instead of moving the mouse),
`--check-permissions`.

On startup the sender checks for a USB-connected TouchPad via novacom.
If found, it always rewrites `/media/internal/secondscreen.conf` with
this Mac's current IP (the receiver binary's built-in default points at
the dev VM, so the conf override is what makes fresh installs dial the
right machine), then launches the receiver if it isn't already running —
plug in the cable, open the app, done. A running receiver is never
restarted (it may be a dev session); it picks up the new conf on its
next launch.

A menu-bar item (display icon) shows connection state and offers
Arrange Displays… / Quit. When launched from Finder, logs go to
`~/Library/Logs/webOSSecondScreen.log`.

## App bundle for distribution

```sh
./package-app.sh                # build + bundle + Developer ID sign
NOTARIZE=1 ./package-app.sh     # + notarize & staple (see below)
```

Produces `dist/webOS Second Screen.app` (icon from `../artwork/`,
bundle id `org.webosarchive.secondscreen.sender`, LSUIElement). One-time
notarization credential setup:

```sh
xcrun notarytool store-credentials notary --apple-id <apple-id> --team-id Z97JEYX9UJ
```

The bundled app has its own TCC identity: first launch re-prompts for
Screen Recording and Accessibility (granted to "webOS Second Screen",
not your terminal). Without Screen Recording it shows an alert that opens
the right Settings pane.

Point the TouchPad at this Mac (also printed at startup):

```sh
echo 'echo "host=<mac-ip>" > /media/internal/secondscreen.conf;
killall secondscreen; sleep 2;
cd /media/cryptofs/apps/usr/palm/applications/org.webosarchive.secondscreen && ./secondscreen &' \
  | novacom run file:///bin/sh
```

The receiver retries every 2 s, so it latches onto a (re)started sender
by itself.

## Permissions (macOS)

Two grants in System Settings → Privacy & Security:

- **Screen & System Audio Recording** — required; capture fails without it.
- **Accessibility** — touch/key injection silently no-ops without it.

Running the app triggers the prompts / registers the entries. Who the
grants attach to depends on how you run it: the bundled .app has its own
identity ("webOS Second Screen"); the bare CLI binary inherits the
terminal app that launched it. Grants don't transfer between the two.
After granting Screen Recording to a terminal, macOS may ask to relaunch
it.

## Design notes

- The virtual display (CVDShim target declares the private CoreGraphics
  classes; shapes as used by FluffyDisplay/DeskPad) lives for the sender
  process lifetime — it survives client reconnects, and stable
  vendor/product/serial IDs make macOS remember its arrangement. Killing
  the sender unplugs the monitor and windows hop back. Verified working
  on macOS 15.7 (Sequoia, Intel).
- Content renders natively at 1024×768 → no letterbox, sharper text, and
  SCK only delivers frames on change, so a static second screen costs
  ~zero bandwidth.
- Capture runs only while a client is connected; one client at a time.
- Mirror mode: SCK scales on-GPU to ≤1024×768 aspect-fit; the encoder
  letterboxes onto a black 1024×768 canvas because the receiver stretches
  every frame fullscreen (sending non-4:3 frames would distort).
- Latest-frame-wins: 1-slot mailbox between the capture callback and the
  send loop, plus SO_SNDBUF capped at 128 KB so TCP backpressure reaches
  the mailbox (frames drop) instead of queueing seconds of latency.
- SCK delivers no frames while the screen is static; the send loop then
  emits a `P` keepalive every 3 s, which doubles as dead-client detection.
- Measured (Intel Mac, 2560×1440 → 1024×576, quality 0.6): encode
  ~8 ms/frame, ~60 KB/frame, ~12 Mbps at 25 fps; receiver renders ~20 fps
  (device JPEG decode is the ceiling on busy desktop content).
