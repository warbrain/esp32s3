"""Two-way Opus test for the ESP32 audio bridge.

Uplink  : board --Opus--> PC, decoded with libopus and written to a WAV (and
          played through the PC speakers when --play is given).
Downlink: PC --Opus--> board speaker, from a WAV file or a generated tone.

Examples
    python bridge_duplex.py --tone 440          # beep on the board's speaker
    python bridge_duplex.py --wav tts.wav       # play a 16 kHz mono WAV on the board
    python bridge_duplex.py --listen 12 --play  # record the mic, then play it back here

The board registers the peer by itself, so nothing has to be started on the
board side: press BOOT for uplink, the downlink is pushed on demand.
"""
import argparse
import math
import os
import pathlib
import socket
import struct
import sys
import time
import wave

# opuslib needs a libopus DLL; pyogg ships one.
import pyogg  # noqa: F401  (used only for its bundled dll path)

_DLL_DIR = str(pathlib.Path(pyogg.__file__).parent)
os.environ["PATH"] = _DLL_DIR + ";" + os.environ["PATH"]
try:
    os.add_dll_directory(_DLL_DIR)
except Exception:
    pass
import ctypes

ctypes.CDLL(os.path.join(_DLL_DIR, "opus.dll"))
import opuslib

MAGIC = b"\xA5\xC3"
HEADER = struct.Struct("<2sBBHHI")
T_HELLO, T_AUDIO_UP, T_AUDIO_DOWN, T_CTRL, T_STATUS = 1, 2, 3, 4, 5
RATE = 16000
FRAME = 320          # 20 ms
BOARD_RATE = 16000
BOARD_CH = 2


def frame(ftype, payload=b"", seq=0):
    return HEADER.pack(MAGIC, ftype, 0, seq, len(payload),
                       int(time.time() * 1000) & 0xFFFFFFFF) + payload


def play_wav(path):
    try:
        import winsound
        winsound.PlaySound(str(path), winsound.SND_FILENAME)
        return True
    except Exception as exc:
        print(f"[duplex] playback failed: {exc}")
        return False


def load_wav(path):
    with wave.open(str(path), "rb") as w:
        if w.getframerate() != RATE or w.getnchannels() != 1 or w.getsampwidth() != 2:
            raise SystemExit(f"{path} must be 16 kHz mono 16-bit (got "
                             f"{w.getframerate()} Hz, {w.getnchannels()} ch, "
                             f"{w.getsampwidth()*8} bit)")
        return w.readframes(w.getnframes())


def make_tone(freq, seconds, amp=0.3):
    out = bytearray()
    for i in range(int(RATE * seconds)):
        out += struct.pack("<h", int(amp * 32767 * math.sin(2 * math.pi * freq * i / RATE)))
    return bytes(out)


def send_downlink(sock, target, pcm):
    enc = opuslib.Encoder(RATE, 1, "voip")
    enc.bitrate = 24000
    seq = 0
    sent = 0
    for off in range(0, len(pcm) - FRAME * 2 + 1, FRAME * 2):
        chunk = pcm[off:off + FRAME * 2]
        packet = enc.encode(chunk, FRAME)
        sock.sendto(frame(T_AUDIO_DOWN, packet, seq), target)
        seq += 1
        sent += len(chunk)
        time.sleep(0.02)          # real-time pacing, the board buffers ~200 ms
    print(f"[duplex] sent {seq} Opus frames ({sent / 2 / RATE:.2f} s) to the board")


def record(sock, seconds, out_path, target):
    dec = opuslib.Decoder(RATE, BOARD_CH)
    pcm = bytearray()
    frames = 0
    deadline = time.time() + seconds
    last_keepalive = time.time()
    while time.time() < deadline:
        # The board forgets a peer that goes quiet for 10 s, so keep it alive.
        if time.time() - last_keepalive > 2.0:
            sock.sendto(frame(T_CTRL, b"volume:70"), target)
            last_keepalive = time.time()
        try:
            data, _addr = sock.recvfrom(2048)
        except socket.timeout:
            continue
        if len(data) < HEADER.size:
            continue
        magic, ftype, _f, _seq, ln, _ts = HEADER.unpack_from(data)
        if magic != MAGIC or ftype != T_AUDIO_UP:
            continue
        try:
            pcm += dec.decode(data[HEADER.size:HEADER.size + ln], FRAME)
        except Exception as exc:
            print(f"[duplex] decode error: {exc}")
            continue
        frames += 1
    secs = len(pcm) / 2 / BOARD_CH / RATE
    print(f"[duplex] received {frames} frames, {secs:.2f} s of stereo audio")
    if not pcm:
        return False
    # store as stereo 16 kHz
    with wave.open(str(out_path), "wb") as w:
        w.setnchannels(BOARD_CH)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(pcm))
    print(f"[duplex] saved {out_path}")
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="172.16.30.145")
    ap.add_argument("--port", type=int, default=5000)
    ap.add_argument("--tone", type=float, default=None, help="beep frequency for downlink")
    ap.add_argument("--wav", default=None, help="16 kHz mono WAV for downlink")
    ap.add_argument("--listen", type=float, default=0, help="seconds of uplink to record")
    ap.add_argument("--play", action="store_true", help="play the recording on the PC")
    ap.add_argument("--out", default="uplink.wav")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    sock.settimeout(0.5)
    target = (args.host, args.port)

    # Register with the board (it learns the peer from any valid datagram).
    sock.sendto(frame(T_CTRL, b"volume:70"), target)

    if args.tone is not None:
        send_downlink(sock, target, make_tone(args.tone, 2.0))
    elif args.wav:
        send_downlink(sock, target, load_wav(args.wav))

    if args.listen > 0:
        sock.sendto(frame(T_CTRL, b"volume:70"), target)
        print(f"[duplex] recording {args.listen:.0f} s of uplink - press and hold BOOT ...",
              flush=True)
        if record(sock, args.listen, args.out, target) and args.play:
            play_wav(args.out)

    sock.close()


if __name__ == "__main__":
    main()
