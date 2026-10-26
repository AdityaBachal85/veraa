// Progress and cancellation shared between a long-running job (an import on a worker thread)
// and the UI thread that shows it.
#pragma once

#include <algorithm>
#include <atomic>

namespace veraa {

struct ImportProgress {
  // Fraction done in [0, 1] (written by the worker, read by the UI).
  void set(double fraction) { permille_.store(static_cast<int>(std::clamp(fraction, 0.0, 1.0) * 1000.0)); }
  double fraction() const { return permille_.load() / 1000.0; }
  // Set by the UI; workers poll it and stop early with an error.
  void cancel() { cancelled_.store(true); }
  bool cancelled() const { return cancelled_.load(); }

 private:
  std::atomic<int> permille_{0};
  std::atomic<bool> cancelled_{false};
};

// Reports `fraction` of the range [from, to] to an optional progress object; returns true when
// the job was cancelled.
inline bool reportProgress(ImportProgress* p, double from, double to, double fraction) {
  if (!p) return false;
  p->set(from + (to - from) * fraction);
  return p->cancelled();
}

}  // namespace veraa
