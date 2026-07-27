#!/bin/bash
# Inspect the installed VLC port on a USB-connected TouchPad.
# novacom's "run file:///bin/sh -- -c" arg passing is unreliable —
# pipe commands to the shell's stdin instead.
set -e

echo "== Connected devices =="
novacom -l

echo 'ls /media/cryptofs/apps/usr/palm/applications/ | grep -i -e vlc -e nizovn' | novacom run file://bin/sh
echo 'cat /media/cryptofs/apps/usr/palm/applications/org.webosarchive.vlcplayer/appinfo.json' | novacom run file://bin/sh
