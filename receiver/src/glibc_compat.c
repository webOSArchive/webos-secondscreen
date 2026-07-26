/* The device glibc is 2.5-era and lacks __isoc99_sscanf (GLIBC_2.7),
 * which the statically-linked libjpeg-turbo references (jmemmgr.c's
 * JPEGMEM parsing). Resolving it locally keeps the binary loadable. */
#include <stdio.h>
#include <stdarg.h>

int __isoc99_sscanf(const char *str, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vsscanf(str, fmt, ap);
    va_end(ap);
    return r;
}
