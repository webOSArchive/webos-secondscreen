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

The server should apply **latest-frame-wins** on send: keep a 1-slot queue
and drop stale frames rather than letting TCP backpressure grow latency.

## Client → server

| type | payload | meaning |
|------|---------|---------|
| `H`  | u16be width, u16be height, u8 protocol_version (=1) | hello, sent on connect |
| `T`  | u8 finger (0–4), u8 action (0=down, 1=move, 2=up), u16be x, u16be y | touch event in screen coords |
| `K`  | u16be SDL keysym, u8 down | key event (virtual keyboard, Phase 2) |

Note: webOS SDL emits a `move` for a finger immediately *before* its
`down` (cursor positioning). Injectors should treat `move` as pointer
positioning only and press/release strictly on `down`/`up`.
