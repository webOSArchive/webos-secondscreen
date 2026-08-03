#ifndef DISCOVER_H
#define DISCOVER_H

#include <stddef.h>

/* Sweep the local /24 for a second-screen sender listening on `port`.
 *
 * Blocking: ~2s when nothing answers. A host that holds the port open but
 * never speaks costs 4s to rule out, and up to 4 such candidates are
 * interrogated, so a pathological subnet can take ~18s.
 *
 * On success returns an open, validated socket (blocking mode, no
 * lingering timeouts) and writes the peer's dotted-quad into host[].
 * The caller adopts that socket as the live link rather than re-dialing:
 * the sender starts capturing the moment it accepts, so a close/redial
 * would cost it a capture stop/start cycle for nothing.
 *
 * Returns -1 if no sender answered. */
int discover_sweep(int port, char *host, size_t hostlen);

#endif
