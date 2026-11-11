// SPDX-License-Identifier: GPL-2.0-or-later
// AviSynth+ interface of AutoLevels: AutoLevels() and AutoGamma() with the arguments of v0.12 Beta 3.
#include <memory>
#include <stdexcept>
#include <string>

#include "avisynth.h"
#include "core/autolevels.hpp"
#include "version.hpp"

using namespace autolevels;

namespace {

class AutoLevelsAvs : public GenericVideoFilter {
 public:
  AutoLevelsAvs(PClip child, const Params& p, const Format& fmt, IScriptEnvironment* env)
      : GenericVideoFilter(child), fmt_(fmt) {
    try {
      env->CheckVersion(8);
      has_v8_ = true;
    } catch (const AvisynthError&) {
      has_v8_ = false;
    }
    try {
      engine_ = std::make_unique<Engine>(p, fmt_, vi.num_frames);
    } catch (const std::exception& e) {
      env->ThrowError("%s", e.what());
    }
  }

  PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env) override {
    n = n < 0 ? 0 : (n >= vi.num_frames ? vi.num_frames - 1 : n);
    PVideoFrame src = child->GetFrame(n, env);
    if (engine_->isExcluded(n)) return src;
    PVideoFrame dst = has_v8_ ? env->NewVideoFrameP(vi, &src) : env->NewVideoFrame(vi);
    const Fetch fetch = [this, env](int k) {
      auto frame = std::make_shared<PVideoFrame>(child->GetFrame(k, env));
      FrameRef r;
      r.frame = readFrame(*frame);
      r.holder = frame;
      return r;
    };
    try {
      engine_->process(n, fetch, readFrame(src), writeFrame(dst));
    } catch (const std::exception& e) {
      env->ThrowError("%s", e.what());
    }
    return dst;
  }

  int __stdcall SetCacheHints(int cachehints, int frame_range) override {
    (void)frame_range;
    return cachehints == CACHE_GET_MTMODE ? MT_NICE_FILTER : 0;
  }

 private:
  int planes(int out[4]) const {
    switch (fmt_.layout) {
      case Layout::PlanarYUV:
        out[0] = PLANAR_Y, out[1] = PLANAR_U, out[2] = PLANAR_V, out[3] = PLANAR_A;
        return fmt_.alpha ? 4 : 3;
      case Layout::PlanarRGB:
        out[0] = PLANAR_R, out[1] = PLANAR_G, out[2] = PLANAR_B, out[3] = PLANAR_A;
        return fmt_.alpha ? 4 : 3;
      case Layout::Gray:
        out[0] = PLANAR_Y;
        return 1;
      default:
        out[0] = 0;
        return 1;
    }
  }

  ConstFrame readFrame(const PVideoFrame& f) const {
    ConstFrame c;
    int ids[4];
    const int count = planes(ids);
    for (int i = 0; i < count; i++) {
      c.data[i] = f->GetReadPtr(ids[i]);
      c.stride[i] = f->GetPitch(ids[i]);
    }
    return c;
  }

  Frame writeFrame(PVideoFrame& f) const {
    Frame w;
    int ids[4];
    const int count = planes(ids);
    for (int i = 0; i < count; i++) {
      w.data[i] = f->GetWritePtr(ids[i]);
      w.stride[i] = f->GetPitch(ids[i]);
    }
    return w;
  }

  Format fmt_;
  bool has_v8_ = false;
  std::unique_ptr<Engine> engine_;
};

