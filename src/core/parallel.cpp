#include "parallel.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace veraa {

unsigned parallelThreads() {
  unsigned n = std::thread::hardware_concurrency();
  return n == 0 ? 1u : n;
}

void parallelFor(size_t count, const std::function<void(size_t)>& fn) {
  if (count == 0) return;
  size_t threads = std::min<size_t>(parallelThreads(), count);
  if (threads <= 1) {
    for (size_t i = 0; i < count; ++i) fn(i);
    return;
  }
  std::atomic<size_t> next{0};
  std::exception_ptr error;
  std::mutex errorMutex;
  auto work = [&] {
    for (size_t i = next.fetch_add(1); i < count; i = next.fetch_add(1)) {
      try {
        fn(i);
      } catch (...) {
        std::lock_guard<std::mutex> lock(errorMutex);
        if (!error) error = std::current_exception();
      }
    }
  };
  std::vector<std::thread> pool;
  pool.reserve(threads - 1);
  for (size_t t = 1; t < threads; ++t) pool.emplace_back(work);
  work();
  for (auto& t : pool) t.join();
  if (error) std::rethrow_exception(error);
}

void parallelRanges(size_t count, size_t grain, const std::function<void(size_t, size_t)>& fn) {
  if (count == 0) return;
  grain = std::max<size_t>(grain, 1);
  size_t ranges = std::min<size_t>(count / grain, size_t(parallelThreads()) * 4);
  if (ranges <= 1) {
    fn(0, count);
    return;
  }
  parallelFor(ranges, [&](size_t r) { fn(count * r / ranges, count * (r + 1) / ranges); });
}

}  // namespace veraa
