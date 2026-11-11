#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compares VapourSynth and AviSynth+ output of the same AutoLevels build (Windows, run with a VapourSynth Python).

Both hosts read the same 8 bit y4m with BestSource; 10/16 bit inputs are made by bit shift in both hosts. Also checks
that AviSynth+ packed RGB24 and planar RGB give the same result.

Usage: python compare_hosts.py --plugin build/AutoLevels.dll --work DIR CLIP.y4m...
"""
import argparse
import hashlib
import os
import subprocess
import sys

import vapoursynth as vs

AVS2YUV = r"F:/Hybrid/64bit/Avisynth/avs2yuv64.exe"
AVS_BS = r"F:/Hybrid/64bit/Avisynth/avisynthPlugins/BestSource.dll"
VS_BS = r"F:/Hybrid/64bit/Vapoursynth/Lib/site-packages/vapoursynth/plugins2/libbestsource.dll"

PARAMS = ["", "debug=true", "autogamma=true, debug=true", "coring=true", "border=16, debug=true",
          "output_low=0, output_high=255, gamma=1.3", 'frameOverrides="E10-20,S50,N60-80"']
# (AviSynth conversion, VapourSynth format, avs2yuv -depth)
FORMATS = {
    "YUV420P8": ("", vs.YUV420P8, 8),
    "Y8": ("ConvertToY8()", vs.GRAY8, 8),
    "YUV420P10": ("ConvertBits(10)", vs.YUV420P10, 10),
    "YUV420P16": ("ConvertBits(16)", vs.YUV420P16, 16),
    "Y16": ("ConvertToY8().ConvertBits(16)", vs.GRAY16, 16),
}


def fwd(p):
    return os.path.abspath(p).replace("\\", "/")


def avs_frames(path, text, depth):
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    p = subprocess.run([AVS2YUV, "-nstdr", "-raw", path, "-o", "-"], capture_output=True)
    if p.returncode != 0 or not p.stdout:
        raise RuntimeError("%s: %s" % (path, p.stderr.decode(errors="replace")[-400:]))
    return p.stdout


def vs_bytes(clip):
    out = bytearray()
    for f in clip.frames():
        for p in range(f.format.num_planes):
            out += bytes(f[p])  # plane rows without stride padding
    return bytes(out)


def args_vs(prm):
    kw = {}
    if not prm:
        return kw
    for part in prm.split(", "):
        k, v = part.split("=", 1)
        if v in ("true", "false"):
            kw[k] = v == "true"
        elif v.startswith('"'):
            kw[k] = v.strip('"')
        else:
            kw[k] = float(v) if "." in v else int(v)
    return kw


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--plugin", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("clips", nargs="+")
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)
    core = vs.core
    core.std.LoadPlugin(VS_BS)
    core.std.LoadPlugin(os.path.abspath(a.plugin))
    failures = total = 0
    idx = 0
    for ci, clip in enumerate(a.clips):
        src = core.bs.VideoSource(clip)
        for name, (conv, vfmt, depth) in FORMATS.items():
            # (no chroma upsampling here: the two hosts' resizers place chroma differently)
            vsrc = core.std.ShufflePlanes(src, 0, vs.GRAY) if vfmt in (vs.GRAY8, vs.GRAY16) else src
            if vfmt != vsrc.format.id:
                vsrc = core.resize.Point(vsrc, format=vfmt)
            for prm in PARAMS:
                idx += 1
                total += 1
                avs = avs_frames(os.path.join(a.work, "h%04d.avs" % idx), 'LoadPlugin("%s")\nLoadPlugin("%s")\n'
                                 'BSVideoSource("%s")\n%s\nAutoLevels(%s)\n' % (AVS_BS, fwd(a.plugin), fwd(clip),
                                                                               conv, prm), depth)
                vsb = vs_bytes(core.autolevels.AutoLevels(vsrc, **args_vs(prm)))
                ok = hashlib.md5(avs).digest() == hashlib.md5(vsb).digest()
                failures += not ok
                print("%s clip%d %-10s AutoLevels(%s)%s" % ("ok  " if ok else "FAIL", ci, name, prm,
                                                            "" if ok else " (%d vs %d bytes)" % (len(avs), len(vsb))))
        # AviSynth+ packed RGB24 against planar RGB of the same pixels
        for prm in PARAMS:
            if prm.startswith("coring"):
                continue
            idx += 1
            total += 1
            head = 'LoadPlugin("%s")\nLoadPlugin("%s")\nBSVideoSource("%s").ConvertToRGB24()\n' % (
                AVS_BS, fwd(a.plugin), fwd(clip))
            ext = "\nStackVertical(ExtractR(), ExtractG(), ExtractB())\n"
            packed = avs_frames(os.path.join(a.work, "r%04d.avs" % idx), head + "AutoLevels(%s)%s" % (prm, ext), 8)
            planar = avs_frames(os.path.join(a.work, "q%04d.avs" % idx),
                                head + "ConvertToPlanarRGB()\nAutoLevels(%s)%s" % (prm, ext), 8)
            ok = packed == planar
            failures += not ok
            print("%s clip%d RGB24 vs RGBP8 AutoLevels(%s)" % ("ok  " if ok else "FAIL", ci, prm))
    print("%d of %d cases differ" % (failures, total))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
