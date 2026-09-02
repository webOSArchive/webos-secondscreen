# Second Screen receiver (TouchPad PDK app)

MJPEG-over-TCP receiver. See `../PLAN.md` for architecture and status,
`PROTOCOL.md` for the wire format.

## Build & deploy (on the Linux VM, TouchPad on USB)

```sh
./build.sh                 # cross-compile (Linaro 4.9.4 + /opt/PalmPDK)
./package.sh               # build the IPK
palm-install build/org.webosarchive.secondscreen_*.ipk
```

The app version comes from `appinfo.json` (compiled in as `APP_VERSION`
by build.sh — bump it there for a release).

Fast iteration without reinstalling (after the IPK is on the device):

```sh
echo "killall secondscreen 2>/dev/null" | novacom run file:///bin/sh; sleep 2
novacom put file:///media/cryptofs/apps/usr/palm/applications/org.webosarchive.secondscreen/secondscreen < build/secondscreen
echo "chmod +x /media/cryptofs/apps/usr/palm/applications/org.webosarchive.secondscreen/secondscreen; \
  /media/cryptofs/apps/usr/palm/applications/org.webosarchive.secondscreen/secondscreen >/dev/null 2>&1 &" \
  | novacom run file:///bin/sh
```

Log: `cat /media/internal/org.webosarchive.secondscreen.log` (fps + stage
timings every 100 frames).

## Server selection

Defaults to `192.168.10.45:5959`. Override order (later wins):

1. `/media/internal/secondscreen.conf` — lines `host=…` / `port=…`
2. launch param: `{"host":"1.2.3.4","port":5959}` or bare `host:port`

The conf file can also override the disconnect timeouts (mainly for
testing): `saver_secs=…` (default 600) and `idle_secs=…` (default 3600).

While disconnected, the conf file is re-read every 60 s so a sender that
comes up with a new IP (it rewrites the conf on launch) is picked up
without restarting the receiver. An explicit launch-param target disables
that polling for the run — a dev session pointed at the VM can't be
yanked away by the Mac autolaunch.

**Subnet discovery** (`src/discover.c`, on by default): once the
configured target has failed a couple of dials in a row, the receiver
sweeps its own /24 for a sender and adopts whatever answers correctly
(see `PROTOCOL.md`'s `Q`/`Y` handshake, with a fallback for pre-0.2.4
senders identified by their frame traffic). A hit is persisted back to
`host=` in the conf file so it survives a restart. Same launch-param
override as above disables discovery for the run — it exists to recover
from a stale conf, not to fight an explicit target.

A sweep is a SYN to all 254 addresses on the /24, which reads as a port
scan to any router running IDS — an idle hour of them (198 sweeps, ~50k
SYNs, measured 2026-08-08) got this device blocked off the network until
it was rebooted. So the rate decays with the age of the disconnection
(`sweep_interval_ms()` in `net.c`): every 15 s for the first four
minutes, every 30 s through the seventh, then 1/min. Front-loaded
because a sweep only pays off when the Mac is coming up right about now;
decaying because the tail is what the IDS notices.

The fast phase is four minutes rather than one because a sleeping Mac
that still accepts out of its listen backlog parks us in the
silent-retry path for minutes without sweeping at all — measured at two
minutes, which under a one-minute fast phase left the search already
decayed to 30 s by the time the address actually went dead.

15 s and not 10 s for that head: sustained *rate* is what an IDS scores,
not just total volume, and four minutes at 6 sweeps/min would exceed the
3.3/min average that got the device blocked. At 15 s the peak is 4/min,
and a full idle stretch up to the screensaver costs ~24 sweeps against
the old 198 per hour.

Two things reset that clock back to the fast phase: a link that actually
carries traffic, and the user dismissing the screensaver. Crucially a
bare TCP connect does *not* — a sleeping Mac keeps completing handshakes
out of its listen backlog, and counting those as progress is what
produced the 198.

Sweeps stop entirely while the screensaver is up. Dials continue
throughout, so a Mac that wakes at its known address still brings the
stream back unattended; only *discovering* a new address needs a touch.
`next_wait_ms()` clamps the dial backoff to the sweep cadence, since a
sweep can only fire after a dial has failed — without it the 60 s
backoff would starve the fast phase down to one sweep a minute.

