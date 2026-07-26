#!/usr/bin/env python3
"""Test MJPEG frame server for the Second Screen receiver (PROTOCOL.md).

Serves a 1024x768 MJPEG stream over TCP with latest-frame-wins pacing and
prints touch/key events the receiver sends back.

  ./serve.py                 # ffmpeg testsrc2 pattern, 20 fps
  ./serve.py --x11           # grab the VM's X display instead
  ./serve.py --fps 15 --quality 7
"""
import argparse
import os
import socket
import struct
import subprocess
import sys
import threading

SOI = b"\xff\xd8"
EOI = b"\xff\xd9"

ACTIONS = {0: "down", 1: "move", 2: "up"}


def ffmpeg_cmd(args):
    if args.file:
        # real-content test: loop a video, letterboxed to 1024x768
        return ["ffmpeg", "-hide_banner", "-loglevel", "error",
                "-re", "-stream_loop", "-1", "-i", args.file,
                "-vf", ("fps={fps},scale=1024:768:force_original_aspect_ratio="
                        "decrease,pad=1024:768:(ow-iw)/2:(oh-ih)/2").format(fps=args.fps),
                "-f", "image2pipe", "-vcodec", "mjpeg",
                "-q:v", str(args.quality), "-"]
    if args.x11:
        # NOTE: only works on a real Xorg session — under Wayland the
        # XWayland root is black (only X11 client windows are visible)
        src = ["-f", "x11grab", "-framerate", str(args.fps),
               "-video_size", "1024x768",
               "-i", os.environ.get("DISPLAY", ":0")]
    else:
        # -re paces the pattern at realtime so measured latency is honest
        src = ["-re", "-f", "lavfi",
               "-i", f"testsrc2=size=1024x768:rate={args.fps}"]
    return ["ffmpeg", "-hide_banner", "-loglevel", "error", *src,
            "-f", "image2pipe", "-vcodec", "mjpeg", "-q:v", str(args.quality),
            "-"]


def jpeg_frames(pipe):
    """Yield complete JPEGs from a concatenated MJPEG byte stream."""
    buf = bytearray()
    while True:
        chunk = pipe.read(65536)
        if not chunk:
            return
        buf += chunk
        while True:
            start = buf.find(SOI)
            if start < 0:
                buf.clear()
                break
            end = buf.find(EOI, start + 2)
            if end < 0:
                if start > 0:
                    del buf[:start]
                break
            yield bytes(buf[start:end + 2])
            del buf[:end + 2]


def read_client(conn):
    """Parse and print messages from the receiver."""
    try:
        while True:
            hdr = conn.recv(5, socket.MSG_WAITALL)
            if len(hdr) < 5:
                return
            mtype, length = chr(hdr[0]), struct.unpack(">I", hdr[1:])[0]
            payload = conn.recv(length, socket.MSG_WAITALL) if length else b""
            if len(payload) < length:
                return
            if mtype == "H":
                w, h, ver = struct.unpack(">HHB", payload[:5])
                print(f"[client] hello: {w}x{h} protocol v{ver}")
            elif mtype == "T":
                finger, action, x, y = struct.unpack(">BBHH", payload[:6])
                print(f"[touch] finger={finger} {ACTIONS.get(action, action)} "
                      f"({x},{y})")
            elif mtype == "K":
                sym, down = struct.unpack(">HB", payload[:3])
                print(f"[key] sym={sym} {'down' if down else 'up'}")
            else:
                print(f"[client] unknown message {mtype!r} len={length}")
    except OSError:
        return


def serve_client(conn, args):
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    threading.Thread(target=read_client, args=(conn,), daemon=True).start()

    proc = subprocess.Popen(ffmpeg_cmd(args), stdout=subprocess.PIPE)

    # latest-frame-wins: capture thread overwrites a 1-slot mailbox; the
    # sender never lets TCP backpressure grow a frame backlog
    slot = {"frame": None}
    cond = threading.Condition()

    def capture():
        for frame in jpeg_frames(proc.stdout):
            with cond:
                slot["frame"] = frame
                cond.notify()
        with cond:
            slot["frame"] = b""  # sentinel: capture ended
            cond.notify()

    threading.Thread(target=capture, daemon=True).start()

    sent = 0
    try:
        while True:
            with cond:
                cond.wait_for(lambda: slot["frame"] is not None)
                frame, slot["frame"] = slot["frame"], None
            if frame == b"":
                print("[server] capture ended")
                return
            conn.sendall(b"J" + struct.pack(">I", len(frame)) + frame)
            sent += 1
            if sent % 100 == 0:
                print(f"[server] {sent} frames sent (last {len(frame)} bytes)")
    except OSError as e:
        print(f"[server] client dropped: {e}")
    finally:
        proc.terminate()
        proc.wait()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", type=int, default=5959)
    ap.add_argument("--fps", type=int, default=20)
    ap.add_argument("--quality", type=int, default=7,
                    help="mjpeg q:v, lower is better quality (2-31)")
    ap.add_argument("--x11", action="store_true",
                    help="grab the X display instead of the test pattern "
                         "(requires real Xorg, not Wayland)")
    ap.add_argument("--file", metavar="VIDEO",
                    help="loop a video file as the source (letterboxed)")
    args = ap.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", args.port))
    srv.listen(1)
    source = args.file or ("x11grab" if args.x11 else "test pattern")
    print(f"[server] listening on :{args.port} ({source}, {args.fps} fps)")
    while True:
        conn, addr = srv.accept()
        print(f"[server] client connected from {addr[0]}:{addr[1]}")
        try:
            serve_client(conn, args)
        finally:
            conn.close()
            print("[server] waiting for next client")


if __name__ == "__main__":
    main()
