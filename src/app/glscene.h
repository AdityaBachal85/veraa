// OpenGL resources for one viewport: shaders, per-node mesh buffers and streamed line batches.
#pragma once

#include <QColor>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>

#include <memory>
#include <unordered_map>
#include <vector>

#include "vmath.h"

namespace veraa {

class Node;

struct LineVertex {
  float x, y, z;
  float r, g, b, a;
};

// Collects colored lines/triangles in world space, then draws them in one call.
class LineBatch {
 public:
  void line(const Vec3& a, const Vec3& b, const QColor& c);
  void tri(const Vec3& a, const Vec3& b, const Vec3& c, const QColor& col);
  void clear() { lines.clear(); tris.clear(); }
  bool empty() const { return lines.empty() && tris.empty(); }
  std::vector<LineVertex> lines, tris;
};

// Viewport shading style: flat facets, and up to four scene lights (none = default lighting).
struct ShadeStyle {
  bool facets = false;
  bool headlightOnly = false;  // Performance quality: one light at the eye
  int lightCount = 0;
  Vec3 lightPos[4];    // world position, or direction toward the light when directional
  Vec3 lightColor[4];  // already scaled by intensity
  bool lightDirectional[4] = {false, false, false, false};
};

class GlScene {
 public:
  explicit GlScene(QOpenGLFunctions_3_3_Core* gl);
  ~GlScene();
  bool init(QString* error);

  // Uploads (if stale) and draws a node's shaded triangles.
  void drawShaded(const Node& node, uint64_t stamp, const Mat4& viewProj, const Vec3& eye, const Vec3& viewDir,
                  bool ortho, const QColor& color, float alpha = 1.0f);
  // Style used by drawShaded until changed.
  void setShadeStyle(const ShadeStyle& s) { style_ = s; }
  // Draws a node's polygon edges.
  void drawWire(const Node& node, uint64_t stamp, const Mat4& viewProj, const QColor& color);
  void drawBatch(const LineBatch& batch, const Mat4& viewProj);
  // Full-screen vertical gradient (call before anything else, depth test off).
  void drawBackground(const QColor& top, const QColor& bottom);
  // Frees buffers of nodes not drawn since the last call.
  void collectGarbage();

  // Bytes uploaded to the GPU by every GlScene since the last resetStats() (benchmarks).
  static uint64_t uploadedBytes();
  static void resetStats();
  // Benchmarks: skip the mesh draw calls (uploads and state changes still happen).
  static void setSkipMeshDraws(bool skip);

 private:
  struct GpuMesh {
    GLuint triVao = 0, triVbo = 0, triEbo = 0;
    GLsizei triCount = 0;
    GLuint wireVao = 0, wireVbo = 0, wireEbo = 0;
    GLsizei wireCount = 0;
    uint64_t stamp = ~0ull;
    bool used = false;
  };
  GpuMesh& meshFor(const Node& node, uint64_t stamp);
  void release(GpuMesh& m);
  void uploadLines(const std::vector<LineVertex>& v);

  QOpenGLFunctions_3_3_Core* gl_;
  QOpenGLShaderProgram meshProg_, lineProg_;
  std::unordered_map<uint32_t, GpuMesh> meshes_;
  GLuint lineVao_ = 0, lineVbo_ = 0;
  size_t lineCapacity_ = 0;
  ShadeStyle style_;
};

}  // namespace veraa