Format formatOf(const VideoInfo& vi, IScriptEnvironment* env) {
  Format f;
  f.width = vi.width;
  f.height = vi.height;
  f.bits = vi.BitsPerComponent();
  if (f.bits > 16) env->ThrowError("AutoLevels: float formats are not supported");
  if (vi.IsYUY2()) {
    f.layout = Layout::YUY2;
  } else if (vi.IsRGB24()) {
    f.layout = Layout::PackedBGR24;
  } else if (vi.IsRGB32()) {
    f.layout = Layout::PackedBGR32;
  } else if (vi.IsRGB48() || vi.IsRGB64()) {
    env->ThrowError("AutoLevels: RGB48/RGB64 are not supported, use planar RGB");
  } else if (vi.IsY()) {
    f.layout = Layout::Gray;
  } else if (vi.IsPlanarRGB() || vi.IsPlanarRGBA()) {
    f.layout = Layout::PlanarRGB;
    f.alpha = vi.IsPlanarRGBA();
  } else if (vi.IsPlanar() && vi.IsYUV()) {
    f.layout = Layout::PlanarYUV;
    f.alpha = vi.IsYUVA();
    f.ssw = vi.GetPlaneWidthSubsampling(PLANAR_U);
    f.ssh = vi.GetPlaneHeightSubsampling(PLANAR_U);
  } else {
    env->ThrowError("AutoLevels: unsupported colorspace");
  }
  return f;
}

AVSValue __cdecl Create(AVSValue args, void* user_data, IScriptEnvironment* env) {
  const bool autogamma_fn = user_data != nullptr;
  PClip child = args[0].AsClip();
  Params p = defaultParams(autogamma_fn);
  auto i = [&](int k, int& v) {
    if (args[k].Defined()) v = args[k].AsInt();
  };
  auto b = [&](int k, bool& v) {
    if (args[k].Defined()) v = args[k].AsBool();
  };
  auto d = [&](int k, double& v) {
    if (args[k].Defined()) v = args[k].AsFloat();
  };
  i(1, p.filterRadius);
  i(2, p.sceneChgThresh);
  if (args[3].Defined()) p.frameOverrides = args[3].AsString("");
  d(4, p.gamma);
  b(5, p.autogamma);
  d(6, p.midpoint);
  b(7, p.autolevel);
  i(8, p.input_low);
  i(9, p.input_high);
  i(10, p.output_low);
  i(11, p.output_high);
  b(12, p.coring);
  d(13, p.ignore);
  d(14, p.ignore_low);
  d(15, p.ignore_high);
  i(16, p.border);
  i(17, p.border_l);
  i(18, p.border_r);
  i(19, p.border_t);
  i(20, p.border_b);
  b(21, p.debug);
  i(22, p.sc2th);
  d(23, p.sc2perc);
  d(24, p.gammax);
  d(25, p.gammin);
  i(26, p.minRng);
  const Format fmt = formatOf(child->GetVideoInfo(), env);
  return new AutoLevelsAvs(child, p, fmt, env);
}

const char* const kArgs =
    "c[filterRadius]i[sceneChgThresh]i[frameOverrides]s[gamma]f[autogamma]b[midpoint]f[autolevel]b"
    "[input_low]i[input_high]i[output_low]i[output_high]i[coring]b[ignore]f[ignore_low]f[ignore_high]f"
    "[border]i[border_l]i[border_r]i[border_t]i[border_b]i[debug]b[sc2th]i[sc2Perc]f[gammax]f[gammin]f[minRng]i";

}  // namespace

#ifdef _WIN32
#define AUTOLEVELS_AVS_EXPORT extern "C" __declspec(dllexport)
#else
#define AUTOLEVELS_AVS_EXPORT extern "C" __attribute__((visibility("default")))
#endif

const AVS_Linkage* AVS_linkage = nullptr;

AUTOLEVELS_AVS_EXPORT const char* __stdcall AvisynthPluginInit3(IScriptEnvironment* env,
                                                                const AVS_Linkage* const vectors) {
  AVS_linkage = vectors;
  static int autogamma_tag = 1;
  env->AddFunction("AutoLevels", kArgs, Create, nullptr);
  env->AddFunction("AutoGamma", kArgs, Create, &autogamma_tag);
  return "AutoLevels " AUTOLEVELS_VERSION;
}
