#!/usr/bin/env python3
"""video2cvid - convert a video to CVID, the 1-bit video format played by /apps/vplay.

  python3 tools/video2cvid.py input.mp4 output.cvid [--size 160x120] [--fps 30] [--threshold 128]
  python3 tools/video2cvid.py --demo output.cvid        (procedural test clip, no ffmpeg needed)

Real videos are decoded with ffmpeg (on PATH, or the binary named by $FFMPEG), scaled to the target
size, converted to grayscale and thresholded to black / white (Bad Apple!! is already
black and white).

CVID layout (little endian):
  header (32 bytes): magic "CVID", u16 version (1), u16 width, u16 height, u16 fps,
                     u32 frame_count, u32 flags (0), u8[12] reserved
  frames: u32 byte_count, then the frame's XOR difference with the previous frame
          (all black before the first one) as alternating run lengths, LEB128 varints:
          unchanged pixels, changed pixels, unchanged, ... covering width*height pixels
          in row-major order. A still frame is a single run.
"""
import math
import os
import struct
import subprocess
import sys


def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def encode_frame(prev, cur):
    runs = bytearray()
    total = len(cur)
    i = 0
    state = 0  # 0 = counting unchanged pixels, 1 = changed
    while i < total:
        j = i
        while j < total and ((prev[j] != cur[j]) == bool(state)):
            j += 1
        runs += varint(j - i)
        state ^= 1
        i = j
    return runs


def write_cvid(path, width, height, fps, frames):
    data = bytearray()
    prev = bytes(width * height)
    count = 0
    for cur in frames:
        enc = encode_frame(prev, cur)
        data += struct.pack('<I', len(enc)) + enc
        prev = cur
        count += 1
    header = b'CVID' + struct.pack('<HHHHII', 1, width, height, fps, count, 0) + bytes(12)
    with open(path, 'wb') as f:
        f.write(header + data)
    print('video2cvid: %s: %dx%d, %d fps, %d frames (%.1f s), %d bytes (%.0f bytes/frame)' %
          (path, width, height, fps, count, count / fps, len(header) + len(data),
           len(data) / max(count, 1)))


def ffmpeg_frames(src, width, height, fps, threshold):
    ffmpeg = os.environ.get('FFMPEG', 'ffmpeg')  # e.g. the binary bundled with pip's imageio-ffmpeg
    cmd = [ffmpeg, '-loglevel', 'error', '-i', src, '-vf', 'scale=%d:%d,fps=%d' % (width, height, fps),
           '-f', 'rawvideo', '-pix_fmt', 'gray', '-']
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    size = width * height
    while True:
        raw = proc.stdout.read(size)
        if len(raw) < size:
            break
        yield bytes(1 if p >= threshold else 0 for p in raw)
    proc.wait()


def demo_frames(width, height, fps, seconds=12):
    """Procedural clip: a bouncing ball, a rotating bar and a growing ring (black and white)."""
    n = fps * seconds
    for k in range(n):
        t = k / fps
        frame = bytearray(width * height)
        bx = width / 2 + (width / 2 - 14) * math.sin(t * 1.7)
        by = height - 14 - abs(math.sin(t * 3.1)) * (height - 30)
        ring = (t * 30) % (max(width, height) * 0.8)
        ang = t * 2.0
        cx, cy = width / 2, height / 2
        ca, sa = math.cos(ang), math.sin(ang)
        for y in range(height):
            for x in range(width):
                on = (x - bx) ** 2 + (y - by) ** 2 <= 12 ** 2
                dx, dy = x - cx, y - cy
                d = math.hypot(dx, dy)
                on ^= abs(d - ring) < 2
                u = dx * ca + dy * sa
                v = -dx * sa + dy * ca
                on ^= abs(u) < 40 and abs(v) < 3
                frame[y * width + x] = 1 if on else 0
        yield bytes(frame)


def main(argv):
    width, height, fps, threshold = 160, 120, 30, 128
    args = []
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == '--size':
            width, height = map(int, argv[i + 1].lower().split('x'))
            i += 2
        elif a == '--fps':
            fps = int(argv[i + 1])
            i += 2
        elif a == '--threshold':
            threshold = int(argv[i + 1])
            i += 2
        else:
            args.append(a)
            i += 1
    if len(args) == 2 and args[0] == '--demo':
        write_cvid(args[1], width, height, fps, demo_frames(width, height, fps))
    elif len(args) == 2:
        write_cvid(args[1], width, height, fps, ffmpeg_frames(args[0], width, height, fps, threshold))
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
