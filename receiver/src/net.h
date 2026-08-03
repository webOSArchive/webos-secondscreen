#ifndef NET_H
#define NET_H

#include <stddef.h>
#include <stdint.h>

#define PROTO_VERSION 1

void net_start(const char *host, int port);
void net_stop(void);
int  net_connected(void);

/* Retarget the client (e.g. the config file changed while disconnected).
 * Takes effect on the reconnect loop's next dial attempt; an established
 * connection is left alone. */
void net_set_target(const char *host, int port);

/* Subnet discovery after repeated dial failures (on by default). An
 * explicit argv target owns the whole run, so main disables it there. */
void net_set_discovery(int enabled);

/* Non-zero once after discovery has retargeted the client, filling host[]
 * with what it found — main persists that to the config file so the
 * self-heal poll doesn't drag the target back to the stale address. */
int net_take_discovered(char *host, size_t hostlen);

/* Latest-frame-wins hand-off: if a JPEG newer than the last take is
 * pending, swaps it into *buf (caller's old buffer is recycled as the
 * network thread's next slot) and returns its length; else returns 0.
 * buf and cap must start as NULL/0 and belong to the caller between calls. */
size_t net_take_frame(uint8_t **buf, size_t *cap);

void net_send_touch(int finger, int action, int x, int y);
void net_send_key(int sym, int down);

#endif
