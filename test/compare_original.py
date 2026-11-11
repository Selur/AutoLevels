#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compares the port with the original AutoLevels v0.12 Beta 3 DLL inside AviSynth+ (Windows).

Every clip is an 8 bit y4m file (decode once with ffmpeg), read with BestSource in both scripts, so both filters see
identical frames. For each colorspace and parameter set the MD5 of every output frame must match.

Usage: python compare_original.py --original AutoLevels_x64.dll --plugin build/AutoLevels.dll --work DIR CLIP.y4m...
"""
import argparse
import concurrent.futures
import hashlib
import os
import subprocess
import sys

AVS2YUV = r"F:/Hybrid/64bit/Avisynth/avs2yuv64.exe"
BESTSOURCE = r"F:/Hybrid/64bit/Avisynth/avisynthPlugins/BestSource.dll"

# colorspace -> (conversion, bytes per output frame as a function of width and height)
# avs2yuv cannot write YV411, so that output is upsampled losslessly (point) to YV24 after the filter.
POST = {"YV411": '.ConvertToYV24(chromaresample="point")', "YUY2": ".ConvertToYV16()",
        "RGB24": "\nStackVertical(ExtractR(), ExtractG(), ExtractB())",
        # The original also maps the RGB32 alpha channel through the levels curve; the port leaves alpha unchanged.
        "RGB32": "\nStackVertical(ExtractR(), ExtractG(), ExtractB())"}
FORMATS = {
    "YV12": ("", lambda w, h: w * h * 3 // 2),
    "YV16": ("ConvertToYV16()", lambda w, h: w * h * 2),
    "YV24": ("ConvertToYV24()", lambda w, h: w * h * 3),
    "Y8": ("ConvertToY8()", lambda w, h: w * h),
    "YV411": ("ConvertToYV411()", lambda w, h: w * h * 3),
    "YUY2": ("ConvertToYUY2()", lambda w, h: w * h * 2),
    "RGB24": ("ConvertToRGB24()", lambda w, h: w * h * 3),
    "RGB32": ("ConvertToRGB32()", lambda w, h: w * h * 3),
}

PARAMS = [
    "",
    "debug=true",
    "autogamma=true",
    "autogamma=true, midpoint=0.42, debug=true",
    "coring=true",
    "coring=true, autogamma=true",
    "input_low=20, input_high=220",
    "input_low=20",
    "output_low=0, output_high=255",
    "output_low=10, output_high=200, gamma=1.4",
    "gamma=0.7",
    "ignore=0.02",
    "ignore_low=0.01, ignore_high=0.05",
    "border=16",
    "border_l=8, border_r=24, border_t=4, border_b=30, debug=true",
    "filterRadius=0",
    "filterRadius=3, sceneChgThresh=10",
    "sceneChgThresh=255",
    "sc2th=256",
    "sc2th=4, sc2Perc=10.0, debug=true",
    "minRng=0",
    "minRng=128, debug=true",
    "gammax=3.0, gammin=0.5, autogamma=true",
    'frameOverrides="E10-20,S50,N60-80,S120"',
    "autolevel=false, autogamma=true",
    "autolevel=false, debug=true",
]
# AutoGamma() with these
GAMMA_PARAMS = ["", "debug=true", "midpoint=0.4, gammax=2.0"]

# parameter sets run on every colorspace; the rest on YV12 only
ALL_FORMAT_PARAMS = {"", "debug=true", "autogamma=true, midpoint=0.42, debug=true", "coring=true",
                     "output_low=10, output_high=200, gamma=1.4", "border=16",
                     "border_l=8, border_r=24, border_t=4, border_b=30, debug=true"}


def fwd(p):
    return os.path.abspath(p).replace("\\", "/")


def run(script_path, text, frame_size):
    with open(script_path, "w", encoding="utf-8") as f:
        f.write(text)
    p = subprocess.run([AVS2YUV, "-nstdr", "-raw", script_path, "-o", "-"], capture_output=True)
    data = p.stdout
    if p.returncode != 0 or not data or len(data) % frame_size:
        raise RuntimeError("%s: rc=%d len=%d %s" % (script_path, p.returncode, len(data),
                                                     p.stderr.decode(errors="replace").strip()[-600:]))
    return [hashlib.md5(data[i:i + frame_size]).hexdigest() for i in range(0, len(data), frame_size)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--original", required=True)
    ap.add_argument("--plugin", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--width", type=int, default=480)
    ap.add_argument("--height", type=int, default=360)
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("clips", nargs="+")
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)

    cases = []
    for ci, clip in enumerate(a.clips):
        for fmt, (conv, size) in FORMATS.items():
            for func, plist in (("AutoLevels", PARAMS), ("AutoGamma", GAMMA_PARAMS)):
                for prm in plist:
                    if fmt != "YV12" and prm not in ALL_FORMAT_PARAMS:
                        continue
                    if prm.startswith("coring") and fmt.startswith("RGB"):
                        continue
                    cases.append((ci, clip, fmt, conv, size(a.width, a.height), func, prm))

    def one(idx_case):
        idx, (ci, clip, fmt, conv, fsize, func, prm) = idx_case
        body = 'BSVideoSource("%s")\n%s\n%s(%s)%s\n' % (fwd(clip), conv, func, prm, POST.get(fmt, ""))
        orig = run(os.path.join(a.work, "o%04d.avs" % idx),
                   'LoadPlugin("%s")\nLoadPlugin("%s")\n%s' % (BESTSOURCE, fwd(a.original), body), fsize)
        port = run(os.path.join(a.work, "p%04d.avs" % idx),
                   'LoadPlugin("%s")\nLoadPlugin("%s")\n%s' % (BESTSOURCE, fwd(a.plugin), body), fsize)
        diff = [i for i, (x, y) in enumerate(zip(orig, port)) if x != y]
        return ci, fmt, func, prm, len(orig), len(port), diff

    failures = 0
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        for ci, fmt, func, prm, no, np_, diff in ex.map(one, enumerate(cases)):
            ok = not diff and no == np_
            failures += not ok
            print("%s clip%d %-6s %s(%s): %d frames%s" % ("ok  " if ok else "FAIL", ci, fmt, func, prm, no,
                                                          "" if ok else " differ at %s" % diff[:10]))
    print("%d of %d cases differ" % (failures, len(cases)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
