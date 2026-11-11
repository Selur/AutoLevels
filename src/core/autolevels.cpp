// SPDX-License-Identifier: GPL-2.0-or-later
// AutoLevels engine. Algorithm: AutoLevels 0.1-0.3 (C) 2007 Theodor Anschuetz, 0.4-0.6 Jim Battle,
// 0.7-0.12 StainlessS; levels mapping from AviSynth's Levels(). Ported to a host-independent core.
#include "core/autolevels.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

namespace autolevels {

namespace {

const double kIgnoreDefault = 1.0 / 512.0;
const double kGammaxLimit = 10.0f;
const float kGammaxDefault = 1.5f;

[[noreturn]] void fail(const char* fmt, ...) {
  char buf[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  throw std::runtime_error(buf);
}

// The original was built with the VS2008 CRT, whose printf rounds exact ties (214.25 with %.1f) away from zero;
// current C libraries round them to even. Pre-rounding the debug values keeps the overlay text identical.
inline double crtRound(double v, int decimals) {
  const double m = std::pow(10.0, decimals);
  const double r = std::floor(std::fabs(v) * m + 0.5) / m;
  return v < 0 ? -r : r;
}

template <typename T>
inline const T* row(const ConstFrame& f, int plane, int y) {
  return reinterpret_cast<const T*>(f.data[plane] + y * f.stride[plane]);
}

template <typename T>
inline T* row(const Frame& f, int plane, int y) {
  return reinterpret_cast<T*>(f.data[plane] + y * f.stride[plane]);
}

}  // namespace

Params defaultParams(bool autogamma_fn) {
  Params p;
  p.autolevel = !autogamma_fn;
  p.autogamma = autogamma_fn;
  p.gammax = autogamma_fn ? 10.0f : kGammaxDefault;
  return p;
}

// ---------------------------------------------------------------------------
// construction and parameter checks
// ---------------------------------------------------------------------------

Engine::Engine(const Params& p, const Format& fmt, int num_frames)
    : fmt_(fmt),
      num_frames_(num_frames),
      filterRadius_(p.filterRadius),
      sceneChgThresh_(p.sceneChgThresh),
      gamma_(p.gamma),
      autogamma_(p.autogamma),
      midpoint_(p.midpoint),
      autolevel_(p.autolevel),
      coring_(p.coring),
      debug_(p.debug),
      sc2th_(p.sc2th),
      sc2perc_(p.sc2perc),
      gammax_(p.gammax),
      minRng_(p.minRng) {
  if (fmt.bits < 8 || fmt.bits > 16) fail("AutoLevels: only 8 to 16 bit integer formats are supported");
  if (fmt.bits != 8 && (fmt.layout == Layout::YUY2 || fmt.layout == Layout::PackedBGR24 ||
                        fmt.layout == Layout::PackedBGR32))
    fail("AutoLevels: packed formats are supported with 8 bit only");
  if (num_frames <= 0) fail("AutoLevels: the clip has no frames");
  scale_ = 1 << (fmt.bits - 8);
  maxv_ = (1 << fmt.bits) - 1;

  // fill excludeFrames and sceneChgOverrides
  std::string frmOverrides = p.frameOverrides;
  for (char& c : frmOverrides)
    if (c == ',') c = ' ';
  std::stringstream ss(frmOverrides);
  std::string range;
  while (ss >> range) {
    const size_t dashPos = range.find_first_of('-');
    int startFrm, endFrm;
    if (dashPos != std::string::npos) {  // range (from - to)
      if (!(std::istringstream(range.substr(1, dashPos - 1)) >> startFrm)) continue;
      if (!(std::istringstream(range.substr(dashPos + 1)) >> endFrm)) continue;
    } else {  // single frame
      if (!(std::istringstream(range.substr(1)) >> startFrm)) continue;
      endFrm = startFrm;
    }
    switch (range.at(0)) {
      case 'S':  // assume a new scene at each frame
        for (int i = startFrm; i <= endFrm; i++) sceneChgOverrides_.insert({i, true});
        break;
      case 'N':  // no new scene at these frames
        for (int i = startFrm; i <= endFrm; i++) sceneChgOverrides_.insert({i, false});
        break;
      case 'E':  // leave these frames unchanged
        excludeFrames_.push_back({startFrm, endFrm});
        break;
      default:
        fail("AutoLevels: bogus frameOverrides specifier '%s' (S, N or E expected)", range.c_str());
    }
  }

  if (filterRadius_ < 0) fail("AutoLevels: filterRadius must be >= 0");
  if (gamma_ <= 0.0) fail("AutoLevels: gamma must be > 0.0");
  if (midpoint_ <= 0.0) fail("AutoLevels: midpoint must be more than 0.0");
  if (midpoint_ >= 1.0) fail("AutoLevels: midpoint must be less than 1.0");

  // ignore_low/ignore_high win over ignore, which wins over the default
  ignore_low_ = (p.ignore_low >= 0.0) ? p.ignore_low : (p.ignore >= 0.0) ? p.ignore : kIgnoreDefault;
  ignore_high_ = (p.ignore_high >= 0.0) ? p.ignore_high : (p.ignore >= 0.0) ? p.ignore : kIgnoreDefault;
  if (ignore_low_ > 0.45) fail("AutoLevels: ignore_low fraction is very large; try something under 0.45");
  if (ignore_high_ > 0.45) fail("AutoLevels: ignore_high fraction is very large; try something under 0.45");

  // pick the more constraining boundary for each side
  border_l_ = std::max(0, std::max(p.border, p.border_l));
  border_r_ = std::max(0, std::max(p.border, p.border_r));
  border_t_ = std::max(0, std::max(p.border, p.border_t));
  border_b_ = std::max(0, std::max(p.border, p.border_b));
  if (border_l_ + border_r_ >= fmt.width)
    fail("AutoLevels: the left+right border is greater than the image width");
  if (border_t_ + border_b_ >= fmt.height)
    fail("AutoLevels: the top+bottom border is greater than the image height");

  // levels on the 8 bit scale; YUV scales by bit shift (limited range), RGB to full range
  auto native = [&](int v) {
    return fmt.isRGB() ? int((int64_t(v) * maxv_ * 2 + 255) / 510) : v * scale_;
  };
  force_input_low_ = (p.input_low != kUndef);
  force_input_high_ = (p.input_high != kUndef);
  if (force_input_low_ && (p.input_low < 0 || p.input_low > 255))
    fail("AutoLevels: input_low is not in range [0 ... 255]");
  if (force_input_high_ && (p.input_high < 0 || p.input_high > 255))
    fail("AutoLevels: input_high is not in range [0 ... 255]");
  if (force_input_low_ && force_input_high_ && (p.input_low >= p.input_high))
    fail("AutoLevels: input_low must be smaller than input_high if both are specified");
  if (p.output_low != kUndef && (p.output_low < 0 || p.output_low > 255))
    fail("AutoLevels: output_low is not in range [0 ... 255]");
  if (p.output_high != kUndef && (p.output_high < 0 || p.output_high > 255))
    fail("AutoLevels: output_high is not in range [0 ... 255]");
  input_low_ = force_input_low_ ? native(p.input_low) : 0;
  input_high_ = force_input_high_ ? native(p.input_high) : maxv_;
  // if coring is enabled, output_low/high refer to [0..255] before the range gets remapped to 16..235
  output_low_ = native((p.output_low != kUndef) ? p.output_low : (fmt.isRGB() || coring_) ? 0 : 16);
  output_high_ = native((p.output_high != kUndef) ? p.output_high : (fmt.isRGB() || coring_) ? 255 : 235);

  if (sc2th_ < 0 || sc2th_ > 256) fail("AutoLevels: 0 <= sc2th <= 256 (%d)", sc2th_);
  if (sc2perc_ < 0.0 || sc2perc_ >= 100.0) fail("AutoLevels: 0.0 <= sc2Perc < 100.0 (%.3f)", sc2perc_);
  if (gammax_ < 1.0 || gammax_ > kGammaxLimit) fail("AutoLevels: 1.0 <= gammax <= %f", kGammaxLimit);
  gammin_ = (p.gammin < 0.0) ? double(float(1.0f / gammax_)) : p.gammin;
  if (gammin_ < (1.0f / kGammaxLimit) || gammin_ > 1.0) fail("AutoLevels: %f <= gammin <= 1.0", 1.0f / kGammaxLimit);
  if (minRng_ != 0 && (minRng_ < 64 || minRng_ > 128))
    fail("AutoLevels: minRng=0 [off] OR 64 <= minRng (%d) <= 128", minRng_);

  // Rec.601 luma of RGB, as AutoLevels 0.4+
  const double Kr = 0.299, Kb = 0.114, Kg = 1.0 - Kr - Kb;
  const int yRange = 255;
  cyb_ = int(Kb * yRange / 255.0 * 65536.0 + 0.5);
  cyg_ = int(Kg * yRange / 255.0 * 65536.0 + 0.5);
  cyr_ = int(Kr * yRange / 255.0 * 65536.0 + 0.5);

  slots_.resize(size_t(num_frames));
}

bool Engine::isExcluded(int n) const {
  for (const auto& e : excludeFrames_)
    if (n >= e.first && n <= e.second) return true;
  return false;
}

void Engine::sourceRange(int n, int& first, int& last) const {
  const bool inputs_known = force_input_low_ && force_input_high_;
  if (isExcluded(n) || !((autolevel_ && !inputs_known) || autogamma_ || debug_)) {
    first = 1;
    last = 0;
    return;
  }
  // scene change test of frame i reads frame i - 1
  first = std::max(0, n - filterRadius_ - 1);
  last = std::min(num_frames_ - 1, n + filterRadius_);
}

// ---------------------------------------------------------------------------
// luma access (border-cropped rows)
// ---------------------------------------------------------------------------

template <typename T>
const T* Engine::lumaRow(const ConstFrame& f, int r, T* buf) const {
  const int xlim = fmt_.width - border_l_ - border_r_;
  switch (fmt_.layout) {
    case Layout::Gray:
    case Layout::PlanarYUV:
      return row<T>(f, 0, border_t_ + r) + border_l_;
    case Layout::YUY2: {
      const uint8_t* s = f.data[0] + (border_t_ + r) * f.stride[0] + 2 * border_l_;
      for (int x = 0; x < xlim; x++) buf[x] = T(s[2 * x]);
      return buf;
    }
    case Layout::PackedBGR24:
    case Layout::PackedBGR32: {
      // rows are stored bottom first, so the memory offset skips the bottom border
      const int inc = fmt_.layout == Layout::PackedBGR32 ? 4 : 3;
      const uint8_t* s = f.data[0] + (border_b_ + r) * f.stride[0] + inc * border_l_;
      for (int x = 0; x < xlim; x++, s += inc)
        buf[x] = T((cyb_ * s[0] + cyg_ * s[1] + cyr_ * s[2] + 0x8000) >> 16);
      return buf;
    }
    case Layout::PlanarRGB: {
      const T* pr = row<T>(f, 0, border_t_ + r) + border_l_;
      const T* pg = row<T>(f, 1, border_t_ + r) + border_l_;
      const T* pb = row<T>(f, 2, border_t_ + r) + border_l_;
      for (int x = 0; x < xlim; x++)
        buf[x] = T((int64_t(cyb_) * pb[x] + int64_t(cyg_) * pg[x] + int64_t(cyr_) * pr[x] + 0x8000) >> 16);
      return buf;
    }
  }
  return buf;
}

template <typename T>
void Engine::histogram(const ConstFrame& f, std::vector<int>& hist) const {
  const int ylim = fmt_.height - border_t_ - border_b_;
  const int xlim = fmt_.width - border_l_ - border_r_;
  std::vector<T> buf(static_cast<size_t>(xlim));
  for (int y = 0; y < ylim; y++) {
    const T* l = lumaRow<T>(f, y, buf.data());
    for (int x = 0; x < xlim; x++) hist[l[x]]++;
  }
}

void Engine::histogramAny(const ConstFrame& f, std::vector<int>& hist) const {
  hist.assign(size_t(maxv_) + 1, 0);
  if (fmt_.bits == 8)
    histogram<uint8_t>(f, hist);
  else
    histogram<uint16_t>(f, hist);
}

// Percentage of pixels whose luma differs by more than sc2th between two frames.
template <typename T>
double Engine::pixDif(const ConstFrame& a, const ConstFrame& b) const {
  const int ylim = fmt_.height - border_t_ - border_b_;
  const int xlim = fmt_.width - border_l_ - border_r_;
  const int th = sc2th_ * scale_;
  std::vector<T> ba(static_cast<size_t>(xlim)), bb(static_cast<size_t>(xlim));
  int64_t cnt = 0;
  for (int y = 0; y < ylim; y++) {
    const T* la = lumaRow<T>(a, y, ba.data());
    const T* lb = lumaRow<T>(b, y, bb.data());
    for (int x = 0; x < xlim; x++)
      if (std::abs(int(la[x]) - int(lb[x])) > th) ++cnt;
  }
  return double(cnt) * 100.0 / (double(ylim) * xlim);
}

double Engine::pixDifPerc(int n, const Fetch& fetch) const {
  if (n <= 0) return 100.0;
  const FrameRef f1 = fetch(n - 1);
  const FrameRef f2 = fetch(n);
  return fmt_.bits == 8 ? pixDif<uint8_t>(f1.frame, f2.frame) : pixDif<uint16_t>(f1.frame, f2.frame);
}

// ---------------------------------------------------------------------------
// per-frame statistics (computed once per frame, shared by all threads)
// ---------------------------------------------------------------------------

// Limits of the luma histogram, skipping the darkest and brightest outliers, and the mean between them.
Engine::Stats Engine::getStats(int n, const Fetch& fetch) const {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slots_[n].stats_valid) return slots_[n].stats;
  }
  std::vector<int> yhisto;
  {
    const FrameRef f = fetch(n);
    histogramAny(f.frame, yhisto);
  }
  const int num_pixels = (fmt_.width - border_l_ - border_r_) * (fmt_.height - border_t_ - border_b_);
  const int pixels_ignore_low = int(num_pixels * ignore_low_);
  const int pixels_ignore_high = int(num_pixels * ignore_high_);

