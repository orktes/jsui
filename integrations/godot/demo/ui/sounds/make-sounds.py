#!/usr/bin/env python3
"""Synthesizes the demo's UI sounds (CC0: generated here, no samples).

    python3 integrations/godot/demo/ui/sounds/make-sounds.py

Writes next to this script:
  tick.wav      hover / focus tick      (16-bit PCM, 22050 Hz mono, 40 ms)
  confirm.wav   press: a rising two-note chime (16-bit PCM, 160 ms)
  back.wav      long press: the chime falling (8-bit PCM, 160 ms)
  blip.mp3      a short blip, for the MP3 path (needs ffmpeg + libmp3lame)
  menu-loop.ogg a quiet 4 s pad that loops seamlessly (needs ffmpeg + libvorbis)

The WAVs need only Python; the MP3 / OGG are encoded from WAVs with ffmpeg
and skipped (with a note) without it. The outputs are committed, so builds
never run this.
"""
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile

RATE = 22050
HERE = os.path.dirname(os.path.abspath(__file__))


def write_wav(path, samples, bits=16):
    """Mono PCM WAV of float samples in -1..1."""
    if bits == 16:
        data = b"".join(struct.pack("<h", max(-32767, min(32767, int(round(s * 32767))))) for s in samples)
    else:  # 8-bit PCM is unsigned
        data = bytes(max(0, min(255, int(round(128 + s * 127)))) for s in samples)
    block = bits // 8
    pad = b"\0" * (len(data) & 1)  # RIFF chunks are word-aligned
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data) + len(pad)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * block, block, bits))
        f.write(b"data" + struct.pack("<I", len(data)) + data + pad)


def tone(freq, seconds, attack=0.004, decay=None, shape="sine", gain=0.5):
    n = int(RATE * seconds)
    out = []
    for i in range(n):
        t = i / RATE
        ph = (freq * t) % 1.0
        if shape == "tri":
            v = 4 * abs(ph - 0.5) - 1
        else:
            v = math.sin(2 * math.pi * ph)
        env = min(1.0, t / attack) if attack > 0 else 1.0
        env *= math.exp(-t / decay) if decay else 1.0
        env *= min(1.0, (seconds - t) / 0.004)  # no click at the end
        out.append(v * env * gain)
    return out


def mix(*parts):
    n = max(len(p) for p in parts)
    return [sum(p[i] for p in parts if i < len(p)) for i in range(n)]


def chime(f1, f2, bits, name):
    a = tone(f1, 0.08, decay=0.05, shape="tri", gain=0.45)
    b = [0.0] * int(RATE * 0.05) + tone(f2, 0.11, decay=0.06, shape="tri", gain=0.45)
    write_wav(os.path.join(HERE, name), mix(a, b), bits)


def pad(seconds=4.0):
    # Two chords, two seconds each; every partial completes whole cycles over
    # the loop, so its end meets its start without a click.
    n = int(RATE * seconds)
    chords = [[110.0, 164.5, 220.0, 277.0], [98.0, 146.75, 196.0, 247.0]]
    out = []
    for i in range(n):
        t = i / RATE
        half = int(t >= seconds / 2)
        # crossfade the chords around each boundary (also the loop point)
        x = (t % (seconds / 2)) / (seconds / 2)
        w = 0.5 - 0.5 * math.cos(2 * math.pi * x)  # 0 at boundaries, 1 mid-chord
        cur = sum(math.sin(2 * math.pi * f * t) for f in chords[half]) / 4
        out.append(0.22 * w * cur + 0.04 * math.sin(2 * math.pi * 55.0 * t))
    return out


def encode(wav, out, args):
    ff = shutil.which("ffmpeg")
    if not ff:
        print("ffmpeg not found: skipped", os.path.basename(out))
        return
    subprocess.run([ff, "-y", "-loglevel", "error", "-i", wav, *args, "-map_metadata", "-1", out], check=True)


def main():
    write_wav(os.path.join(HERE, "tick.wav"), tone(2400, 0.04, attack=0.001, decay=0.008, gain=0.35))
    chime(660, 990, 16, "confirm.wav")
    chime(880, 587, 8, "back.wav")
    with tempfile.TemporaryDirectory() as tmp:
        blip = os.path.join(tmp, "blip.wav")
        write_wav(blip, tone(1320, 0.12, decay=0.04, gain=0.4))
        encode(blip, os.path.join(HERE, "blip.mp3"), ["-c:a", "libmp3lame", "-b:a", "32k", "-ac", "1"])
        loop = os.path.join(tmp, "loop.wav")
        write_wav(loop, pad())
        encode(loop, os.path.join(HERE, "menu-loop.ogg"), ["-c:a", "libvorbis", "-q:a", "0", "-ac", "1"])
    for f in sorted(os.listdir(HERE)):
        if not f.endswith((".py", ".import")):
            print(f, os.path.getsize(os.path.join(HERE, f)), "bytes")


if __name__ == "__main__":
    sys.exit(main())
