#include "DisplayLogic.h"
#include <cassert>
#include <iostream>
#include <limits>
#include <string>

using namespace followers;

struct BoundedCanvas {
  int calls = 0;
  void point(int x, int y) { assert(x >= 0 && x < 128 && y >= 28 && y < 106); }
  void fillRect(int x, int y, int w, int h, uint32_t) {
    assert(w > 0 && h > 0);
    point(x, y); point(x + w - 1, y + h - 1); ++calls;
  }
  void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t) {
    point(x0, y0); point(x1, y1); point(x2, y2); ++calls;
  }
  void fillCircle(int x, int y, int radius, uint32_t) {
    point(x - radius, y - radius); point(x + radius, y + radius); ++calls;
  }
};

int main() {
  for (size_t count = 1; count <= 20; ++count) {
    auto layout = digitLayout(count);
    assert(layout.totalWidth <= 116 && layout.x >= 6);
    assert(layout.height <= 68);
    std::string number(count, '8');  // All segments on exercises every boundary.
    BoundedCanvas canvas;
    drawSevenSegment(canvas, number.c_str(), 0xA8FF60);
    assert(canvas.calls > 0);
  }
  for (const char* number : {"748", "1000", "12345", "18446744073709551615"}) {
    BoundedCanvas canvas;
    drawSevenSegment(canvas, number, 0xA8FF60);
  }
  struct FormatCase { uint64_t count; const char* text; const char* unit; };
  const FormatCase cases[] = {
    {749, "749", ""}, {1234, "1234", ""}, {99999, "99999", ""},
    {100000, "100.00", "K"}, {158000, "158.00", "K"},
    {999994, "999.99", "K"}, {999995, "1.00", "M"},
    {1000000, "1.00", "M"}, {1234567, "1.23", "M"},
    {15800000, "15.80", "M"}, {1000000000, "1.00", "B"},
    {UINT64_MAX, "18.45", "E"}
  };
  for (const auto& test : cases) {
    const auto formatted = formatFollowers(test.count);
    assert(!strcmp(formatted.text, test.text) && !strcmp(formatted.unit, test.unit));
    BoundedCanvas canvas;
    const int reserved = formatted.unit[0] ? 20 : 0;
    const auto layout = drawSevenSegment(canvas, formatted.text, 0xA8FF60, reserved);
    assert(layout.x >= 6 && layout.x + layout.totalWidth + reserved <= 122);
  }
  RotationFilter filter;
  uint16_t angle = 0;
  filter.reset(angle);
  assert(!filter.sample(1, 0, 0, 0, angle));
  assert(!filter.sample(1, 0, 0, 599, angle));
  assert(filter.sample(1, 0, 0, 600, angle) && angle == 90);
  assert(!filter.sample(1, 0, 0, 900, angle));
  filter.reset(angle);
  for (uint32_t now = 0; now < 2000; now += 50) {
    assert(!filter.sample(0, 0, 1, now, angle));
    assert(angle == 90);  // Flat holds the last orientation.
  }
  for (uint16_t target : {0, 90, 180, 270}) {
    angle = (target + 180) % 360;
    filter.reset(angle);
    const float x = target == 90 ? 1 : target == 270 ? -1 : 0;
    const float y = target == 0 ? 1 : target == 180 ? -1 : 0;
    for (uint32_t now = 0; now <= 1000; now += 50) filter.sample(x, y, 0, now, angle);
    assert(angle == target);
  }
  angle = 0;
  filter.reset(angle);
  for (uint32_t now = 0; now < 2000; now += 50) {
    assert(!filter.sample(0.71f, 0.70f, 0, now, angle)); // Diagonal dead zone.
  }
  filter.reset(angle);
  assert(!filter.sample(1, 0, 0, 0, angle));
  assert(!filter.sample(2, 0, 0, 400, angle));  // Shake resets pending transition.
  assert(!filter.sample(1, 0, 0, 600, angle));
  assert(!filter.sample(1, 0, 0, 1199, angle));
  assert(filter.sample(1, 0, 0, 1200, angle));
  assert(!filter.sample(std::numeric_limits<float>::quiet_NaN(), 0, 0, 1500, angle));
  angle = 0;
  filter.reset(angle);
  assert(!filter.sample(1, 0, 0, 0xFFFFFF00, angle));
  assert(filter.sample(1, 0, 0, 0x00000160, angle) && angle == 90);
  std::cout << "PASS: 1-20 digit rendering fits the screen; auto rotation stability, "
               "four directions, flat/diagonal/shake rejection and time rollover\n";
}
