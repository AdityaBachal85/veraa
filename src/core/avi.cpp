#include "avi.h"

#include <cmath>
#include <cstring>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb/stb_image_write.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace veraa {
namespace {

void put32(std::FILE* f, uint32_t v) {
  uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
                  static_cast<uint8_t>(v >> 24)};
  std::fwrite(b, 1, 4, f);
}

void put16(std::FILE* f, uint16_t v) {
  uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
  std::fwrite(b, 1, 2, f);
}

void fourcc(std::FILE* f, const char* c) { std::fwrite(c, 1, 4, f); }

void patch32(std::FILE* f, long pos, uint32_t v) {
  long cur = std::ftell(f);
  std::fseek(f, pos, SEEK_SET);
  put32(f, v);
  std::fseek(f, cur, SEEK_SET);
}

void appendBytes(void* context, void* data, int size) {
  auto* out = static_cast<std::vector<uint8_t>*>(context);
  const auto* p = static_cast<const uint8_t*>(data);
  out->insert(out->end(), p, p + size);
}

std::FILE* openFile(const std::string& path) {
#if defined(_WIN32)
  // UTF-8 path to UTF-16.
  std::wstring w;
  for (size_t i = 0; i < path.size();) {
    uint32_t c = static_cast<uint8_t>(path[i]);
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    c &= extra == 3 ? 0x07 : extra == 2 ? 0x0F : extra == 1 ? 0x1F : 0x7F;
    for (int k = 1; k <= extra && i + k < path.size(); ++k) c = (c << 6) | (static_cast<uint8_t>(path[i + k]) & 0x3F);
    i += static_cast<size_t>(extra) + 1;
    if (c >= 0x10000) {
      c -= 0x10000;
      w.push_back(static_cast<wchar_t>(0xD800 + (c >> 10)));
      w.push_back(static_cast<wchar_t>(0xDC00 + (c & 0x3FF)));
    } else {
      w.push_back(static_cast<wchar_t>(c));
    }
  }
  return _wfopen(w.c_str(), L"wb");
#else
  return std::fopen(path.c_str(), "wb");
#endif
}

}  // namespace

bool encodeJpeg(const uint8_t* pixels, int width, int height, int components, int quality, std::vector<uint8_t>& out) {
  out.clear();
  if (!pixels || width <= 0 || height <= 0 || (components != 3 && components != 4)) return false;
  std::vector<uint8_t> rgb;
  const uint8_t* src = pixels;
  if (components == 4) {
    rgb.resize(static_cast<size_t>(width) * height * 3);
    for (size_t i = 0, n = static_cast<size_t>(width) * height; i < n; ++i) std::memcpy(&rgb[i * 3], &pixels[i * 4], 3);
    src = rgb.data();
  }
  return stbi_write_jpg_to_func(appendBytes, &out, width, height, 3, src, quality) != 0 && !out.empty();
}

AviWriter::~AviWriter() { close(); }

