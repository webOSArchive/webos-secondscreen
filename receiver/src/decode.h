#ifndef DECODE_H
#define DECODE_H

#include <stddef.h>
#include <stdint.h>

/* Decodes a complete baseline JPEG from memory into tightly-packed RGB565.
 * rgb must hold at least max_w*max_h*2 bytes; frames larger than
 * max_w×max_h are rejected. Returns 1 on success and sets out_w and out_h,
 * 0 on any decode error (corrupt frame — safe to just skip it). */
int decode_jpeg(const uint8_t *data, size_t len, uint8_t *rgb,
                int max_w, int max_h, int *out_w, int *out_h);

#endif
