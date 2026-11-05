"""UDP datagram self test for the ESP32 audio bridge.

The board has no connection state: it learns the peer from the first valid
datagram. So the PC just fires a CTRL frame at the board and starts listening.

Usage: python bridge_udp_test.py [--host 172.16.30.145] [--seconds 6]
"""
import argparse
import math
import socket
import struct
import time
import wave

MAGIC = b"\xA5\xC3"
HEADER = struct.Struct("<2sBBHHI")
T_HELLO, T_AUDIO_UP, T_AUDIO_DOWN, T_CTRL, T_STATUS = 1, 2, 3, 4, 5
RATE = 16000


def frame(ftype, payload=b"", seq=0):
    return HEADER.pack(MAGIC, ftype, 0, seq, len(payload),
                       int(time.time() * 1000) & 0xFFFFFFFF) + payload


def tone(freq, seconds, amp=0.3):
    out = bytearray()
    for i in range(int(RATE * seconds)):
        out += struct.pack("<h", int(amp * 32767 * math.sin(2 * math.pi * freq * i / RATE)))
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="172.16.30.145")
    ap.add_argument("--port", type=int, default=5000)
    ap.add_argument("--seconds", type=float, default=6.0)
    ap.add_argument("--out", default="mic_udp.wav")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    sock.settimeout(2.0)
    target = (args.host, args.port)

    # Wait for the board's discovery beacon so a fresh boot is not missed.
    print(f"[udp] waiting for beacon from {args.host} ...", flush=True)
    sock.settimeout(6.0)
    try:
        while True:
            data, addr = sock.recvfrom(2048)
            print(f"[udp] beacon from {addr[0]}: {data[12:].decode(errors='replace')}", flush=True)
            break
    except socket.timeout:
        print("[udp] no beacon (board not up yet?), blasting anyway", flush=True)

    print(f"[udp] target {args.host}:{args.port}", flush=True)
    for _ in range(10):
        sock.sendto(frame(T_CTRL, b"capture_start"), target)
        time.sleep(0.1)

    pcm = bytearray()
    frames = 0
    gaps = 0
    last_seq = None
    deadline = time.time() + args.seconds

    while time.time() < deadline:
        try:
            data, addr = sock.recvfrom(2048)
        except socket.timeout:
            continue
        if len(data) < HEADER.size:
            continue
        magic, ftype, _f, seq, ln, _ts = HEADER.unpack_from(data)
        if magic != MAGIC:
            continue
        body = data[HEADER.size:HEADER.size + ln]
        if ftype == T_AUDIO_UP:
            if last_seq is not None:
                expected = (last_seq + 1) & 0xFFFF
                if seq != expected:
                    gaps += (seq - expected) & 0xFFFF
            last_seq = seq
            pcm += body
            frames += 1
        elif ftype in (T_HELLO, T_CTRL, T_STATUS):
            print(f"[udp] <- {ftype}: {body.decode(errors='replace')}", flush=True)

    sock.sendto(frame(T_CTRL, b"capture_stop"), target)

    secs = len(pcm) / 2 / 2 / RATE  # s16le, 2 channels
    peak = 0
    if pcm:
        peak = max(abs(v) for v in struct.unpack(f"<{len(pcm)//2}h", pcm))
    print(f"[udp] {frames} frames, {secs:.2f} s of stereo audio, {gaps} frames lost, peak {peak}",
          flush=True)

    if pcm:
        with wave.open(args.out, "wb") as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(RATE)
            w.writeframes(bytes(pcm))
        print(f"[udp] saved {args.out}", flush=True)

    print("[udp] pushing 1.5 s 440 Hz tone to the speaker", flush=True)
    data = tone(440.0, 1.5)
    for i in range(0, len(data), 640):
        sock.sendto(frame(T_AUDIO_DOWN, data[i:i + 640], i // 640), target)
        time.sleep(0.02)

    expect = args.seconds * 50
    verdict = "PASS" if frames > expect * 0.9 else "LOW"
    print(f"[udp] RESULT: {verdict} ({frames}/{int(expect)} expected frames)", flush=True)
    sock.close()


if __name__ == "__main__":
    main()