**Rationing sweeps is not the same as rationing reconnects.** A dial is
one SYN to one address the receiver already believes in; only the /24
sweep is what an IDS scores. So the first two minutes after a link goes
down are treated as a re-check — "are you still there?" — rather than a
search, and three things stay deliberately impatient there
(`RECHECK_MS`, `SILENT_RETRY_*` and `FIRST_RX_TIMEOUT_S` in `net.c`):

- the dial backoff is capped at 10 s no matter what the doubling has
  reached, and unlike the sweep clamp above this applies with discovery
  off or the screensaver's pause in effect too — otherwise a receiver
  pinned to `discover=0` sat out a full minute between dials within 90 s
  of a drop;
- a host that accepts and then says nothing gets 2 s, 4 s, 8 s, 16 s,
  30 s rather than a flat 30 s. The first such accept after a working
  link is usually the *sender* still tearing down the old session — its
  backlog completes our handshake while its accept loop is elsewhere —
  and a flat wait charged that race the sleeping-Mac price. The ramp is
  at 30 s inside the first minute, so an idle hour still costs what the
  flat wait cost it;
- the first read on a new connection has a 5 s deadline instead of the
  steady-state 10 s, raised the moment anything arrives. A live sender
  owes a byte within 3 s of accepting, so the socket still silent at 5 s
  is the one only the kernel accepted — no reason to spend the full
  dead-link timeout finding that out on every reconnect.

## Runtime behavior

- **Update check** (`src/updater.c`): at startup a background thread asks
  the webOS Archive App Catalog
  (`getLatestVersionInfo.php?app=webOS Second Screen/<version>`) for a
  newer `major.minor.build`. If found, an embedded prompt screen appears
  (Update Now → hands the `downloadURI` to Preware and exits; Later →
  dismissed for this run). The prompt meta is
  `assets/update.jpg`, regenerated by `assets/make-update-jpg.py` —
  its button rects must match the `BTN_*`/`UPD_*`/`LATER_*` defines in
  `src/main.c`.
- **Dead-peer detection**: the receiver drops the link after 10 s of
  silence (`SO_RCVTIMEO`; the sender guarantees a frame or ping every
  3 s), or after 5 s if the connection has not yet carried a single
  byte. A sleeping Mac leaves TCP half-open — without this the app
  thinks it's connected forever and neither the screensaver nor the
  idle exit ever triggers.
- **Heartbeat** (protocol v2): the same half-open trap catches the *sender*
  when this device walks out of WiFi range, and there it is worse — the
  sender takes one client at a time, so until its TCP gives up it is not
  accepting the reconnect we are busy making. So a sender that advertises
  `V` on connect gets an empty `P` back every 3 s, sent from the read loop
  (the sender owes us traffic every 3 s, so it wakes often enough to keep
  the interval without a timer). A sender that sends no `V` is pre-v2 and
  gets nothing — silence is what it expects, and an unknown message every
  3 s is not. A sweep-adopted socket gets its advert from the copy the
  sender repeats after the `Y`, since `discover.c`'s handshake loop has
  already eaten the first one. See `PROTOCOL.md`.
- **Screensaver**: after 10 min without a connection (`saver_secs=` in
  the conf) the app icon bounces around a black screen, DVD-player
  style. Touch wakes it back to the waiting screen (and resets the
  idle-exit clock); a reconnect or the update prompt also ends it. The
  sprite is `assets/saver-icon.jpg`, regenerated by
  `assets/make-saver-icon.py` — its size must match the `SAVER_ICON_*`
  defines in `src/main.c`.
- **Idle exit**: after 60 min without a connection or a touch
  (`idle_secs=` in the conf) the app exits cleanly. Otherwise the
  display wake-lock it holds would keep the TouchPad on all night after
  the Mac sleeps.

## Test stream

```sh
python3 -u ../server-test/serve.py            # 20 fps test pattern
python3 -u ../server-test/serve.py --x11      # mirror the VM's X display
```

Screen colors: red = not connected, dark blue = connected & waiting for
first frame. Before long WiFi sessions run `iwconfig eth0 power off` on
the device (not persistent across reboot).
