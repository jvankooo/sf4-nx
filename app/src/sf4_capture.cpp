// sf4 - PNG captures of the game image (see sf4_capture.h).
//
// The PNG is written uncompressed (deflate "stored" blocks): the SDK does not include stb_image_write and this is
// enough for testing. A 1280x720 capture takes about 2.8 MB.

#include "sf4_capture.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

REXCVAR_DEFINE_INT32(sf4_capture_every_s, 0, "SF4",
                     "Save a PNG of the game image every N seconds in captures/ next to the executable "
                     "(0 = never; tests only)")
    .range(0, 3600);
REXCVAR_DEFINE_INT32(sf4_capture_max, 30, "SF4", "Maximum captures per run").range(1, 1000);

namespace sf4::capture {
namespace {

std::mutex g_mutex;
std::condition_variable g_cv;
bool g_stop = false;
std::thread g_thread;

uint32_t Crc32(const uint8_t* data, size_t n, uint32_t crc) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      t[i] = c;
    }
    return t;
  }();
  for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return crc;
}

void PutBe32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(uint8_t(v >> 24));
  out.push_back(uint8_t(v >> 16));
  out.push_back(uint8_t(v >> 8));
  out.push_back(uint8_t(v));
}

void Chunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& data) {
  PutBe32(out, uint32_t(data.size()));
  const size_t type_start = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  PutBe32(out, Crc32(out.data() + type_start, out.size() - type_start, 0xFFFFFFFFu) ^ 0xFFFFFFFFu);
}

// RGBX (R8 G8 B8 X8, like rex::ui::RawImage) to an 8-bit RGB PNG.
std::vector<uint8_t> EncodePng(const rex::ui::RawImage& image) {
  const uint32_t width = image.width, height = image.height;
  std::vector<uint8_t> raw;
  raw.reserve(size_t(height) * (size_t(width) * 3 + 1));
  for (uint32_t y = 0; y < height; ++y) {
    raw.push_back(0);  // no filter
    const uint8_t* row = image.data.data() + size_t(y) * image.stride;
    for (uint32_t x = 0; x < width; ++x) {
      raw.push_back(row[x * 4]);
      raw.push_back(row[x * 4 + 1]);
      raw.push_back(row[x * 4 + 2]);
    }
  }
  // zlib with "stored" blocks and Adler-32 at the end.
  std::vector<uint8_t> zlib = {0x78, 0x01};
  size_t pos = 0;
  do {
    const size_t piece = std::min<size_t>(65535, raw.size() - pos);
    zlib.push_back(pos + piece == raw.size() ? 1 : 0);
    zlib.push_back(uint8_t(piece));
    zlib.push_back(uint8_t(piece >> 8));
    zlib.push_back(uint8_t(~piece));
    zlib.push_back(uint8_t(~piece >> 8));
    zlib.insert(zlib.end(), raw.begin() + pos, raw.begin() + pos + piece);
    pos += piece;
  } while (pos < raw.size());
  uint32_t a = 1, b = 0;
  for (uint8_t byte : raw) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  PutBe32(zlib, (b << 16) | a);

  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> ihdr;
  PutBe32(ihdr, width);
  PutBe32(ihdr, height);
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8 bits, RGB, deflate, adaptive filter, no interlace
  Chunk(png, "IHDR", ihdr);
  Chunk(png, "IDAT", zlib);
  Chunk(png, "IEND", {});
  return png;
}

void Loop(std::function<rex::ui::Presenter*()> get_presenter, int every_s, int max_count) {
  const std::filesystem::path folder = rex::filesystem::GetExecutableFolder() / "captures";
  const auto start = std::chrono::steady_clock::now();
  for (int n = 1; n <= max_count; ++n) {
    {
      std::unique_lock<std::mutex> lock(g_mutex);
      if (g_cv.wait_for(lock, std::chrono::seconds(every_s), [] { return g_stop; })) return;
    }
    rex::ui::Presenter* presenter = get_presenter ? get_presenter() : nullptr;
    rex::ui::RawImage image;
    if (!presenter || !presenter->CaptureGuestOutput(image) || !image.width || !image.height) {
      REXLOG_WARN("[capture] {}: no game image to capture", n);
      continue;
    }
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const long long seconds =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
    char name[64];
    std::snprintf(name, sizeof(name), "capture_%03d_%04llds.png", n, seconds);
    const std::filesystem::path path = folder / name;
    const std::vector<uint8_t> png = EncodePng(image);
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
    if (!file) {
      REXLOG_WARN("[capture] could not write {}", rex::path_to_utf8(path));
      continue;
    }
    REXLOG_INFO("[capture] {} ({}x{})", rex::path_to_utf8(path), image.width, image.height);
  }
}

}  // namespace

void Start(std::function<rex::ui::Presenter*()> get_presenter) {
  const int every_s = REXCVAR_GET(sf4_capture_every_s);
  if (every_s <= 0 || g_thread.joinable()) return;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stop = false;
  }
  g_thread = std::thread(Loop, std::move(get_presenter), every_s, int(REXCVAR_GET(sf4_capture_max)));
  REXLOG_INFO("[capture] one capture every {} s (at most {})", every_s, int(REXCVAR_GET(sf4_capture_max)));
}

void Stop() {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stop = true;
  }
  g_cv.notify_all();
  if (g_thread.joinable()) g_thread.join();
}

}  // namespace sf4::capture
