// SPDX-License-Identifier: GPL-2.0-or-later
// VapourSynth (API 4) interface of AutoLevels: autolevels.AutoLevels / autolevels.AutoGamma,
// 8-16 bit integer Gray, YUV and RGB.
#include <memory>
#include <stdexcept>
#include <string>

#include "VSHelper4.h"
#include "VapourSynth4.h"
#include "core/autolevels.hpp"
#include "version.hpp"

using namespace autolevels;

namespace {

struct FilterData {
  VSNode* node = nullptr;
  int planes = 1;
  std::unique_ptr<Engine> engine;
};

const VSFrame* VS_CC getFrame(int n, int reason, void* instance, void**, VSFrameContext* ctx, VSCore* core,
                              const VSAPI* vsapi) {
  auto* d = static_cast<FilterData*>(instance);
  if (reason == arInitial) {
    int first, last;
    d->engine->sourceRange(n, first, last);
    if (n < first || n > last) vsapi->requestFrameFilter(n, d->node, ctx);
    for (int k = first; k <= last; k++) vsapi->requestFrameFilter(k, d->node, ctx);
    return nullptr;
  }
  if (reason != arAllFramesReady) return nullptr;

  const VSFrame* src = vsapi->getFrameFilter(n, d->node, ctx);
  if (d->engine->isExcluded(n)) return src;

  auto read = [d, vsapi](const VSFrame* f) {
    ConstFrame c;
    for (int i = 0; i < d->planes; i++) {
      c.data[i] = vsapi->getReadPtr(f, i);
      c.stride[i] = vsapi->getStride(f, i);
    }
    return c;
  };
  const Fetch fetch = [d, ctx, vsapi, &read](int k) {
    std::shared_ptr<const VSFrame> frame(vsapi->getFrameFilter(k, d->node, ctx),
                                         [vsapi](const VSFrame* f) { vsapi->freeFrame(f); });
    FrameRef r;
    r.frame = read(frame.get());
    r.holder = frame;
    return r;
  };

  VSFrame* dst = vsapi->newVideoFrame(vsapi->getVideoFrameFormat(src), vsapi->getFrameWidth(src, 0),
                                      vsapi->getFrameHeight(src, 0), src, core);
  Frame out;
  for (int i = 0; i < d->planes; i++) {
    out.data[i] = vsapi->getWritePtr(dst, i);
    out.stride[i] = vsapi->getStride(dst, i);
  }
  try {
    d->engine->process(n, fetch, read(src), out);
  } catch (const std::exception& e) {
    vsapi->setFilterError(e.what(), ctx);
    vsapi->freeFrame(src);
    vsapi->freeFrame(dst);
    return nullptr;
  }
  vsapi->freeFrame(src);
  return dst;
}

void VS_CC freeFilter(void* instance, VSCore*, const VSAPI* vsapi) {
  auto* d = static_cast<FilterData*>(instance);
  vsapi->freeNode(d->node);
  delete d;
}

void VS_CC create(const VSMap* in, VSMap* out, void* user_data, VSCore* core, const VSAPI* vsapi) {
  const bool autogamma_fn = user_data != nullptr;
  auto d = std::make_unique<FilterData>();
  d->node = vsapi->mapGetNode(in, "clip", 0, nullptr);
  auto fail = [&](const std::string& msg) {
    vsapi->mapSetError(out, msg.c_str());
    vsapi->freeNode(d->node);
  };

  int e;
  Params p = defaultParams(autogamma_fn);
  auto i = [&](const char* key, int& v) {
    const int64_t x = vsapi->mapGetInt(in, key, 0, &e);
    if (!e) v = int(x);
  };
  auto b = [&](const char* key, bool& v) {
    const int64_t x = vsapi->mapGetInt(in, key, 0, &e);
    if (!e) v = x != 0;
  };
  auto f = [&](const char* key, double& v) {
    const double x = vsapi->mapGetFloat(in, key, 0, &e);
    if (!e) v = x;
  };
  i("filterRadius", p.filterRadius);
  i("sceneChgThresh", p.sceneChgThresh);
  const char* ov = vsapi->mapGetData(in, "frameOverrides", 0, &e);
  if (!e) p.frameOverrides = ov;
  f("gamma", p.gamma);
  b("autogamma", p.autogamma);
  f("midpoint", p.midpoint);
  b("autolevel", p.autolevel);
  i("input_low", p.input_low);
  i("input_high", p.input_high);
  i("output_low", p.output_low);
  i("output_high", p.output_high);
  b("coring", p.coring);
  f("ignore", p.ignore);
  f("ignore_low", p.ignore_low);
  f("ignore_high", p.ignore_high);
  i("border", p.border);
  i("border_l", p.border_l);
  i("border_r", p.border_r);
  i("border_t", p.border_t);
  i("border_b", p.border_b);
  b("debug", p.debug);
  i("sc2th", p.sc2th);
  f("sc2Perc", p.sc2perc);
  f("gammax", p.gammax);
  f("gammin", p.gammin);
  i("minRng", p.minRng);

  const VSVideoInfo* vi = vsapi->getVideoInfo(d->node);
  if (!vsh::isConstantVideoFormat(vi)) return fail("AutoLevels: only constant format input is supported");
  const VSVideoFormat& vf = vi->format;
  if (vf.sampleType != stInteger || vf.bitsPerSample > 16)
    return fail("AutoLevels: only 8-16 bit integer clips are supported");
  Format fmt;
  fmt.width = vi->width;
  fmt.height = vi->height;
  fmt.bits = vf.bitsPerSample;
  switch (vf.colorFamily) {
    case cfGray:
      fmt.layout = Layout::Gray;
      d->planes = 1;
      break;
    case cfYUV:
      fmt.layout = Layout::PlanarYUV;
      fmt.ssw = vf.subSamplingW;
      fmt.ssh = vf.subSamplingH;
      d->planes = 3;
      break;
    case cfRGB:
      fmt.layout = Layout::PlanarRGB;
      d->planes = 3;
      break;
    default:
      return fail("AutoLevels: unsupported color family");
  }
  try {
    d->engine = std::make_unique<Engine>(p, fmt, vi->numFrames);
  } catch (const std::exception& ex) {
    return fail(ex.what());
  }

  VSFilterDependency deps[] = {{d->node, rpGeneral}};
  vsapi->createVideoFilter(out, autogamma_fn ? "AutoGamma" : "AutoLevels", vi, getFrame, freeFilter, fmParallel,
                           deps, 1, d.release(), core);
}

const char* const kArgs =
    "clip:vnode;filterRadius:int:opt;sceneChgThresh:int:opt;frameOverrides:data:opt;gamma:float:opt;"
    "autogamma:int:opt;midpoint:float:opt;autolevel:int:opt;input_low:int:opt;input_high:int:opt;"
    "output_low:int:opt;output_high:int:opt;coring:int:opt;ignore:float:opt;ignore_low:float:opt;"
    "ignore_high:float:opt;border:int:opt;border_l:int:opt;border_r:int:opt;border_t:int:opt;border_b:int:opt;"
    "debug:int:opt;sc2th:int:opt;sc2Perc:float:opt;gammax:float:opt;gammin:float:opt;minRng:int:opt;";

int autogamma_tag = 1;

}  // namespace

VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin* plugin, const VSPLUGINAPI* vspapi) {
  vspapi->configPlugin("io.github.selur.autolevels", "autolevels", "AutoLevels " AUTOLEVELS_VERSION,
                       VS_MAKE_VERSION(AUTOLEVELS_VERSION_MAJOR, AUTOLEVELS_VERSION_MINOR), VAPOURSYNTH_API_VERSION,
                       0, plugin);
  vspapi->registerFunction("AutoLevels", kArgs, "clip:vnode;", create, nullptr, plugin);
  vspapi->registerFunction("AutoGamma", kArgs, "clip:vnode;", create, &autogamma_tag, plugin);
}
