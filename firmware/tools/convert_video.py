#!/usr/bin/env python3
import argparse
import pathlib
import shutil
import subprocess
import sys


def run(cmd):
    print(" ".join(str(part) for part in cmd))
    subprocess.run(cmd, check=True)


def main():
    parser = argparse.ArgumentParser(description="Convert a video into Macintosh Mini MJPEG + AAC files.")
    parser.add_argument("input", type=pathlib.Path)
    parser.add_argument("--out", type=pathlib.Path, default=pathlib.Path("sdcard"))
    parser.add_argument("--name", default=None)
    parser.add_argument("--fps", type=int, default=25)  # match cfg::VideoFps
    parser.add_argument("--size", default="240:240")
    parser.add_argument("--video-quality", type=int, default=9)
    parser.add_argument("--audio-bitrate", default="64k")
    parser.add_argument("--ffmpeg", type=pathlib.Path, default=None)
    args = parser.parse_args()

    if not args.input.exists():
        print(f"missing input: {args.input}", file=sys.stderr)
        return 2
    ffmpeg = args.ffmpeg or shutil.which("ffmpeg")
    if not ffmpeg:
      print("missing ffmpeg: install ffmpeg or pass --ffmpeg /path/to/ffmpeg", file=sys.stderr)
      return 2

    args.out.mkdir(parents=True, exist_ok=True)
    stem = args.name or args.input.stem
    mjpeg = args.out / f"{stem}.mjpeg"
    aac = args.out / f"{stem}.aac"

    vf = (
        f"fps={args.fps},"
        f"scale={args.size}:force_original_aspect_ratio=decrease,"
        "pad=240:240:(ow-iw)/2:(oh-ih)/2:black,"
        "format=yuvj420p"
    )
    run([
        ffmpeg, "-y", "-i", args.input,
        "-an", "-vf", vf,
        "-c:v", "mjpeg", "-q:v", str(args.video_quality),
        "-f", "mjpeg", mjpeg,
    ])
    run([
        ffmpeg, "-y", "-i", args.input,
        "-vn", "-ac", "1", "-ar", "44100",
        "-c:a", "aac", "-b:a", args.audio_bitrate,
        "-f", "adts", aac,
    ])
    print(f"wrote {mjpeg}")
    print(f"wrote {aac}")


if __name__ == "__main__":
    raise SystemExit(main())
