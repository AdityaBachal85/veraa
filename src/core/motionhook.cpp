#include "motionhook.h"

#include <mutex>

namespace veraa {

namespace {
std::mutex& hookMutex() {
  static std::mutex m;
  return m;
}
NodeMatrixAtTime& hook() {
  static NodeMatrixAtTime h;
  return h;
}
}  // namespace

void setNodeMatrixAtTime(NodeMatrixAtTime provider) {
  std::lock_guard<std::mutex> lock(hookMutex());
  hook() = std::move(provider);
}

bool nodeMatrixAtTime(const Scene& scene, const Node& node, double frame, bool wantPivot, Mat4& out) {
  NodeMatrixAtTime h;
  {
    std::lock_guard<std::mutex> lock(hookMutex());
    h = hook();
  }
  return h ? h(scene, node, frame, wantPivot, out) : false;
}

}  // namespace veraa
