#include "cfb.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <functional>

namespace veraa {
namespace cfb {

namespace {

constexpr uint32_t kFree = 0xFFFFFFFFu;
constexpr uint32_t kEnd = 0xFFFFFFFEu;
constexpr uint32_t kMaxRegular = 0xFFFFFFFAu;

uint16_t u16(const std::vector<uint8_t>& d, size_t o) {
  return o + 2 <= d.size() ? static_cast<uint16_t>(d[o] | (d[o + 1] << 8)) : 0;
}
uint32_t u32(const std::vector<uint8_t>& d, size_t o) {
  if (o + 4 > d.size()) return 0;
  return static_cast<uint32_t>(d[o]) | (static_cast<uint32_t>(d[o + 1]) << 8) | (static_cast<uint32_t>(d[o + 2]) << 16) |
         (static_cast<uint32_t>(d[o + 3]) << 24);
}

void appendUtf8(std::string& s, uint32_t c) {
  if (c < 0x80) {
    s += static_cast<char>(c);
  } else if (c < 0x800) {
    s += static_cast<char>(0xC0 | (c >> 6));
    s += static_cast<char>(0x80 | (c & 0x3F));
  } else {
    s += static_cast<char>(0xE0 | (c >> 12));
    s += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    s += static_cast<char>(0x80 | (c & 0x3F));
  }
}

bool fail(std::string* error, const std::string& msg) {
  if (error) *error = msg;
  return false;
}

}  // namespace

bool CompoundFile::openFile(const std::string& path, std::string* error) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return fail(error, "Cannot open " + path);
  f.seekg(0, std::ios::end);
  std::streamoff len = f.tellg();
  if (len < 0) return fail(error, "Cannot read " + path);
  f.seekg(0);
  std::vector<uint8_t> d(static_cast<size_t>(len));
  if (len > 0 && !f.read(reinterpret_cast<char*>(d.data()), len)) return fail(error, "Cannot read " + path);
  return open(std::move(d), error);
}

bool CompoundFile::sectorRange(uint32_t sector, size_t& offset) const {
  if (sector > kMaxRegular) return false;
  uint64_t o = (static_cast<uint64_t>(sector) + 1) * sectorSize_;
  if (o + sectorSize_ > data_.size()) {
    // A short last sector is tolerated (some writers truncate the file).
    if (o >= data_.size()) return false;
  }
  offset = static_cast<size_t>(o);
  return true;
}

bool CompoundFile::readChain(const std::vector<uint32_t>& fat, uint32_t start, size_t secSize, bool mini, uint64_t size,
                             std::vector<uint8_t>& out) const {
  out.clear();
  if (size == 0) return true;
  const std::vector<uint8_t>& src = mini ? miniStream_ : data_;
  uint64_t maxSteps = fat.size() + 1;
  if (size > static_cast<uint64_t>(src.size()) + secSize) return false;
  out.reserve(static_cast<size_t>(size));
  uint32_t s = start;
  for (uint64_t steps = 0; out.size() < size; ++steps) {
    if (steps > maxSteps || s >= fat.size() || s > kMaxRegular) return false;
    size_t off;
    if (mini) {
      off = static_cast<size_t>(s) * secSize;
      if (off >= src.size()) return false;
    } else if (!sectorRange(s, off)) {
      return false;
    }
    size_t n = std::min<size_t>(secSize, static_cast<size_t>(size - out.size()));
    n = std::min(n, src.size() - off);
    if (n == 0) return false;
    out.insert(out.end(), src.begin() + static_cast<std::ptrdiff_t>(off), src.begin() + static_cast<std::ptrdiff_t>(off + n));
    s = fat[s];
  }
  return true;
}

bool CompoundFile::open(std::vector<uint8_t> data, std::string* error) {
  data_ = std::move(data);
  fat_.clear();
  miniFat_.clear();
  miniStream_.clear();
  entries_.clear();
  streams_.clear();
  static const uint8_t sig[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
  if (data_.size() < 512 || std::memcmp(data_.data(), sig, 8) != 0)
    return fail(error, "Not a 3ds Max scene (no compound file signature)");
  uint16_t shift = u16(data_, 0x1E), miniShift = u16(data_, 0x20);
  if (shift != 9 && shift != 12) return fail(error, "Compound file: unsupported sector size");
  if (miniShift == 0 || miniShift > shift) return fail(error, "Compound file: bad mini sector size");
  sectorSize_ = size_t(1) << shift;
  miniSectorSize_ = size_t(1) << miniShift;
  uint32_t numFat = u32(data_, 0x2C), firstDir = u32(data_, 0x30);
  miniCutoff_ = u32(data_, 0x38);
  uint32_t firstMiniFat = u32(data_, 0x3C), numMiniFat = u32(data_, 0x40);
  uint32_t firstDifat = u32(data_, 0x44), numDifat = u32(data_, 0x48);
  size_t totalSectors = data_.size() / sectorSize_ + 1;
  if (numFat > totalSectors) return fail(error, "Compound file: corrupt header (FAT size)");

  // FAT sector list: 109 entries in the header, the rest in the DIFAT chain.
  std::vector<uint32_t> fatSectors;
  for (int i = 0; i < 109 && fatSectors.size() < numFat; ++i) {
    uint32_t s = u32(data_, 0x4C + i * 4);
    if (s <= kMaxRegular) fatSectors.push_back(s);
  }
  uint32_t dif = firstDifat;
  size_t perDifat = sectorSize_ / 4 - 1;
  for (uint32_t k = 0; k < numDifat && dif <= kMaxRegular && fatSectors.size() < numFat; ++k) {
    size_t off;
    if (!sectorRange(dif, off)) return fail(error, "Compound file: corrupt DIFAT");
    for (size_t i = 0; i < perDifat && fatSectors.size() < numFat; ++i) {
      uint32_t s = u32(data_, off + i * 4);
      if (s <= kMaxRegular) fatSectors.push_back(s);
    }
    dif = u32(data_, off + perDifat * 4);
  }
  for (uint32_t s : fatSectors) {
    size_t off;
    if (!sectorRange(s, off)) return fail(error, "Compound file: FAT sector out of range");
    for (size_t i = 0; i < sectorSize_ / 4; ++i) fat_.push_back(u32(data_, off + i * 4));
  }
  if (fat_.empty()) return fail(error, "Compound file: empty FAT");

  // Directory: walk its sector chain to the end (its length is not stored for v3 files).
  std::vector<uint8_t> dir;
  {
    uint32_t s = firstDir;
    for (size_t steps = 0; s <= kMaxRegular && steps <= fat_.size(); ++steps) {
      size_t off;
      if (s >= fat_.size() || !sectorRange(s, off)) break;
      size_t n = std::min(sectorSize_, data_.size() - off);
      dir.insert(dir.end(), data_.begin() + static_cast<std::ptrdiff_t>(off), data_.begin() + static_cast<std::ptrdiff_t>(off + n));
      s = fat_[s];
    }
  }
  if (dir.size() < 128) return fail(error, "Compound file: no directory");
  size_t count = dir.size() / 128;
  entries_.resize(count);
  for (size_t i = 0; i < count; ++i) {
    size_t o = i * 128;
    Entry& e = entries_[i];
    uint16_t nameLen = u16(dir, o + 0x40);
    std::string name;
    size_t chars = std::min<size_t>(nameLen / 2, 32);
    for (size_t k = 0; k < chars; ++k) {
      uint16_t c = u16(dir, o + k * 2);
      if (c == 0) break;
      appendUtf8(name, c);
    }
    e.name = name;
    e.type = dir[o + 0x42];
    e.left = u32(dir, o + 0x44);
    e.right = u32(dir, o + 0x48);
    e.child = u32(dir, o + 0x4C);
    e.start = u32(dir, o + 0x74);
    e.size = u32(dir, o + 0x78);
    if (sectorSize_ == 4096) e.size |= static_cast<uint64_t>(u32(dir, o + 0x7C)) << 32;
  }
  if (entries_[0].type != 5) return fail(error, "Compound file: missing root entry");

  // Mini FAT and mini stream (held by the root entry).
  std::vector<uint8_t> mf;
  readChain(fat_, firstMiniFat, sectorSize_, false, static_cast<uint64_t>(numMiniFat) * sectorSize_, mf);
  for (size_t i = 0; i + 4 <= mf.size(); i += 4) miniFat_.push_back(u32(mf, i));
  if (entries_[0].size > 0 && !readChain(fat_, entries_[0].start, sectorSize_, false, entries_[0].size, miniStream_))
    miniStream_.clear();

  // Walk the directory tree from the root (red-black tree siblings, child = storage contents).
  std::vector<bool> seen(count, false);
  std::function<void(uint32_t, const std::string&, int)> walk = [&](uint32_t idx, const std::string& prefix, int depth) {
    if (idx >= count || seen[idx] || depth > 64) return;
    seen[idx] = true;
    Entry& e = entries_[idx];
    walk(e.left, prefix, depth + 1);
    if (e.type == 2) {
      e.path = prefix + e.name;
      streams_.push_back(static_cast<int>(idx));
    } else if (e.type == 1) {
      walk(e.child, prefix + e.name + "/", depth + 1);
    }
    walk(e.right, prefix, depth + 1);
  };
  walk(entries_[0].child, "", 0);
  return true;
}

std::vector<std::string> CompoundFile::streamNames() const {
  std::vector<std::string> out;
  for (int i : streams_) out.push_back(entries_[i].path);
  return out;
}

bool CompoundFile::hasStream(const std::string& name) const {
  for (int i : streams_)
    if (entries_[i].path == name) return true;
  return false;
}

bool CompoundFile::readStream(const std::string& name, std::vector<uint8_t>& out, std::string* error) const {
  for (int i : streams_) {
    const Entry& e = entries_[i];
    if (e.path != name) continue;
    bool mini = e.size < miniCutoff_;
    bool ok = mini ? readChain(miniFat_, e.start, miniSectorSize_, true, e.size, out)
                   : readChain(fat_, e.start, sectorSize_, false, e.size, out);
    if (!ok) return fail(error, "Compound file: stream " + name + " is damaged");
    return true;
  }
  return fail(error, "Compound file: no stream " + name);
}

}  // namespace cfb
}  // namespace veraa
