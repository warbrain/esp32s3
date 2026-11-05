"""PTT test: registers as the peer and never asks for capture.

Only the BOOT button on the board can start the uplink, so any AUDIO_UP frame
that arrives here proves the whole key -> capture -> opus -> udp chain.

Usage: python bridge_ptt_test.py [--host 172.16.30.145] [--seconds 25]
"""
import argparse
import socket
import struct
import time

MAGIC = b"\xA5\xC3"
HEADER = struct.Struct("<2sBBHHI")
T_HELLO, T_AUDIO_UP, T_AUDIO_DOWN, T_CTRL, T_STATUS = 1, 2, 3, 4, 5


def frame(ftype, payload=b"", seq=0):
    return HEADER.pack(MAGIC, ftype, 0, seq, len(payload),
                       int(time.time() * 1000) & 0xFFFFFFFF) + payload


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="172.16.30.145")
    ap.add_argument("--port", type=int, default=5000)
    ap.add_argument("--seconds", type=float, default=25.0)
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(1.0)
    target = (args.host, args.port)

    # Register ourselves as the peer without touching capture state.
    sock.sendto(frame(T_CTRL, b"volume:70"), target)
    print(f"[ptt] registered as peer of {args.host}:{args.port}", flush=True)
    print("[ptt] press and hold BOOT on the board for ~2 s ...", flush=True)

    deadline = time.time() + args.seconds
    frames = 0
    bytes_total = 0
    first_at = None

    while time.time() < deadline:
        try:
            data, _addr = sock.recvfrom(2048)
        except socket.timeout:
            # keep the peer entry alive on the board side
            sock.sendto(frame(T_CTRL, b"volume:70"), target)
            continue
        if len(data) < HEADER.size:
            continue
        magic, ftype, _f, _seq, ln, _ts = HEADER.unpack_from(data)
        if magic != MAGIC:
            continue
        if ftype == T_AUDIO_UP:
            if first_at is None:
                first_at = time.time()
                print("[ptt] first AUDIO_UP frame arrived <- button works", flush=True)
            frames += 1
            bytes_total += ln
        elif ftype == T_CTRL:
            print(f"[ptt] board says: {data[HEADER.size:HEADER.size+ln].decode(errors='replace')}",
                  flush=True)

    print(f"[ptt] {frames} frames, {bytes_total} B, ~{bytes_total * 8 / max(1, frames) / 1000:.1f} kbit/pkt",
          flush=True)
    print("[ptt] RESULT: " + ("PASS" if frames > 20 else "FAIL (no audio from the button)"), flush=True)
    sock.close()


if __name__ == "__main__":
    main()
