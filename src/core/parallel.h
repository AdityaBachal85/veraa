// Simple parallel loops for coarse work (importing objects, triangulating meshes).
#pragma once

#include <cstddef>
#include <functional>

namespace veraa {

// Number of threads parallelFor uses (hardware threads, at least 1).
unsigned parallelThreads();

// Calls fn(i) for every i in [0, count) on up to parallelThreads() threads (the calling thread
// included) and returns when all are done. Items are handed out one at a time, so uneven items
// balance well. fn must be safe to run concurrently for different i. The first exception thrown
// by fn is rethrown after all threads finish.
void parallelFor(size_t count, const std::function<void(size_t)>& fn);

// Splits [0, count) into contiguous ranges of at least `grain` items and calls fn(begin, end)
// for each, in parallel. Small counts run on the calling thread only.
void parallelRanges(size_t count, size_t grain, const std::function<void(size_t, size_t)>& fn);

}  // namespace veraa
