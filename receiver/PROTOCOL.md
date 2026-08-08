# Second Screen wire protocol (v1)

Single TCP connection. The **device connects to the server** (device-pull:
no firewall rule needed on webOS — RELATED,ESTABLISHED is allowed).
Set `TCP_NODELAY` on both ends.

Every message, both directions, is framed as:

```
offset  size  field
0       1     type (ASCII byte)
1       4     payload length, big-endian u32
5       n     payload
```

## Server → client

| type | payload | meaning |
|------|---------|---------|
| `J`  | complete baseline JPEG | one video frame (≤ 1024×768) |
| `A`  | reserved | audio chunk (Phase 2) |
| `P`  | empty | ping / keepalive (client ignores) |
| `Y`  | `"SSND"`, u8 protocol_version, u8 name_len, name bytes (UTF-8, ≤ 63) | answer to a `Q` probe: yes, I'm a sender, and this is my name |

The server should apply **latest-frame-wins** on send: keep a 1-slot queue
and drop stale frames rather than letting TCP backpressure grow latency.

**Liveness:** the server must send *something* at least every 3 s — a
frame, or a `P` ping when the screen is static. The client drops the
link after 10 s of silence (a sleeping server leaves TCP half-open with
no FIN/RST, so silence is the only dead-peer signal) and falls back to
its reconnect loop.

## Client → server

| type | payload | meaning |
|------|---------|---------|
| `H`  | u16be width, u16be height, u8 protocol_version (=1) | hello, sent on connect |
| `Q`  | `"SSCR"`, u8 protocol_version | discovery probe: are you a second-screen sender? |
| `T`  | u8 finger (0–4), u8 action (0=down, 1=move, 2=up), u16be x, u16be y | touch event in screen coords |
| `K`  | u16be SDL keysym, u8 down | key event (virtual keyboard, Phase 2) |

Note: webOS SDL emits a `move` for a finger immediately *before* its
`down` (cursor positioning). Injectors should treat `move` as pointer
positioning only and press/release strictly on `down`/`up`.

## Discovery

When the configured address keeps failing, the device sweeps its own /24
with non-blocking connects to the server port and interrogates whatever
accepts. The socket that passes is kept as the live link — the server
starts capturing the moment it accepts, so redialling would cost it a
capture stop/start for nothing.

Discovery is device-driven for the same reason the stream is: webOS only
admits inbound packets conntrack sees as RELATED,ESTABLISHED. A server-side
broadcast beacon never arrives, and neither does an mDNS response —
conntrack keys the reply tuple on the multicast destination, so the
server's unicast answer doesn't match the entry. Outbound unicast TCP is
the only direction that reliably works.

A host that accepts is confirmed in one of two ways:

1. It answers `Q` with a well-formed `Y` (magic `"SSND"`).
2. It ignores `Q` but sends a valid `J` or `P` within 4 s — a server from
   before `Q` existed. A server sends *something* within 3 s of accepting
   (a frame, or a `P` on a static screen), so this identifies pre-0.2.4
   servers without them having to change.

Anything else is closed and the sweep moves on. Servers must therefore
keep ignoring unknown message types rather than dropping the link, and
must serialise `Y` against frame sends — they go out from different
threads and interleaved writes would shred the framing.

A server that is already streaming to another device leaves the probe
sitting in its listen backlog, never answers, and is correctly skipped.

Sweep rate decays with the age of the disconnection — every 15 s for the
first four minutes, every 30 s through the seventh, then 1/min — and
stops entirely while the screensaver is up. The clock restarts when a
link carries traffic or the user dismisses the screensaver.

A completed TCP handshake does not count as progress. A sleeping Mac
accepts out of its listen backlog and then says nothing, so the receiver
treats "accepted but never spoke" as a known-good address with a
sleeping sender: it waits 30 s and redials rather than sweeping, because
a sweep would only rediscover the same silent host.
