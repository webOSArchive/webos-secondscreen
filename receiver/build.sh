#!/bin/bash
# Cross-compile the receiver for the TouchPad (run on the Linux VM).
set -e
cd "$(dirname "$0")"

TC=$HOME/linaro-toolchain/bin
PDK=/opt/PalmPDK
CC="$TC/arm-linux-gnueabi-gcc"
[ -x "$CC" ] || CC="$TC/arm-linux-gnueabi-gcc-4.9.4"

# libjpeg-turbo (NEON) is statically linked; its headers must shadow the
# PDK's plain libjpeg 6.2 headers. Rebuild recipe: third_party/README.md
TURBO=third_party/libjpeg-turbo

# appinfo.json is the single source of the version; the updater sends it
# to App Museum II for the newer-version comparison
VER=$(sed -n 's/.*"version": *"\([^"]*\)".*/\1/p' appinfo.json)
[ -n "$VER" ] || { echo "ERROR: no version in appinfo.json" >&2; exit 1; }

CFLAGS="-O2 -mcpu=cortex-a8 -mfpu=neon -mfloat-abi=softfp"
CFLAGS="$CFLAGS -D__webos__ -DLINUX -I$TURBO/include -I$PDK/include -I$PDK/include/SDL"
CFLAGS="$CFLAGS -Wall -fsigned-char -D_GNU_SOURCE=1 -D_REENTRANT"
# expanded unquoted at the compile line: the escaped quotes reach gcc
# intact and make APP_VERSION a C string literal
CFLAGS="$CFLAGS -DAPP_VERSION=\"$VER\""
LDFLAGS="-L$PDK/device/lib -Wl,-rpath-link,$PDK/device/lib"
LIBS="-lSDL -lpdl -lGLES_CM $TURBO/libjpeg.a -lpthread -lm"

mkdir -p build
xxd -i -n waiting_jpg assets/waiting.jpg > build/waiting_jpg.h
xxd -i -n update_jpg assets/update.jpg > build/update_jpg.h
$CC $CFLAGS -Ibuild src/main.c src/net.c src/decode.c src/updater.c src/glibc_compat.c -o build/secondscreen $LDFLAGS $LIBS
"$TC/arm-linux-gnueabi-strip" build/secondscreen

# The device glibc is 2.5-era: refuse to ship a binary that sneaked in
# newer versioned symbols (e.g. __isoc99_* from GLIBC_2.7).
BAD=$("$TC/arm-linux-gnueabi-readelf" -V build/secondscreen \
      | grep -oE 'GLIBC_2\.[0-9]+' | sort -uV | awk -F. '$2 > 5' || true)
if [ -n "$BAD" ]; then
    echo "ERROR: binary requires glibc versions newer than device (2.5):" >&2
    echo "$BAD" >&2
    exit 1
fi
echo "OK: build/secondscreen"
