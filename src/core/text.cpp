// SPDX-License-Identifier: GPL-2.0-or-later
// Debug text overlay: port of DDigitS() from DDigit v1.07 by StainlessS (horizontal text at pixel coordinates,
// control codes, 7/8 background fade), for every layout and bit depth of the engine.
#include <cstdint>
#include <cstring>

#include "core/autolevels.hpp"
#include "core/font.hpp"

namespace autolevels {

namespace {

// Colour table: 16 colours plus 16 grey levels; RGB as 0xRRGGBB, YUV as 0xYYUUVV (limited range).
const uint32_t kCmapRGB[32] = {
    0XA9A9A9, 0X1E90FF, 0XFF4500, 0XDA70D6, 0X00FF00, 0X7FFFD4, 0XFFFF00, 0XFFFFFF,
    0XC0C0C0, 0X6495ED, 0XFFA500, 0XDDA0DD, 0X7FFF00, 0XB0E0E6, 0XFFD700, 0XDCDCDC,
    0X000000, 0X111111, 0X222222, 0X333333, 0X444444, 0X555555, 0X666666, 0X777777,
    0X888888, 0X999999, 0XAAAAAA, 0XBBBBBB, 0XCCCCCC, 0XDDDDDD, 0XEEEEEE, 0XFFFFFF};
const uint32_t kCmapYUV[32] = {
    0XA18080, 0X79C246, 0X7446D7, 0X959DA7, 0X913622, 0XC6804B, 0XD21092, 0XEB8080,
    0XB58080, 0X8CAE64, 0XA52AB3, 0XAF9296, 0XB1235A, 0XC58A6A, 0XBE1CA1, 0XCD8080,
    0X108080, 0X1F8080, 0X2D8080, 0X3C8080, 0X4A8080, 0X598080, 0X688080, 0X768080,
    0X858080, 0X938080, 0XA28080, 0XB18080, 0XBF8080, 0XCE8080, 0XDC8080, 0XEB8080};

// default / hilite colour index: colour clips, luma-only clips
const int kCmapDef[2][2] = {{7, 10}, {28, 31}};

const int kTab = 4;

}  // namespace

template <typename T>
void Engine::drawText(const Frame& dst, int x, int y, const char* s) const {
  const int width = fmt_.width;
  const int height = fmt_.height;
  const int shift = fmt_.bits - 8;
  const bool rgb = fmt_.isRGB();
  const bool planarYUV = fmt_.layout == Layout::PlanarYUV;
  const int in_x = x;
  const int cmapix = fmt_.layout == Layout::Gray ? 1 : 0;

  // 8 bit colour -> sample value: YUV by bit shift, RGB to full range
  auto level = [&](uint32_t v) -> int {
    return rgb ? int((int64_t(v) * maxv_ * 2 + 255) / 510) : int(v << shift);
  };
  int c1 = 0, c2 = 0, c3 = 0;
  auto setColor = [&](int index) {
    const uint32_t c = rgb ? kCmapRGB[index] : kCmapYUV[index];
    c1 = level((c >> 16) & 0xFF);
    c2 = level((c >> 8) & 0xFF);
    c3 = level(c & 0xFF);
  };
  const int startColor = kCmapDef[cmapix][0];
  setColor(startColor);

  // background fade 7/8 (DDIGIT_BACKGROUNDFADE 0)
  const int fadeY = 2 << shift, fadeC = 16 << shift;
  auto bgY = [&](T& v) { v = T(((v * 7) >> 3) + fadeY); };
  auto bgC = [&](T& v) { v = T(((v * 7) >> 3) + fadeC); };
  auto bgRGB = [&](T& v) { v = T((v * 7) >> 3); };

  const int xSubS = 1 << fmt_.ssw, ySubS = 1 << fmt_.ssh;
  const unsigned fontmask = (0xFFFF0000u >> xSubS) & 0xFFFF;

  const size_t length = std::strlen(s);
  for (size_t s_ix = 0; s_ix < length; ++s_ix) {
    int num = static_cast<unsigned char>(s[s_ix]);
    if (num <= '\r' && num >= '\a') {  // control character
      if (num == '\a') {               // colour control: \a followed by 0-9, A-V, '-' default, '!' hilite, '*' start
        int cc = s[s_ix + 1];
        if (cc >= 'a' && cc <= 'v') cc -= ('a' - 'A');
        static const char colstr[] = "0123456789ABCDEFGHIJKLMNOPQRSTUV-!*";
        const char* ccp = cc ? std::strchr(colstr, cc) : nullptr;
        if (ccp) {
          ++s_ix;
          cc = int(ccp - colstr);
          if (cc == 32)
            cc = kCmapDef[cmapix][0];
          else if (cc == 33)
            cc = kCmapDef[cmapix][1];
          else if (cc == 34)
            cc = startColor;
          setColor(cc);
          continue;
        }
      } else {
        bool handled = true;
        switch (num) {
          case '\n':
            x = 0;
            y += kCharHeight;
            break;
          case '\r':
            x = in_x;
            y += kCharHeight;
            break;
          case '\t':
            x += (kTab * kCharWidth) - (x % (kTab * kCharWidth));
            break;
          case '\f':
            x += kCharWidth;
            break;
          case '\b':
            x -= kCharWidth;
            break;
          default:
            handled = false;
        }
        if (handled) continue;
      }
    }

    if (x < width && x > -kCharWidth && y < height && y > -kCharHeight) {
      num = (num - ' ') & 0xFF;
      if (num >= kFontChars) num = 0;  // not in the font: space
      const unsigned short* glyph = kFont[num];

      int xx = x, yy = y;
      int txlft = 0, txrgt = kCharWidth;
      if (xx < 0) {
        txlft = -xx;
        xx = 0;
      } else if (xx + kCharWidth > width) {
        txrgt = width - xx;
      }
      int tytop = 0, tybot = kCharHeight;
      if (yy < 0) {
        tytop = -yy;
        yy = 0;
      } else if (yy + kCharHeight > height) {
        tybot = height - yy;
      }

      switch (fmt_.layout) {
        case Layout::Gray:
        case Layout::PlanarYUV: {
          for (int ty = tytop; ty < tybot; ++ty) {
            T* rp = reinterpret_cast<T*>(dst.data[0] + (yy + ty - tytop) * dst.stride[0]) + xx;
            unsigned fbm = unsigned(glyph[ty]) << txlft;
            for (int tx = txlft; tx < txrgt; ++tx, ++rp, fbm <<= 1) {
              if (fbm & 0x8000)
                *rp = T(c1);
              else
                bgY(*rp);
            }
          }
          if (!planarYUV) break;
          // Chroma: the font bitmap is shifted so chroma stays aligned with luma when x or y is not a multiple
          // of the subsampling; rows covered by one chroma sample are OR-ed together.
          const unsigned rshift = unsigned(xx & (xSubS - 1));
          const unsigned dshift = unsigned(yy & (ySubS - 1));
          unsigned yStep = unsigned(ySubS) - dshift;
          int crow = yy / ySubS;
          for (int ty = tytop; ty < tybot; ty += int(yStep), yStep = unsigned(ySubS), ++crow) {
            T* rpu = reinterpret_cast<T*>(dst.data[1] + crow * dst.stride[1]) + xx / xSubS;
            T* rpv = reinterpret_cast<T*>(dst.data[2] + crow * dst.stride[2]) + xx / xSubS;
            unsigned fbm = 0;
            int me = ty + int(yStep);
            if (me > tybot) me = tybot;
            for (int m = ty; m < me; ++m) fbm |= glyph[m];
            fbm <<= txlft;
            fbm >>= rshift;
            for (int tx = txlft; tx < txrgt; tx += xSubS, ++rpu, ++rpv, fbm <<= xSubS) {
              if (fbm & fontmask) {
                *rpu = T(c2);
                *rpv = T(c3);
              } else {
                bgC(*rpu);
                bgC(*rpv);
              }
            }
          }
          break;
        }
        case Layout::YUY2: {
          for (int ty = tytop; ty < tybot; ++ty) {
            uint8_t* line = dst.data[0] + (yy + ty - tytop) * dst.stride[0];
            unsigned short fbm = static_cast<unsigned short>(glyph[ty] << txlft);
            for (int tx = txlft, px = xx; tx < txrgt; ++tx, ++px, fbm = static_cast<unsigned short>(fbm << 1)) {
              T* rp = reinterpret_cast<T*>(line) + 2 * px;
              const bool odd = px & 1;  // [Y0 U Y1 V]
              T& u = odd ? rp[-1] : rp[1];
              T& v = odd ? rp[1] : rp[3];
              if (fbm & 0x8000) {
                rp[0] = T(c1);
                u = T(c2);
                v = T(c3);
              } else {
                bgY(rp[0]);
                bgC(u);
                bgC(v);
              }
            }
          }
          break;
        }
        case Layout::PackedBGR24:
        case Layout::PackedBGR32: {
          const int step = fmt_.layout == Layout::PackedBGR32 ? 4 : 3;
          for (int ty = tytop; ty < tybot; ++ty) {
            // bottom row first in memory
            T* rp = reinterpret_cast<T*>(dst.data[0] + (height - 1 - (yy + ty - tytop)) * dst.stride[0]) + xx * step;
            unsigned short fbm = static_cast<unsigned short>(glyph[ty] << txlft);
            for (int tx = txlft; tx < txrgt; ++tx, rp += step, fbm = static_cast<unsigned short>(fbm << 1)) {
              if (fbm & 0x8000U) {
                rp[0] = T(c3);
                rp[1] = T(c2);
                rp[2] = T(c1);
              } else {
                bgRGB(rp[0]);
                bgRGB(rp[1]);
                bgRGB(rp[2]);
              }
            }
          }
          break;
        }
        case Layout::PlanarRGB: {
          for (int ty = tytop; ty < tybot; ++ty) {
            const int r = yy + ty - tytop;
            T* pr = reinterpret_cast<T*>(dst.data[0] + r * dst.stride[0]) + xx;
            T* pg = reinterpret_cast<T*>(dst.data[1] + r * dst.stride[1]) + xx;
            T* pb = reinterpret_cast<T*>(dst.data[2] + r * dst.stride[2]) + xx;
            unsigned short fbm = static_cast<unsigned short>(glyph[ty] << txlft);
            for (int tx = txlft; tx < txrgt; ++tx, ++pr, ++pg, ++pb, fbm = static_cast<unsigned short>(fbm << 1)) {
              if (fbm & 0x8000U) {
                *pr = T(c1);
                *pg = T(c2);
                *pb = T(c3);
              } else {
                bgRGB(*pr);
                bgRGB(*pg);
                bgRGB(*pb);
              }
            }
          }
          break;
        }
      }
    }
    x += kCharWidth;
  }
}

template void Engine::drawText<uint8_t>(const Frame&, int, int, const char*) const;
template void Engine::drawText<uint16_t>(const Frame&, int, int, const char*) const;

}  // namespace autolevels
