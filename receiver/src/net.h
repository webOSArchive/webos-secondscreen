#ifndef NET_H
#define NET_H

#include <stddef.h>
#include <stdint.h>

void net_start(const char *host, int port);
void net_stop(void);
int  net_connected(void);

/* Latest-frame-wins hand-off: if a JPEG newer than the last take is
 * pending, swaps it into *buf (caller's old buffer is recycled as the
 * network thread's next slot) and returns its length; else returns 0.
 * buf and cap must start as NULL/0 and belong to the caller between calls. */
size_t net_take_frame(uint8_t **buf, size_t *cap);

void net_send_touch(int finger, int action, int x, int y);
void net_send_key(int sym, int down);

#endif
