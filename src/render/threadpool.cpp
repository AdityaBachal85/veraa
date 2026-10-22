#include "threadpool.h"

namespace veraa {
namespace render {

ThreadPool::ThreadPool(int threads) {
  int extra = threads > 1 ? threads - 1 : 0;
  for (int i = 0; i < extra; ++i) workers_.emplace_back([this, i] { workerLoop(i); });
}

ThreadPool::~ThreadPool() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    quit_ = true;
  }
  wake_.notify_all();
  for (auto& t : workers_) t.join();
}

void ThreadPool::drain(int threadIndex) {
  const auto& fn = *job_;
  for (;;) {
    int task = next_.fetch_add(1);
    if (task >= count_) break;
    fn(task, threadIndex);
  }
}

void ThreadPool::run(int count, const std::function<void(int, int)>& fn) {
  if (count <= 0) return;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    job_ = &fn;
    count_ = count;
    next_.store(0);
    active_ = static_cast<int>(workers_.size());
    ++generation_;
  }
  wake_.notify_all();
  drain(static_cast<int>(workers_.size()));
  std::unique_lock<std::mutex> lock(mutex_);
  done_.wait(lock, [this] { return active_ == 0; });
  job_ = nullptr;
}

void ThreadPool::workerLoop(int index) {
  unsigned seen = 0;
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [&] { return quit_ || generation_ != seen; });
      if (quit_) return;
      seen = generation_;
    }
    drain(index);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (--active_ == 0) done_.notify_all();
    }
  }
}

}  // namespace render
}  // namespace veraa