  Stats s;
  s.ymin = maxv_;
  int sm = 0;
  for (int i = 0; i <= maxv_; ++i) {
    sm += yhisto[i];
    if (sm > pixels_ignore_low) {
      s.ymin = i;
      break;
    }
  }
  s.ymax = 0;
  sm = 0;
  for (int i = maxv_; i >= 0; --i) {
    sm += yhisto[i];
    if (sm > pixels_ignore_high) {
      s.ymax = i;
      break;
    }
  }
  int64_t wsum = 0;
  int pixcnt = 0;
  for (int bkt = s.ymin; bkt <= s.ymax; bkt++) {
    pixcnt += yhisto[bkt];
    wsum += int64_t(yhisto[bkt]) * bkt;
  }
  s.mean = double(wsum) / double(pixcnt);

  std::lock_guard<std::mutex> lock(mutex_);
  slots_[n].stats = s;
  slots_[n].stats_valid = true;
  return s;
}

// Not a scene change detection in the strict sense: it checks whether the minimum, maximum or mean luma change
// greatly, then (sc2th < 256) confirms with the share of changed pixels.
bool Engine::isSceneStart(int n, const Fetch& fetch, bool* sc1) const {
  if (sc1) *sc1 = false;
  auto it = sceneChgOverrides_.find(n);
  if (it != sceneChgOverrides_.end()) return it->second;
  if (n <= 0) return false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slots_[n].sc_valid) {
      if (sc1) *sc1 = slots_[n].sc1;
      return slots_[n].sc;
    }
  }
  const Stats s1 = getStats(n - 1, fetch);
  const Stats s2 = getStats(n, fetch);
  const int thresh = sceneChgThresh_ * scale_;
  const int minDiff = std::abs(s2.ymin - s1.ymin);
  const int maxDiff = std::abs(s2.ymax - s1.ymax);
  const double meanDiff = std::fabs(s1.mean - s2.mean);
  bool primary = (minDiff >= thresh) || (maxDiff >= thresh) || (meanDiff >= (thresh * 0.5));
  bool sceneChange = primary;
  if (primary && sc2th_ < 256) sceneChange = (sc2perc_ < pixDifPerc(n, fetch));

  std::lock_guard<std::mutex> lock(mutex_);
  slots_[n].sc1 = primary;
  slots_[n].sc = sceneChange;
  slots_[n].sc_valid = true;
  if (sc1) *sc1 = primary;
  return sceneChange;
}

