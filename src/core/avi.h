// Motion-JPEG AVI writer (frames encoded as JPEG, indexed AVI 1.0 container).
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace veraa {

class AviWriter {
 public:
  AviWriter() = default;
  AviWriter(const AviWriter&) = delete;
  AviWriter& operator=(const AviWriter&) = delete;
  ~AviWriter();

  // quality: JPEG quality 1..100.
  bool open(const std::string& path, int width, int height, double fps, int quality = 90);
  // Adds a frame of width*height pixels with `components` bytes each (3 = RGB, 4 = RGBA), top row first.
  bool addFrame(const uint8_t* pixels, int components);
  // Writes the index and the final header sizes. Called by the destructor if needed.
  bool close();
  bool isOpen() const { return file_ != nullptr; }
  int frameCount() const { return static_cast<int>(index_.size()); }
  const std::string& error() const { return error_; }

 private:
  struct IndexEntry {
    uint32_t offset, size;
  };
  std::FILE* file_ = nullptr;
  int width_ = 0, height_ = 0, quality_ = 90;
  double fps_ = 30.0;
  long riffSizePos_ = 0, totalFramesPos_ = 0, lengthPos_ = 0, moviSizePos_ = 0, moviStart_ = 0;
  long suggestedPos_[2] = {0, 0};
  uint32_t maxFrame_ = 0;
  std::vector<IndexEntry> index_;
  std::string error_;
};

// Encodes RGB(A) pixels as a JPEG in memory.
bool encodeJpeg(const uint8_t* pixels, int width, int height, int components, int quality, std::vector<uint8_t>& out);

}  // namespace veraa
