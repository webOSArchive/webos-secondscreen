#!/bin/bash
# THE WORKING PHASE 0 RECIPE (validated 2026-07-25, ~16fps at 1024x768):
# on-device ffmpeg (shipped inside the vlcplayer app, fbdev+NEON build)
# pulls the HTTP stream and paints the framebuffer directly.
# Run a stream server first (stream-test.sh or stream-screen.sh).
# Usage: ./touchpad-play.sh [url] [seconds]
set -e

URL="${1:-http://192.168.10.37:8080/tp.ts}"
DURATION="${2:-}"
[ -n "$DURATION" ] && TFLAG="-t $DURATION"

echo "killall vlcplayer 2>/dev/null
APP=/media/cryptofs/apps/usr/palm/applications/org.webosarchive.vlcplayer
GLIBC=/media/cryptofs/apps/usr/palm/applications/com.nizovn.glibc/lib
\$GLIBC/ld.so --library-path \$APP/lib:\$GLIBC \$APP/bin/ffmpeg -hide_banner \
  -fflags nobuffer -flags low_delay -i $URL -an -pix_fmt bgra $TFLAG \
  -f fbdev /dev/fb0 2>&1 | tail -5" | novacom run file://bin/sh