// ---------------------------------------------------------------------------
// levels mapping (AviSynth Levels())
// ---------------------------------------------------------------------------

template <typename T>
void Engine::adjust(const ConstFrame& src, const Frame& dst, int in_min, float gamma, int in_max, int out_min,
                    int out_max, bool coring) const {
  const int s = scale_;
  std::vector<T> map(size_t(maxv_) + 1), mapchroma(size_t(maxv_) + 1);

  gamma = std::min(std::max(gamma, 0.1f), 10.0f);
  gamma = 1 / gamma;
  const int divisor = (in_max == in_min) ? 1 : (in_max - in_min);

  if (fmt_.isYUV()) {
    const int lo = coring ? 16 * s : 0, hi = coring ? 235 * s : maxv_, hic = coring ? 240 * s : maxv_;
    for (int i = 0; i <= maxv_; ++i) {
      float p;
      if (coring)
        p = ((i - 16 * s) * (255.0f / 219.0f) - in_min) / divisor;
      else
        p = float(i - in_min) / divisor;
      p = std::pow(std::min(std::max(p, 0.0f), 1.0f), gamma);
      p = p * (out_max - out_min) + out_min;
      int pp;
      if (coring)
        pp = int(p * (219.0f / 255.0f) + (16.0f * s + 0.5f));
      else
        pp = int(p + 0.5f);
      map[i] = T(std::min(std::max(pp, lo), hi));
      const int q = int((int64_t(i - 128 * s) * (out_max - out_min) + (divisor >> 1)) / divisor) + 128 * s;
      mapchroma[i] = T(std::min(std::max(q, lo), hic));
    }
  } else {
    for (int i = 0; i <= maxv_; ++i) {
      float p = float(i - in_min) / divisor;
      p = std::pow(std::min(std::max(p, 0.0f), 1.0f), gamma);
      p = p * (out_max - out_min) + out_min;
      const int z = int(p + 0.5f);
      map[i] = T(z < 0 ? 0 : z > maxv_ ? maxv_ : z);
    }
  }

  auto mapPlane = [&](int plane, int w, int h, const std::vector<T>& m) {
    for (int y = 0; y < h; y++) {
      const T* sp = row<T>(src, plane, y);
      T* dp = row<T>(dst, plane, y);
      for (int x = 0; x < w; x++) dp[x] = m[sp[x]];
    }
  };
  auto copyPlane = [&](int plane, int w, int h) {
    for (int y = 0; y < h; y++)
      std::copy(row<T>(src, plane, y), row<T>(src, plane, y) + w, row<T>(dst, plane, y));
  };

  const int w = fmt_.width, h = fmt_.height;
  switch (fmt_.layout) {
    case Layout::Gray:
      mapPlane(0, w, h, map);
      break;
    case Layout::PlanarYUV: {
      mapPlane(0, w, h, map);
      const int wc = w >> fmt_.ssw, hc = h >> fmt_.ssh;
      mapPlane(1, wc, hc, mapchroma);
      mapPlane(2, wc, hc, mapchroma);
      break;
    }
    case Layout::YUY2:
      for (int y = 0; y < h; y++) {
        const uint8_t* sp = src.data[0] + y * src.stride[0];
        uint8_t* dp = dst.data[0] + y * dst.stride[0];
        for (int x = 0; x < 2 * w; x += 2) {
          dp[x] = uint8_t(map[sp[x]]);
          dp[x + 1] = uint8_t(mapchroma[sp[x + 1]]);
        }
      }
      break;
    case Layout::PackedBGR24:
      mapPlane(0, 3 * w, h, map);
      break;
    case Layout::PackedBGR32:  // alpha stays unchanged
      for (int y = 0; y < h; y++) {
        const uint8_t* sp = src.data[0] + y * src.stride[0];
        uint8_t* dp = dst.data[0] + y * dst.stride[0];
        for (int x = 0; x < 4 * w; x += 4) {
          dp[x] = uint8_t(map[sp[x]]);
          dp[x + 1] = uint8_t(map[sp[x + 1]]);
          dp[x + 2] = uint8_t(map[sp[x + 2]]);
          dp[x + 3] = sp[x + 3];
        }
      }
      break;
    case Layout::PlanarRGB:
      for (int p = 0; p < 3; p++) mapPlane(p, w, h, map);
      break;
  }
  if (fmt_.alpha) copyPlane(3, w, h);
}

