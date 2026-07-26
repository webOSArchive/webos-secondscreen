# Second Screen receiver (TouchPad PDK app)

MJPEG-over-TCP receiver. See `../PLAN.md` for architecture and status,
`PROTOCOL.md` for the wire format.

## Build & deploy (on the Linux VM, TouchPad on USB)

```sh
./build.sh                 # cross-compile (Linaro 4.9.4 + /opt/PalmPDK)
./package.sh               # build the IPK
palm-install build/org.webosarchive.secondscreen_0.2.0_all.ipk
```

Fast iteration without reinstalling (after the IPK is on the device):

```sh
echo "killall secondscreen 2>/dev/null" | novacom run file:///bin/sh; sleep 2
novacom put file:///media/cryptofs/apps/usr/palm/applications/org.webosarchive.secondscreen/secondscreen < build/secondscreen
echo "chmod +x /media/cryptofs/apps/usr/palm/applications/org.webosarchive.secondscreen/secondscreen; \
  /media/cryptofs/apps/usr/palm/applications/org.webosarchive.secondscreen/secondscreen >/dev/null 2>&1 &" \
  | novacom run file:///bin/sh
```

Log: `cat /media/internal/org.webosarchive.secondscreen.log` (fps + stage
timings every 100 frames).

## Server selection

Defaults to `192.168.10.45:5959`. Override order (later wins):

1. `/media/internal/secondscreen.conf` — lines `host=…` / `port=…`
2. launch param: `{"host":"1.2.3.4","port":5959}` or bare `host:port`

## Test stream

```sh
python3 -u ../server-test/serve.py            # 20 fps test pattern
python3 -u ../server-test/serve.py --x11      # mirror the VM's X display
```

Screen colors: red = not connected, dark blue = connected & waiting for
first frame. Before long WiFi sessions run `iwconfig eth0 power off` on
the device (not persistent across reboot).
