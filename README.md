# AutoLevels

Temporally smoothed automatic levels and gamma adjustment, as one plugin binary for **AviSynth+** and
**VapourSynth** (API 4), built for Windows x64, Linux x86_64 and macOS arm64.

This is a port of AutoLevels v0.12 Beta 3 (2019-07-11):

- Revisions 0.1–0.3 by Theodor Anschütz, Copyright 2007
- Revisions 0.4–0.6 by Jim Battle
- Revisions 0.7–0.12 by StainlessS
- Debug text uses the DDigit font and renderer by StainlessS

With 8 bit input the output is bit-identical to the original `AutoLevels_x64.dll` (see [Differences](#differences-to-v012-beta-3)
for the few intended changes). New: VapourSynth support, 10–16 bit integer formats, planar RGB, multithreading,
Linux and macOS builds.

## Function

Sometimes a video source needs to be adjusted because it is too dark, too bright, the contrast is too high or
too low, or the gamma is wrong. The histogram of such a video often shows that its luma values use only part of the
possible range.

The AviSynth filter `Levels()` remaps a continuous range of values monotonically to another range:

    output = [(input - input_low) / (input_high - input_low)] ^ (1/gamma)
           * (output_high - output_low) + output_low

Luma values at or below `input_low` map to 0.0, values at or above `input_high` map to 1.0, everything between is
linearly interpolated. The intermediate value is raised to a power (0.0 and 1.0 stay where they are), then mapped
into the range `output_low` to `output_high`.

The shortcoming of `Levels()` is that the parameters must be set by hand, which is difficult when the input range
shifts during a video because of changing lighting. `ColorYUV(autogain=true)` measures every frame on its own and can
produce sudden, unpleasant jumps in brightness.

AutoLevels averages luma statistics over the frames around the current frame, stops the averaging at scene changes,
and derives `input_low`, `input_high` and optionally the gamma from these averages.

## Usage

AviSynth+:

```
LoadPlugin("AutoLevels.dll")   # or put it in the plugins folder for autoloading
AutoLevels()
AutoGamma()
```

VapourSynth:

```python
clip = core.autolevels.AutoLevels(clip)
clip = core.autolevels.AutoGamma(clip)
```

Both functions take the same parameters; they differ only in defaults. `AutoGamma()` has `autogamma=true`,
`autolevel=false` and `gammax=10.0`. In VapourSynth booleans are given as `True`/`False` (or 1/0) and parameter
names are case-sensitive as written below; AviSynth ignores case.

```
AutoLevels(clip,
           int    filterRadius   = 7,
           int    sceneChgThresh = 20,
           string frameOverrides = "",

           float  gamma          = 1.0,
           bool   autogamma      = false,
           float  midpoint       = 0.5,

           bool   autolevel      = true,

           int    input_low      = measured,
           int    input_high     = measured,
           int    output_low     = 16 for YUV (0 with coring), 0 for RGB,
           int    output_high    = 235 for YUV (255 with coring), 255 for RGB,

           bool   coring         = false,

           float  ignore         = 1.0/512,
           float  ignore_low     = ignore,
           float  ignore_high    = ignore,

           int    border = 0, int border_l = 0, int border_r = 0, int border_t = 0, int border_b = 0,

           bool   debug          = false,

           int    sc2th          = 12,
           float  sc2Perc        = 33.33,
           float  gammax         = 1.5,
           float  gammin         = 1.0/gammax,
           int    minRng         = 100)
```

All parameters are optional. The default is good for most of them.

## Parameters

* **filterRadius** — how many frames before and after the current frame are averaged. Default 7. The higher the
  frame rate, the larger this number can be. Must be >= 0; 0 uses the current frame alone.

* **sceneChgThresh** — scene change detection threshold, 0–255, default 20. A scene change is assumed when the
  minimum or maximum luma of consecutive frames changes by at least this amount, or their mean luma by at least half
  of it. The averaging never crosses a scene change. A smaller number makes the detection more sensitive, at the cost
  of false positives. 255 effectively disables the detection.

* **sc2th**, **sc2Perc** — second scene change test. A scene change found with `sceneChgThresh` is only accepted when
  more than `sc2Perc` percent (0.0 <= sc2Perc < 100.0, default 33.33) of the luma pixels differ by more than `sc2th`
  (0–256, default 12) from the previous frame. `sc2th=256` switches the second test off.

* **frameOverrides** — special handling of certain frames: a comma or space separated list; each entry is one of the
  letters S, N, E followed by a frame number or a range `start-end`:

  - `S<frame>` or `S<start-end>`: assume a scene start at these frames (overrides the detection)
  - `N<frame>` or `N<start-end>`: assume no scene start at these frames (overrides the detection)
  - `E<frame>` or `E<start-end>`: leave these frames unchanged

  Example: `frameOverrides="E8400-8405,S62908,S63315,N8527-8540"`

* **gamma** — manual gamma, the same as the gamma of `Levels()`. Logically applied after the histogram stretch.
  Default 1.0 (no effect). Ignored when `autogamma` is true.

* **autogamma** — estimate a gamma correction from the mean luma: the gamma moves the mean towards `midpoint`. The
  idea comes from Fred Weinhaus' ImageMagick autolevel script.

* **midpoint** — target of `autogamma`, 0.0 < midpoint < 1.0 (0.0 darkest, 1.0 brightest), default 0.5. A value
  around 0.4 is often more pleasing.

* **gammax**, **gammin** — limits of the automatic gamma: 1.0 <= gammax <= 10.0 limits upward corrections,
  0.1 <= gammin <= 1.0 downward ones. Defaults: `gammax` 1.5 for `AutoLevels()`, 10.0 for `AutoGamma()`;
  `gammin` 1.0/gammax.

* **autolevel** — perform the histogram stretch. Set it to false to use autogamma alone.

* **input_low**, **input_high** — the luma levels that become black and white. Normally measured; when given they
  win over the statistics. 0–255, `input_low` < `input_high`.

* **output_low**, **output_high** — the darkest and brightest level after the adjustment, 0–255. Defaults: 16 and
  235 for limited range YUV, 0 and 255 for RGB, for full range YUV (see [Range](#range)) and for YUV with
  `coring=true`.

* **coring** — as in `Levels()`: the 16..235 luma range is expanded to 0..255 before the mapping and compressed back
  afterwards; input and output levels refer to the expanded range. With measured input levels the front-end
  remapping is cancelled, so the measured low and high cutoffs still map to 0.0 and 1.0. Default false (unlike
  `Levels()`), ignored for RGB, an error for full range YUV.

* **ignore**, **ignore_low**, **ignore_high** — fraction of the darkest (low) and brightest (high) pixels ignored when
  measuring, so outliers do not dominate. `ignore` sets both, `ignore_low`/`ignore_high` set one tail. Default 1.0/512
  (about 0.2%), at most 0.45; more than a few percent clips badly.

* **border**, **border_l**, **border_r**, **border_t**, **border_b** — bands of pixels at the edges that are left out of
  the statistics (the whole image is still processed). `border` applies to all four sides; when both `border` and a
  side value are given, the larger one is used. Useful when the edges contain defects or brightness roll-off.

* **minRng** — fade protection: when the luma range of the current frame (ymax − ymin) is below `minRng`, all
  corrections are reduced linearly with the range, so fades to and from black are not stretched to full contrast.
  0 switches it off, otherwise 64–128, default 100.

* **debug** — draws statistics on the frame (see below).

### Debug display

```
n:from,to] LHG±sSF : sc2=x%
ymin=  y   ymin_avg =  a{  b}
ymax=  y   ymax_avg =  a{  b}
mean=  m   ymean_avg= a
Gamm=g     out_mean = o(o/max)
```

- `n` is the frame number, `from,to` the averaged frames (limited by scene changes).
- Flags, bright when set, grey otherwise: `L` input low adjusted, `H` input high adjusted, `G` gamma applied,
  `±` automatic gamma limited by `gammin`/`gammax`, `s` the first scene change test fired, `S` scene start at this
  frame (after the second test and overrides), `F` (orange) fade protection active.
- `sc2` is the share of changed pixels used by the second scene change test.
- `ymin`/`ymax`/`mean` are this frame's statistics, `*_avg` the averages; the values in braces are the levels
  actually used after fade protection. `out_mean` is the mean luma of the output.

At 10–16 bit the values are shown in the native sample range of the clip.

## Formats

| | AviSynth+ | VapourSynth |
|---|---|---|
| YUV planar 8–16 bit, any subsampling | YV12, YV16, YV24, YV411, YUV420P10, ... (+ alpha) | YUV420P8 ... YUV444P16 |
| Gray 8–16 bit | Y8 ... Y16 | GRAY8 ... GRAY16 |
| RGB | RGB24, RGB32, planar RGB(A) 8–16 bit | RGB24 ... RGB48 |
| YUY2 | yes | — |

Float formats and AviSynth's packed RGB48/RGB64 are not supported (use planar RGB).

RGB is measured on Rec.601 luma (`0.299 R + 0.587 G + 0.114 B`) and the levels curve is applied to R, G and B.
YUV chroma is scaled around neutral with the luma gain. Alpha planes are copied unchanged.

**High bit depth:** all level parameters (`input_*`, `output_*`, `sceneChgThresh`, `sc2th`, `minRng`) stay on the
8 bit scale 0–255 and are scaled to the clip: limited range YUV and Gray by bit shift (16 → 4096 at 16 bit),
RGB and full range YUV and Gray to the full sample range (255 → 65535). The statistics are measured at full precision, so a 16 bit result may
differ from the 8 bit result by a level or two where the 8 bit path rounds.

## Range

YUV and Gray frames are limited range (16–235) unless their frame properties say full range. Both property names are
read in both hosts, the host's own name first:

| Property | Full range | Limited range | Read first in |
|---|---|---|---|
| `_Range` | 1 | 0 | VapourSynth (R74 and later) |
| `_ColorRange` | 0 | 1 | AviSynth+ (and older VapourSynth scripts) |

A frame without either property counts as limited range, as in v0.12. Full range changes the defaults of
`output_low`/`output_high` to 0/255 and the high bit depth scaling of the level parameters (see [Formats](#formats));
given values still win, and `coring=true` fails. The range is read per frame. RGB is always full range. AviSynth+
reads frame properties from interface 8; older versions always see limited range.

## Multithreading

The filter is `MT_NICE_FILTER` in AviSynth+ and `fmParallel` in VapourSynth. Each frame's statistics and scene change
result are computed once and shared, so the output does not depend on the number of threads or the order in which
frames are requested.

## Examples

```
AutoLevels()
AutoLevels(filterRadius=8, sceneChgThresh=10, frameOverrides="E8400-8405,S62908,S63315,N8527-8540")
AutoLevels(debug=true)
AutoLevels(sceneChgThresh=255)               # effectively disables scene change logic
AutoLevels(autolevel=false, autogamma=true)  # autogamma only
AutoLevels(ignore=1.0/500.0, gamma=1.5)
AutoLevels(border=32)
AutoGamma()
```

```python
clip = core.autolevels.AutoLevels(clip, autogamma=True, midpoint=0.42, debug=True)
clip = core.autolevels.AutoLevels(clip, frameOverrides="E8400-8405,S62908")
```

## Differences to v0.12 Beta 3

- RGB32: the alpha channel is copied; v0.12 also passed it through the levels curve.
- Border check: v0.12 compared only the side values (`border_l + border_r`) with the frame size, not the combined
  value with `border`, so a large `border` slipped through and broke the statistics. The effective borders are checked.
- `filterRadius` < 0 is rejected (it made v0.12 divide by zero).
- Full range YUV and Gray frames (see [Range](#range)) get 0/255 output defaults; v0.12 assumed limited range for
  every YUV clip.
- The `minRng` error message gave 48 as the lower limit; the check has always been 64.
- An invalid `frameOverrides` letter names the offending entry in the error message.
- AviSynth 2.5/2.6 (non-plus) and 32 bit builds are no longer provided; the plugin needs AviSynth+ (frame properties
  are passed on from interface 8) or VapourSynth R55+ (API 4).

## Installation

Releases contain, per platform, a VapourSynth wheel (`pip install vapoursynth_autolevels-*.whl`, autoloaded) and a
zip with the bare plugin (`AutoLevels.dll`, `libAutoLevels.so`, `libAutoLevels.dylib`) for AviSynth+ or manual
VapourSynth installs.

## Building

```
meson setup build
ninja -C build
```

On Windows with MSVC: `meson setup build --vsenv -Db_vscrt=mt` (or run `build.bat`). A wheel: `pip install .`
(meson-python). The GitHub workflow builds and tests wheels for Windows x64, Linux x86_64 and macOS arm64 and
attaches them, together with the plugin zips, to a release for `v*` tags.

Tests: `test/test_autolevels.py` (VapourSynth, runs in CI), `test/compare_original.py` (AviSynth+ against the
original DLL, Windows), `test/compare_hosts.py` (VapourSynth against AviSynth+ output).

## License

GPL-2.0-or-later, see [LICENSE](LICENSE). The vendored AviSynth+ and VapourSynth headers carry their own licenses.

## Future ideas (from the original readme)

- an equalization mode, perhaps with more than one target distribution (uniform, gaussian)
- a more informative debug mode
- better scene change detection (histogram correlation, second order statistics)
- detecting scenes that should be kept dark
