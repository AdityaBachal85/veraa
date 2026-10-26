// Minimal reader for Compound File Binary (OLE2 / "structured storage") containers, the
// outer layer of 3ds Max .max scene files. Only reading of whole streams is supported.
// Every offset and sector chain is bounds-checked; malformed files fail with an error
// instead of crashing or looping.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace veraa {
namespace cfb {

class CompoundFile {
 public:
  // Takes the whole file. Returns false (with a message) when it is not a readable CFB file.
  bool open(std::vector<uint8_t> data, std::string* error);
  bool openFile(const std::string& path, std::string* error);

  // Names of all streams (UTF-8), in directory order. Streams inside sub-storages are listed
  // as "storage/stream".
  std::vector<std::string> streamNames() const;
  bool hasStream(const std::string& name) const;
  // Reads a whole stream by name (as listed by streamNames()). False if missing or broken.
  bool readStream(const std::string& name, std::vector<uint8_t>& out, std::string* error = nullptr) const;

 private:
  struct Entry {
    std::string name;
    int type = 0;  // 1 storage, 2 stream, 5 root
    uint32_t left = 0xFFFFFFFFu, right = 0xFFFFFFFFu, child = 0xFFFFFFFFu;
    uint32_t start = 0;
    uint64_t size = 0;
    std::string path;  // full path for streams
  };
  bool readChain(const std::vector<uint32_t>& fat, uint32_t start, size_t sectorSize, bool mini, uint64_t size,
                 std::vector<uint8_t>& out) const;
  bool sectorRange(uint32_t sector, size_t& offset) const;

  std::vector<uint8_t> data_;
  size_t sectorSize_ = 512;
  size_t miniSectorSize_ = 64;
  uint32_t miniCutoff_ = 4096;
  std::vector<uint32_t> fat_;
  std::vector<uint32_t> miniFat_;
  std::vector<uint8_t> miniStream_;
  std::vector<Entry> entries_;
  std::vector<int> streams_;  // indices into entries_
};

}  // namespace cfb
}  // namespace veraa
