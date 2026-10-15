#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace followers {

struct DigitLayout {
  int x, y, width, height, gap, thickness, totalWidth;
};

struct DisplayNumber {
  char text[24] = {};
  char unit[2] = {};
};

inline DisplayNumber formatFollowers(uint64_t count) {
  DisplayNumber result;
  if (count < 100000) {
    snprintf(result.text, sizeof(result.text), "%llu", static_cast<unsigned long long>(count));
    return result;
  }
  static constexpr char units[] = " KMBTQE";
  uint64_t base = 1;
  size_t unit = 0;
  while (unit < 6 && count / base >= 1000) { base *= 1000; ++unit; }
  uint64_t whole = count / base;
  // All unit bases are divisible by 100. Avoid count*100, which could overflow.
  const uint64_t quantum = base / 100;
  uint64_t decimals = (count % base + quantum / 2) / quantum;
  if (decimals == 100) { ++whole; decimals = 0; }
  if (whole == 1000 && unit < 6) { ++unit; whole = 1; }
  snprintf(result.text, sizeof(result.text), "%llu.%02u",
           static_cast<unsigned long long>(whole), static_cast<unsigned>(decimals));
  result.unit[0] = units[unit];
  return result;
}

inline DigitLayout digitLayout(size_t digits, int availableWidth = 116) {
  if (!digits) digits = 1;
  // Even UINT64_MAX (20 digits) fits. Three to five digits are the primary UI.
  int gap = digits <= 5 ? 4 : 1;
  int width = std::min(36, (availableWidth - (static_cast<int>(digits) - 1) * gap) / static_cast<int>(digits));
  width = std::max(2, width);
  int height = std::min(68, width * 2);
  int total = width * static_cast<int>(digits) + (static_cast<int>(digits) - 1) * gap;
  return {(128 - total) / 2, 66 - height / 2, width, height, gap,
          std::max(1, width / 6), total};
}

// Seven-segment geometry is sized directly on the pixel grid. No bitmap font
// is stretched or resampled, so the small screen keeps consistent crisp edges.
template <typename Canvas>
inline DigitLayout drawSevenSegment(Canvas& canvas, const char* number, uint32_t color,
                                    int reservedRight = 0) {
  static constexpr uint8_t masks[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
  size_t digits = 0;
  bool hasPoint = false;
  for (const char* p = number; *p; ++p) {
    if (*p >= '0' && *p <= '9') ++digits;
    if (*p == '.') hasPoint = true;
  }
  const int pointWidth = hasPoint ? 5 : 0;
  DigitLayout layout = digitLayout(digits, 116 - reservedRight - pointWidth);
  layout.totalWidth += pointWidth;
  layout.x = (128 - layout.totalWidth - reservedRight) / 2;
  const int t = layout.thickness, cap = t / 2;
  auto horizontal = [&](int x, int y, int length) {
    canvas.fillRect(x + cap, y, length - 2 * cap, t, color);
    if (cap) {
      canvas.fillTriangle(x, y + t / 2, x + cap, y, x + cap, y + t - 1, color);
      canvas.fillTriangle(x + length - 1, y + t / 2, x + length - cap - 1, y,
                          x + length - cap - 1, y + t - 1, color);
    }
  };
  auto vertical = [&](int x, int y, int length) {
    canvas.fillRect(x, y + cap, t, length - 2 * cap, color);
    if (cap) {
      canvas.fillTriangle(x + t / 2, y, x, y + cap, x + t - 1, y + cap, color);
      canvas.fillTriangle(x + t / 2, y + length - 1, x, y + length - cap - 1,
                          x + t - 1, y + length - cap - 1, color);
    }
  };
  int cursor = layout.x;
  for (size_t i = 0; number[i]; ++i) {
    if (number[i] == '.') {
      canvas.fillCircle(cursor + pointWidth / 2, layout.y + layout.height - 2, 1, color);
      cursor += pointWidth;
      continue;
    }
    if (number[i] < '0' || number[i] > '9') continue;
    const uint8_t mask = masks[number[i] - '0'];
    const int x = cursor;
    cursor += layout.width + layout.gap;
    const int y = layout.y, w = layout.width, h = layout.height;
    const int middle = (h - t) / 2;
    const int upperLength = middle - t;
    const int lowerY = middle + t;
    const int lowerLength = h - t - lowerY;
    if (mask & 0x01) horizontal(x + t / 2, y, w - t);
    if (mask & 0x02) vertical(x + w - t, y + t, upperLength);
    if (mask & 0x04) vertical(x + w - t, y + lowerY, lowerLength);
    if (mask & 0x08) horizontal(x + t / 2, y + h - t, w - t);
    if (mask & 0x10) vertical(x, y + lowerY, lowerLength);
    if (mask & 0x20) vertical(x, y + t, upperLength);
    if (mask & 0x40) horizontal(x + t / 2, y + middle, w - t);
  }
  return layout;
}

class RotationFilter {
 public:
  static constexpr uint32_t STABLE_MS = 600;
  void reset(uint16_t current) {
    initialized_ = false;
    pending_ = false;
    candidate_ = current;
  }
  bool sample(float ax, float ay, float az, uint32_t now, uint16_t& rotation) {
    const float magnitude2 = ax * ax + ay * ay + az * az;
    if (!std::isfinite(magnitude2) || magnitude2 < 0.65f * 0.65f || magnitude2 > 1.35f * 1.35f) {
      pending_ = false;
      initialized_ = false;
      return false;
    }
    if (!initialized_) { x_ = ax; y_ = ay; z_ = az; initialized_ = true; }
    else {
      x_ += (ax - x_) * 0.25f;
      y_ += (ay - y_) * 0.25f;
      z_ += (az - z_) * 0.25f;
    }
    const float x = std::fabs(x_), y = std::fabs(y_);
    // Flat, diagonal or weak gravity projections cannot establish orientation.
    if (std::fabs(z_) > 0.80f || std::max(x, y) < 0.55f || std::fabs(x - y) < 0.20f) {
      pending_ = false;
      return false;
    }
    uint16_t proposed = x > y ? (x_ > 0 ? 90 : 270) : (y_ > 0 ? 0 : 180);
    if (proposed == rotation) { pending_ = false; return false; }
    if (!pending_ || proposed != candidate_) {
      candidate_ = proposed;
      since_ = now;
      pending_ = true;
      return false;
    }
    if (now - since_ < STABLE_MS) return false;
    rotation = proposed;
    pending_ = false;
    return true;
  }
 private:
  float x_ = 0, y_ = 0, z_ = 0;
  uint32_t since_ = 0;
  uint16_t candidate_ = 0;
  bool initialized_ = false, pending_ = false;
};

}  // namespace followers
