// Minimal blocking parallel-for over a persistent set of worker threads.
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace veraa {
namespace render {

class ThreadPool {
 public:
  // threads: total number of threads that run tasks, including the calling thread.
  explicit ThreadPool(int threads);
  ~ThreadPool();
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  int size() const { return static_cast<int>(workers_.size()) + 1; }
  // Calls fn(task, threadIndex) for every task in [0, count); returns when all are done.
  // threadIndex is in [0, size()). Not reentrant.
  void run(int count, const std::function<void(int, int)>& fn);

 private:
  void workerLoop(int index);
  void drain(int threadIndex);

  std::vector<std::thread> workers_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable done_;
  const std::function<void(int, int)>* job_ = nullptr;
  int count_ = 0;
  std::atomic<int> next_{0};
  unsigned generation_ = 0;
  int active_ = 0;
  bool quit_ = false;
};

}  // namespace render
}  // namespace veraa
