#!/usr/bin/env python3
import argparse
import pathlib
import re
import subprocess
import sys


SAMPLE_RATES = {
    0: 96000,
    1: 88200,
    2: 64000,
    3: 48000,
    4: 44100,
    5: 32000,
    6: 24000,
    7: 22050,
    8: 16000,
    9: 12000,
    10: 11025,
    11: 8000,
    12: 7350,
}


def read_config_value(config_path, name):
    text = config_path.read_text(encoding="utf-8")
    match = re.search(rf"constexpr\s+[^=]+\s+{name}\s*=\s*([^;]+);", text)
    if not match:
        raise ValueError(f"missing config value: {name}")
    expr = match.group(1)
    if not re.fullmatch(r"[0-9\s()+\-*/ULul]+", expr):
        raise ValueError(f"unsupported expression for {name}: {expr}")
    return int(eval(expr, {"__builtins__": {}}, {}))


def jpeg_size(frame):
    i = 2
    while i + 9 < len(frame):
        if frame[i] != 0xFF:
            i += 1
            continue
        marker = frame[i + 1]
        i += 2
        if marker in (0xD8, 0xD9):
            continue
        if i + 2 > len(frame):
            break
        segment_len = int.from_bytes(frame[i:i + 2], "big")
        if marker in (0xC0, 0xC1, 0xC2, 0xC3):
            h = int.from_bytes(frame[i + 3:i + 5], "big")
            w = int.from_bytes(frame[i + 5:i + 7], "big")
            return w, h
        i += segment_len
    return None


def split_mjpeg(path):
    data = path.read_bytes()
    frames = []
    pos = 0
    while True:
        start = data.find(b"\xff\xd8", pos)
        if start < 0:
            break
        end = data.find(b"\xff\xd9", start + 2)
        if end < 0:
            raise ValueError(f"truncated JPEG frame after offset {start}")
        frames.append(data[start:end + 2])
        pos = end + 2
    if not frames:
        raise ValueError("no JPEG frames found")
    return frames


def parse_adts(path):
    data = path.read_bytes()
    pos = 0
    frames = 0
    total_samples = 0
    sample_rate = None
    channel_config = None
    while pos + 7 <= len(data):
        if data[pos] != 0xFF or (data[pos + 1] & 0xF0) != 0xF0:
            raise ValueError(f"ADTS sync lost at offset {pos}")
        protection_absent = data[pos + 1] & 0x01
        sf_index = (data[pos + 2] & 0x3C) >> 2
        sr = SAMPLE_RATES.get(sf_index)
        if not sr:
            raise ValueError(f"unsupported AAC sample-rate index {sf_index}")
        ch = ((data[pos + 2] & 0x01) << 2) | ((data[pos + 3] & 0xC0) >> 6)
        frame_len = ((data[pos + 3] & 0x03) << 11) | (data[pos + 4] << 3) | ((data[pos + 5] & 0xE0) >> 5)
        header_len = 7 if protection_absent else 9
        if frame_len < header_len or pos + frame_len > len(data):
            raise ValueError(f"invalid ADTS frame length at offset {pos}")
        blocks = data[pos + 6] & 0x03
        frames += 1
        total_samples += 1024 * (blocks + 1)
        sample_rate = sr if sample_rate is None else sample_rate
        channel_config = ch if channel_config is None else channel_config
        if sample_rate != sr:
            raise ValueError("AAC sample rate changes inside file")
        if channel_config != ch:
            raise ValueError("AAC channel config changes inside file")
        pos += frame_len
    if pos != len(data):
        raise ValueError(f"trailing AAC bytes: {len(data) - pos}")
    if frames == 0:
        raise ValueError("no ADTS frames found")
    return frames, sample_rate, channel_config, total_samples / sample_rate


def run_ffmpeg(ffmpeg, media_path, fmt):
    if not ffmpeg:
        return
    cmd = [str(ffmpeg), "-v", "error", "-f", fmt, "-i", str(media_path), "-f", "null", "-"]
    subprocess.run(cmd, check=True)


def main():
    parser = argparse.ArgumentParser(description="Validate Macintosh Mini SD media files.")
    parser.add_argument("sdcard", type=pathlib.Path)
    parser.add_argument("--name", required=True)
    parser.add_argument("--ffmpeg", type=pathlib.Path, default=None)
    parser.add_argument("--config", type=pathlib.Path, default=pathlib.Path("firmware/include/config.h"))
    args = parser.parse_args()

    mjpeg = args.sdcard / f"{args.name}.mjpeg"
    aac = args.sdcard / f"{args.name}.aac"
    if not mjpeg.exists() or not aac.exists():
        print(f"missing pair: {mjpeg} / {aac}", file=sys.stderr)
        return 2

    frame_capacity = read_config_value(args.config, "JpegFrameBufferSize")
    expected_fps = read_config_value(args.config, "VideoFps")
    frames = split_mjpeg(mjpeg)
    sizes = [len(frame) for frame in frames]
    dimensions = [jpeg_size(frame) for frame in frames]
    too_large = [size for size in sizes if size > frame_capacity]
    if too_large:
        print(f"FAIL: {len(too_large)} JPEG frames exceed {frame_capacity} bytes", file=sys.stderr)
        return 1
    if any(dim != (240, 240) for dim in dimensions):
        print(f"FAIL: unexpected JPEG dimensions: {sorted(set(dimensions))}", file=sys.stderr)
        return 1

    aac_frames, sample_rate, channels, audio_seconds = parse_adts(aac)
    video_seconds = len(frames) / expected_fps

    run_ffmpeg(args.ffmpeg, mjpeg, "mjpeg")
    run_ffmpeg(args.ffmpeg, aac, "aac")

    print("SD media validation OK")
    print(f"video_frames={len(frames)} video_seconds={video_seconds:.3f} max_jpeg={max(sizes)} frame_buffer={frame_capacity}")
    print(f"jpeg_dimensions={sorted(set(dimensions))}")
    print(f"aac_frames={aac_frames} audio_seconds={audio_seconds:.3f} sample_rate={sample_rate} channels={channels}")
    print(f"av_duration_delta={abs(audio_seconds - video_seconds):.3f}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