// ---------------------------------------------------------------------------
// process a frame
// ---------------------------------------------------------------------------

void Engine::process(int n, const Fetch& fetch, const ConstFrame& src, const Frame& dst) const {
  const bool hbd = fmt_.bits > 8;
  const double M = maxv_;
  // Excluded frames (isExcluded) never get here: the hosts return the source frame itself.

  // averages of the minimum, maximum and mean luma over the nearby frames
  int this_ymin = 0, this_ymax = maxv_;
  double this_ymean = this_ymax / 2.0;
  bool this_newscene = false;
  bool this_sc1 = false;
  double ymin_avg = 0.0;
  double ymax_avg = double(this_ymax);
  double ymean_avg = ymax_avg / 2.0;
  const bool inputs_known = force_input_low_ && force_input_high_;
  int avgFrom = 0, avgTo = 0;

  if ((autolevel_ && !inputs_known) || autogamma_ || debug_) {
    avgFrom = std::max(0, n - filterRadius_);
    avgTo = std::min(num_frames_ - 1, n + filterRadius_);
    // restrict the range by scene changes found between avgFrom and avgTo
    for (int i = avgFrom; i <= avgTo; i++) {
      bool sc1 = false;
      const bool sceneChange = (i > 0) && isSceneStart(i, fetch, &sc1);
      if (i == n) this_sc1 = sc1;
      if (sceneChange) {
        if (i <= n) {
          avgFrom = i;
          this_newscene |= (i == n);
        } else {
          avgTo = i - 1;
          break;
        }
      }
    }
    const int frame_count = avgTo - avgFrom + 1;
    int64_t yMinAccum = 0, yMaxAccum = 0;
    double ymeanAccum = 0.0;
    for (int i = avgFrom; i <= avgTo; i++) {
      const Stats s = getStats(i, fetch);
      yMinAccum += s.ymin;
      yMaxAccum += s.ymax;
      ymeanAccum += s.mean;
      if (i == n) {
        this_ymin = s.ymin;
        this_ymax = s.ymax;
        this_ymean = s.mean;
      }
    }
    ymin_avg = double(yMinAccum) / frame_count;
    ymax_avg = double(yMaxAccum) / frame_count;
    ymean_avg = ymeanAccum / frame_count;
  }

  const bool do_coring = coring_ && fmt_.isYUV();
  const double in_low = force_input_low_ ? double(input_low_) : ymin_avg;
  const double in_high = force_input_high_ ? double(input_high_) : ymax_avg;

  // gamma curve: limit the mean to 1 .. 254 (8 bit scale) to avoid nasty numbers and division by zero
  double do_gamma =
      !autogamma_ ? gamma_ : std::log(std::min(std::max(ymean_avg, 1.0 * scale_), 254.0 * scale_) / M) /
                                 std::log(midpoint_);
  do_gamma = std::min(std::max(do_gamma, 0.1), 10.0);

  bool fading = false;
  double adj_lo = in_low, adj_hi = in_high, adj_gamma = do_gamma;
  if ((autolevel_ && !inputs_known) || autogamma_) {
    if (autogamma_) adj_gamma = std::min(std::max(adj_gamma, gammin_), gammax_);
    const int rng = this_ymax - this_ymin;
    const int minRng = minRng_ * scale_;
    // fade the adjustments out on frames with a small luma range
    if ((minRng != 0) && (rng < minRng) && (!force_input_high_ || !force_input_low_)) {
      fading = true;
      const double fadefact = rng / double(minRng);
      if (!force_input_low_) adj_lo *= fadefact;
      if (!force_input_high_) adj_hi = M - ((M - adj_hi) * fadefact);
      adj_gamma = (adj_gamma - 1.0) * fadefact + 1.0;
    }
  }
  // with coring and measured levels, counteract the coring remap done in adjust()
  if (do_coring) {
    if (!force_input_low_) adj_lo = adj_lo * (219.0 / 255.0) + 16.0 * scale_;
    if (!force_input_high_) adj_hi = adj_hi * (219.0 / 255.0) + 16.0 * scale_;
  }

  if (hbd)
    adjust<uint16_t>(src, dst, int(adj_lo + 0.5), float(adj_gamma), int(adj_hi + 0.5), output_low_, output_high_,
                     do_coring);
  else
    adjust<uint8_t>(src, dst, int(adj_lo + 0.5), float(adj_gamma), int(adj_hi + 0.5), output_low_, output_high_,
                    do_coring);

  if (!debug_) return;

  // output mean, from the written frame
  ConstFrame out;
  for (int i = 0; i < 4; i++) {
    out.data[i] = dst.data[i];
    out.stride[i] = dst.stride[i];
  }
  std::vector<int> hist;
  histogramAny(out, hist);
  int64_t px = 0, acc = 0;
  for (int i = maxv_; i >= 0; --i) {
    px += hist[i];
    acc += int64_t(i) * hist[i];
  }
  const double oavg = double(acc) / double(px);

  char bf[64];
  char flobf[64] = "";
  char fhibf[64] = "";
  char scbf[64] = "";
  const bool sos = this_newscene;
  const bool sos1 = this_sc1 || (n == 0);
  const bool doing_lo = (int(adj_lo + 0.5) != this_ymin);
  const bool doing_hi = (int(adj_hi + 0.5) != this_ymax);
  const bool doing_gamma = (adj_gamma != 1.0);
  const bool gamlim = (do_gamma != adj_gamma);
  std::snprintf(bf, sizeof(bf), "\a%cL\a%cH\a%cG\a%c%c\a%cs\a%cS\a%cF\a-", doing_lo ? 'V' : 'P',
                doing_hi ? 'V' : 'P', doing_gamma ? 'V' : 'P', gamlim ? 'V' : 'P', 0x91, sos1 ? 'V' : 'P',
                sos ? 'V' : 'P', fading ? '!' : 'P');
  if (minRng_ != 0) {
    std::snprintf(flobf, sizeof(flobf), "\a%c{%5.1f}\a-", fading ? '!' : 'P', crtRound(adj_lo, 1));
    std::snprintf(fhibf, sizeof(fhibf), "\a%c{%5.1f}\a-", fading ? '!' : 'P', crtRound(adj_hi, 1));
  }
  if (sc2th_ < 256) std::snprintf(scbf, sizeof(scbf), " : sc2=%.1f%%", crtRound(pixDifPerc(n, fetch), 1));
  char text[2048];
  std::snprintf(text, sizeof(text),
                "%d:%d,%d] %s%s\nymin=%3d   ymin_avg =%5.1f%s\nymax=%3d   ymax_avg =%5.1f%s\nmean=%5.1f "
                "ymean_avg=%5.1f\n\a%cGamm=%4.3f\a- out_mean =%6.2f(%.2f)",
                n, avgFrom, avgTo, bf, scbf, this_ymin, crtRound(in_low, 1), flobf, this_ymax, crtRound(in_high, 1), fhibf,
                crtRound(this_ymean, 1), crtRound(ymean_avg, 1), fading ? '!' : '-', crtRound(adj_gamma, 3),
                crtRound(oavg, 2), crtRound(oavg / M, 2));
  if (hbd)
    drawText<uint16_t>(dst, 0, 0, text);
  else
    drawText<uint8_t>(dst, 0, 0, text);
}

}  // namespace autolevels
