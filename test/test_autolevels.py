#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Functional tests for the AutoLevels VapourSynth plugin (API 4); run in CI on every platform.

Usage: python test/test_autolevels.py [path/to/AutoLevels plugin]

Without an argument the plugin is expected to be autoloaded (e.g. from the installed wheel).
Bit-exactness against the original AviSynth plugin is checked by compare_original.py (Windows only).
"""
import ctypes
import sys

import vapoursynth as vs

core = vs.core
FAILURES = []
W, H, LENGTH, CUT = 96, 64, 24, 12


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)
        print("FAIL: " + msg)
        return
    print("ok  ", msg)


def expect_error(fn, msg, contains):
    try:
        fn()
    except vs.Error as e:
        check(contains in str(e), "%s (%s)" % (msg, str(e).strip().splitlines()[-1]))
    else:
        check(False, "no error raised: " + msg)


def pattern(w, h, n, p):
    """Low-contrast gradient plus deterministic noise; two scenes with different levels."""
    scene = 0 if n < CUT else 1
    lo, span = ((60, 90), (110, 50))[scene]
    if p > 0:
        lo, span = ((118, 20), (126, 12))[scene][p - 1], 12
    rows = []
    state = (w * 7919 + h * 104729 + n * 1299709 + p * 15485863) & 0xFFFFFFFF
    for y in range(h):
        row = bytearray(w)
        for x in range(w):
            state = (state * 1103515245 + 12345) & 0x7FFFFFFF
            noise = (state >> 16) % 7 - 3
            base = lo + ((x * span) // w + (y * span) // (2 * h) + n) % (span + 1)
            row[x] = max(0, min(255, base + noise))
        rows.append(bytes(row))
    return rows


def pattern_clip(fmt=vs.YUV420P8):
    """8 bit pattern clip; other formats are made from it with resize.Point."""
    blank = core.std.BlankClip(width=W, height=H, format=vs.YUV420P8, length=LENGTH)

    def fill(n, f):
        fout = f.copy()
        for p in range(3):
            pw, ph = (W, H) if p == 0 else (W // 2, H // 2)
            ptr = fout.get_write_ptr(p)
            ptr = ptr.value if hasattr(ptr, "value") else ptr
            stride = fout.get_stride(p)
            for y, row in enumerate(pattern(pw, ph, n, p)):
                ctypes.memmove(ptr + y * stride, row, pw)
        return fout

    clip = core.std.ModifyFrame(blank, blank, fill)
    if fmt != vs.YUV420P8:
        clip = core.resize.Point(clip, format=fmt)
    return clip


def samples(clip, p, n=0):
    """Plane p of frame n as a list of sample values."""
    f = clip.get_frame(n)
    data = bytes(f[p])
    if f.format.bytes_per_sample == 2:
        return list(memoryview(data).cast("H"))
    return list(data)


def frames(clip, order):
    return {n: tuple(bytes(clip.get_frame(n)[p]) for p in range(clip.format.num_planes)) for n in order}


def main():
    if len(sys.argv) > 1:
        core.std.LoadPlugin(sys.argv[1])
    check(hasattr(core, "autolevels") and hasattr(core.autolevels, "AutoLevels")
          and hasattr(core.autolevels, "AutoGamma"), "plugin loaded (namespace autolevels)")
    al = core.autolevels.AutoLevels
    ag = core.autolevels.AutoGamma
    src = pattern_clip()

    out = al(src)
    check(out.format.id == src.format.id and out.width == W and out.height == H and out.num_frames == LENGTH,
          "format, size and length kept")
    y_in, y_out = samples(src, 0), samples(out, 0)
    check(min(y_out) <= 30 and max(y_out) >= 215 and max(y_out) - min(y_out) > max(y_in) - min(y_in) + 60,
          "luma range stretched towards 16..235 (%d-%d -> %d-%d)" % (min(y_in), max(y_in), min(y_out), max(y_out)))
    check(max(samples(out, 0)) <= 235 and min(samples(out, 0)) >= 16, "default output stays within 16..235")
    full = al(src, output_low=0, output_high=255)
    check(min(samples(full, 0)) < 16 and max(samples(full, 0)) > 235, "output_low/output_high widen the output")
    c_in, c_out = samples(src, 1), samples(out, 1)
    check(max(c_out) - min(c_out) >= max(c_in) - min(c_in), "chroma is scaled with the luma gain")

    # frames outside the scene of the current frame must not influence it: frame CUT - 1 only sees scene 0
    short = al(src, filterRadius=0)
    check(samples(al(src), 0, CUT - 1) != samples(al(src, sceneChgThresh=255, sc2th=256), 0, CUT - 1),
          "scene change limits the averaging window")
    check(samples(al(src, frameOverrides="N%d" % CUT), 0, CUT - 1) == samples(
        al(src, sceneChgThresh=255, sc2th=256), 0, CUT - 1), "N override removes the scene change")
    check(samples(short, 0, 3) != samples(al(src), 0, 3), "filterRadius=0 uses the frame alone")
    excl = al(src, frameOverrides="E2-4,E7")
    check(all(frames(excl, [n])[n] == frames(src, [n])[n] for n in (2, 3, 4, 7)), "E override returns the source")
    check(frames(excl, [5])[5] == frames(out, [5])[5], "frames outside E ranges are processed")
    expect_error(lambda: al(src, frameOverrides="X5"), "bogus override", "bogus frameOverrides specifier")

    g = ag(src)
    mean_in = sum(samples(src, 0)) / (W * H)
    mean_g = sum(samples(g, 0)) / (W * H)
    check(mean_g > mean_in + 5, "AutoGamma brightens a dark clip (mean %.1f -> %.1f)" % (mean_in, mean_g))
    check(frames(al(src, autolevel=False, autogamma=True, gammax=10.0), range(LENGTH)) == frames(g, range(LENGTH)),
          "AutoGamma() equals AutoLevels(autolevel=False, autogamma=True, gammax=10)")
    check(samples(al(src, input_low=0, input_high=255, output_low=0, output_high=255), 0) == samples(src, 0),
          "fixed identity levels leave luma unchanged")

    big = core.resize.Point(src, 640, 480)
    dbg, plain = al(big, debug=True), al(big)
    check(samples(dbg, 0, 3)[:640 * 100] != samples(plain, 0, 3)[:640 * 100]
          and samples(dbg, 0, 3)[640 * 100:] == samples(plain, 0, 3)[640 * 100:], "debug draws text in the top 100 rows")

    # results must not depend on the order in which frames are requested
    a = frames(al(src, autogamma=True, debug=True), range(LENGTH))
    b = frames(al(src, autogamma=True, debug=True), reversed(range(LENGTH)))
    check(a == b, "request order does not change the result")
    reqs = [al(src).get_frame_async(n) for n in range(LENGTH)]
    par = {n: tuple(bytes(f.result()[p]) for p in range(3)) for n, f in enumerate(reqs)}
    check(par == frames(al(src), range(LENGTH)), "parallel requests give the same result")

    # formats
    for fmt in (vs.GRAY8, vs.YUV444P8, vs.YUV422P8, vs.YUV410P8, vs.YUV411P8, vs.RGB24, vs.YUV420P10, vs.GRAY16,
                vs.RGB48):
        if fmt in (vs.GRAY8, vs.GRAY16):
            clip = core.std.ShufflePlanes(pattern_clip(vs.YUV420P8 if fmt == vs.GRAY8 else vs.YUV420P16), 0, vs.GRAY)
        elif fmt in (vs.RGB24, vs.RGB48):
            clip = core.resize.Point(src, format=fmt, matrix_in_s="709")
        else:
            clip = pattern_clip(fmt)
        try:
            r = al(clip, debug=True)
            r.get_frame(CUT)
            fmt_name = core.get_video_format(fmt).name
            check(r.format.id == clip.format.id and samples(r, 0, CUT) != samples(clip, 0, CUT),
                  "%s processed (debug)" % fmt_name)
        except vs.Error as e:
            check(False, "%s: %s" % (core.get_video_format(fmt).name, e))

    # High bit depth: a 10/16 bit copy of the 8 bit clip gives the 8 bit result. With fixed input levels luma
    # differs only by the 8 bit output rounding; measured levels are rounded to whole 8 bit levels (times the gain)
    # in the 8 bit path, and its chroma division truncates, so those cases get a wider tolerance.
    for fmt, shift in ((vs.YUV420P16, 8), (vs.YUV420P10, 2)):
        for kw, planes, tol in (({"input_low": 20, "input_high": 200}, (0,), 0.5),
                                ({}, (0, 1, 2), 3.0), ({"autogamma": True}, (0, 1, 2), 3.0),
                                ({"coring": True}, (0, 1, 2), 3.0), ({"output_low": 0, "output_high": 255}, (0, 1, 2), 3.0)):
            r8 = al(src, **kw)
            rh = al(pattern_clip(fmt), **kw)
            worst = 0
            for n in (0, CUT - 1, CUT, LENGTH - 1):
                for p in planes:
                    s8, sh = samples(r8, p, n), samples(rh, p, n)
                    worst = max(worst, max(abs(h / (1 << shift) - e) for e, h in zip(s8, sh)))
            check(worst <= tol, "%s %s matches 8 bit within %.2f" % (core.get_video_format(fmt).name, kw, worst))

    expect_error(lambda: al(core.resize.Point(src, format=vs.YUV420PS)), "float rejected", "8-16 bit integer")
    expect_error(lambda: al(src, gamma=0), "gamma range", "gamma must be > 0.0")
    expect_error(lambda: al(src, midpoint=1.0), "midpoint range", "midpoint must be less than 1.0")
    expect_error(lambda: al(src, input_low=100, input_high=50), "input order", "input_low must be smaller")
    expect_error(lambda: al(src, output_high=300), "output range", "output_high is not in range")
    expect_error(lambda: al(src, ignore=0.5), "ignore range", "ignore_low fraction is very large")
    expect_error(lambda: al(src, border=48), "border size", "left+right border")
    expect_error(lambda: al(src, minRng=50), "minRng range", "minRng")
    expect_error(lambda: al(src, gammax=0.5), "gammax range", "gammax")
    expect_error(lambda: al(src, filterRadius=-1), "filterRadius range", "filterRadius must be >= 0")

    if FAILURES:
        print("%d failure(s)" % len(FAILURES))
        sys.exit(1)
    print("all tests passed")


if __name__ == "__main__":
    main()
