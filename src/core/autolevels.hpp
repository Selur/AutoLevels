// SPDX-License-Identifier: GPL-2.0-or-later
// Host-independent engine of AutoLevels: temporally smoothed automatic levels/gamma adjustment.
// 8..16 bit integer samples; at 8 bit the output equals AutoLevels v0.12 Beta 3 by StainlessS.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace autolevels {

// Sample layouts. Planar YUV planes: 0 Y, 1 U, 2 V, 3 A. Planar RGB planes: 0 R, 1 G, 2 B, 3 A.
// Packed layouts (8 bit only) use plane 0; packed RGB (AviSynth RGB24/RGB32) stores the bottom image row first.
enum class Layout { Gray, PlanarYUV, YUY2, PackedBGR24, PackedBGR32, PlanarRGB };

struct Format {
  Layout layout = Layout::PlanarYUV;
  int bits = 8;        // 8..16; samples wider than 8 bit are uint16_t
  int width = 0;
  int height = 0;
  int ssw = 0;         // log2 chroma subsampling, PlanarYUV only
  int ssh = 0;
  bool alpha = false;  // planar alpha plane present (copied unchanged)

  bool isRGB() const {
    return layout == Layout::PackedBGR24 || layout == Layout::PackedBGR32 || layout == Layout::PlanarRGB;
  }
  bool isYUV() const { return !isRGB(); }
};

// Plane pointers with strides in bytes.
struct ConstFrame {
  const uint8_t* data[4] = {nullptr, nullptr, nullptr, nullptr};
  ptrdiff_t stride[4] = {0, 0, 0, 0};
};

struct Frame {
  uint8_t* data[4] = {nullptr, nullptr, nullptr, nullptr};
  ptrdiff_t stride[4] = {0, 0, 0, 0};
};

// A source frame plus whatever keeps its memory alive in the host.
struct FrameRef {
  ConstFrame frame;
  std::shared_ptr<const void> holder;
};
using Fetch = std::function<FrameRef(int)>;

constexpr int kUndef = -0xC0FFEE;  // sentinel for unspecified integer parameters

// All level parameters (input/output levels, thresholds, minRng) are on the 8 bit scale [0, 255] and are
// scaled to the clip's bit depth internally.
struct Params {
  int filterRadius = 7;
  int sceneChgThresh = 20;
  std::string frameOverrides;
  double gamma = 1.0;
  bool autogamma = false;
  double midpoint = 0.5;
  bool autolevel = true;
  int input_low = kUndef, input_high = kUndef;
  int output_low = kUndef, output_high = kUndef;
  bool coring = false;
  double ignore = -1.0, ignore_low = -1.0, ignore_high = -1.0;
  int border = 0, border_l = 0, border_r = 0, border_t = 0, border_b = 0;
  bool debug = false;
  int sc2th = 12;
  double sc2perc = double(33.33f);
  double gammax = 1.5;
  double gammin = -1.0;  // < 0: derived from gammax
  int minRng = 100;
};

// Defaults of AutoLevels() (autogamma_fn = false) and AutoGamma() (autogamma_fn = true).
Params defaultParams(bool autogamma_fn);

class Engine {
 public:
  // Throws std::runtime_error with a user-facing message for invalid parameters or formats.
  Engine(const Params& p, const Format& fmt, int num_frames);

  bool isExcluded(int n) const;
  // Source frames process() may fetch for output frame n: [first, last]; none when first > last.
  void sourceRange(int n, int& first, int& last) const;
  // Writes output frame n into dst (every plane); src is source frame n. Thread-safe.
  // Not for excluded frames: the host returns the source frame for those.
  // full_range: the frame's range property says full range (YUV/Gray only; RGB is always full range).
  void process(int n, const Fetch& fetch, const ConstFrame& src, const Frame& dst, bool full_range) const;

 private:
  struct Stats {
    int ymin = 0, ymax = 0;
    double mean = 0.0;
  };
  struct Slot {
    bool stats_valid = false;
    bool sc_valid = false;
    bool sc = false, sc1 = false;
    Stats stats;
  };

  Stats getStats(int n, const Fetch& fetch) const;
  bool isSceneStart(int n, const Fetch& fetch, bool* sc1) const;
  double pixDifPerc(int n, const Fetch& fetch) const;
  template <typename T> void histogram(const ConstFrame& f, std::vector<int>& hist) const;
  template <typename T> double pixDif(const ConstFrame& a, const ConstFrame& b) const;
  template <typename T> const T* lumaRow(const ConstFrame& f, int row, T* buf) const;
  template <typename T>
  void adjust(const ConstFrame& src, const Frame& dst, int in_min, float gamma, int in_max, int out_min, int out_max,
              bool coring) const;
  template <typename T> void drawText(const Frame& dst, int x, int y, const char* s) const;
  void histogramAny(const ConstFrame& f, std::vector<int>& hist) const;

  Format fmt_;
  int num_frames_;
  int scale_;  // 1 << (bits - 8)
  int maxv_;   // (1 << bits) - 1
  int filterRadius_, sceneChgThresh_;
  std::unordered_map<int, bool> sceneChgOverrides_;
  std::vector<std::pair<int, int>> excludeFrames_;
  double gamma_;
  bool autogamma_;
  double midpoint_;
  bool autolevel_;
  // native scale; index 0 limited, 1 full range (equal for RGB)
  int input_low_[2], input_high_[2], output_low_[2], output_high_[2];
  bool force_input_low_, force_input_high_;
  bool coring_;
  double ignore_low_, ignore_high_;
  int border_l_, border_r_, border_t_, border_b_;
  bool debug_;
  int sc2th_;  // 8 bit scale; 256 switches the secondary scene change test off
  double sc2perc_;
  double gammax_, gammin_;
  int minRng_;  // 8 bit scale
  int cyb_, cyg_, cyr_;  // Rec.601 RGB -> luma, 16.16 fixed point

  mutable std::mutex mutex_;
  mutable std::vector<Slot> slots_;
};

}  // namespace autolevels