bool AviWriter::open(const std::string& path, int width, int height, double fps, int quality) {
  close();
  error_.clear();
  index_.clear();
  maxFrame_ = 0;
  if (width <= 0 || height <= 0 || width > 65535 || height > 65535 || fps <= 0.0) {
    error_ = "Invalid video size or frame rate";
    return false;
  }
  file_ = openFile(path);
  if (!file_) {
    error_ = "Cannot create " + path;
    return false;
  }
  width_ = width;
  height_ = height;
  fps_ = fps;
  quality_ = quality < 1 ? 1 : quality > 100 ? 100 : quality;
  std::FILE* f = file_;
  uint32_t rate = static_cast<uint32_t>(std::lround(fps * 1000.0));

  fourcc(f, "RIFF");
  riffSizePos_ = std::ftell(f);
  put32(f, 0);
  fourcc(f, "AVI ");
  // hdrl: avih (8 + 56) + strl list (12 + strh (8 + 56) + strf (8 + 40)).
  fourcc(f, "LIST");
  put32(f, 4 + (8 + 56) + (12 + (8 + 56) + (8 + 40)));
  fourcc(f, "hdrl");
  fourcc(f, "avih");
  put32(f, 56);
  put32(f, static_cast<uint32_t>(std::lround(1e6 / fps)));  // microseconds per frame
  put32(f, 0);                                              // max bytes per second
  put32(f, 0);                                              // padding granularity
  put32(f, 0x10);                                           // has index
  totalFramesPos_ = std::ftell(f);
  put32(f, 0);  // total frames
  put32(f, 0);  // initial frames
  put32(f, 1);  // streams
  suggestedPos_[0] = std::ftell(f);
  put32(f, 0);  // suggested buffer size
  put32(f, static_cast<uint32_t>(width));
  put32(f, static_cast<uint32_t>(height));
  for (int i = 0; i < 4; ++i) put32(f, 0);

  fourcc(f, "LIST");
  put32(f, 4 + (8 + 56) + (8 + 40));
  fourcc(f, "strl");
  fourcc(f, "strh");
  put32(f, 56);
  fourcc(f, "vids");
  fourcc(f, "MJPG");
  put32(f, 0);     // flags
  put16(f, 0);     // priority
  put16(f, 0);     // language
  put32(f, 0);     // initial frames
  put32(f, 1000);  // scale
  put32(f, rate);  // rate (rate / scale = fps)
  put32(f, 0);     // start
  lengthPos_ = std::ftell(f);
  put32(f, 0);  // length (frames)
  suggestedPos_[1] = std::ftell(f);
  put32(f, 0);            // suggested buffer size
  put32(f, 0xFFFFFFFFu);  // quality (default)
  put32(f, 0);            // sample size
  put16(f, 0);
  put16(f, 0);
  put16(f, static_cast<uint16_t>(width));
  put16(f, static_cast<uint16_t>(height));

  fourcc(f, "strf");
  put32(f, 40);
  put32(f, 40);  // bitmap info header size
  put32(f, static_cast<uint32_t>(width));
  put32(f, static_cast<uint32_t>(height));
  put16(f, 1);   // planes
  put16(f, 24);  // bit count
  fourcc(f, "MJPG");
  put32(f, static_cast<uint32_t>(width * height * 3));
  put32(f, 0);
  put32(f, 0);
  put32(f, 0);
  put32(f, 0);

  fourcc(f, "LIST");
  moviSizePos_ = std::ftell(f);
  put32(f, 0);
  moviStart_ = std::ftell(f);
  fourcc(f, "movi");
  if (std::ferror(f)) {
    error_ = "Write error";
    std::fclose(file_);
    file_ = nullptr;
    return false;
  }
  return true;
}

bool AviWriter::addFrame(const uint8_t* pixels, int components) {
  if (!file_) return false;
  std::vector<uint8_t> jpg;
  if (!encodeJpeg(pixels, width_, height_, components, quality_, jpg)) {
    error_ = "JPEG encoding failed";
    return false;
  }
  uint32_t size = static_cast<uint32_t>(jpg.size());
  long chunkPos = std::ftell(file_);
  fourcc(file_, "00dc");
  put32(file_, size);
  std::fwrite(jpg.data(), 1, jpg.size(), file_);
  if (size & 1) std::fputc(0, file_);
  index_.push_back({static_cast<uint32_t>(chunkPos - moviStart_), size});
  if (size > maxFrame_) maxFrame_ = size;
  if (std::ferror(file_)) {
    error_ = "Write error (disk full?)";
    return false;
  }
  return true;
}

bool AviWriter::close() {
  if (!file_) return true;
  std::FILE* f = file_;
  long moviEnd = std::ftell(f);
  patch32(f, moviSizePos_, static_cast<uint32_t>(moviEnd - moviStart_));
  fourcc(f, "idx1");
  put32(f, static_cast<uint32_t>(index_.size() * 16));
  for (const auto& e : index_) {
    fourcc(f, "00dc");
    put32(f, 0x10);  // key frame
    put32(f, e.offset);
    put32(f, e.size);
  }
  long end = std::ftell(f);
  patch32(f, riffSizePos_, static_cast<uint32_t>(end - 8));
  patch32(f, totalFramesPos_, static_cast<uint32_t>(index_.size()));
  patch32(f, lengthPos_, static_cast<uint32_t>(index_.size()));
  patch32(f, suggestedPos_[0], maxFrame_ + 8);
  patch32(f, suggestedPos_[1], maxFrame_ + 8);
  bool ok = !std::ferror(f);
  ok = std::fclose(f) == 0 && ok;
  file_ = nullptr;
  if (!ok) error_ = "Write error";
  return ok;
}

}  // namespace veraa
