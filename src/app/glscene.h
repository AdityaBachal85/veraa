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
  // Call at the start of each paint (other code may have changed the bound program).
  void beginFrame() { bound_ = nullptr; }
  // Frees buffers of nodes not drawn since the last call.
  void collectGarbage();

  // Bytes uploaded to the GPU by every GlScene since the last resetStats() (benchmarks).
  static uint64_t uploadedBytes();
  static void resetStats();
  // Benchmarks: skip the mesh draw calls (uploads and state changes still happen).
  static void setSkipMeshDraws(bool skip);

 private:
  // Vertex arrays of this context pointing at the shared buffers of one node.
  struct LocalMesh {
    GLuint triVao = 0, wireVao = 0;
    uint64_t generation = 0;  // SharedMesh::generation the arrays were set up for
    bool used = false;
  };
  struct SharedMesh;

 public:
  struct SharedCache;  // per share group; defined in glscene.cpp

 private:
  // Uploads (if stale) the node's buffers, shared by every viewport of the window (their OpenGL
  // contexts share objects), and returns this context's vertex arrays for them.
  const LocalMesh* meshFor(const Node& node, uint64_t stamp, GLsizei* triCount, GLsizei* wireCount);
  void releaseLocal(LocalMesh& m);
  void uploadLines(const std::vector<LineVertex>& v);
  void useProgram(QOpenGLShaderProgram& p);
  void applyFrameUniforms(const Vec3& eye, const Vec3& viewDir, bool ortho);

  QOpenGLFunctions_3_3_Core* gl_;
  QOpenGLShaderProgram meshProg_, lineProg_;
  SharedCache* shared_ = nullptr;
  std::unordered_map<uint32_t, LocalMesh> meshes_;
  GLuint lineVao_ = 0, lineVbo_ = 0;
  size_t lineCapacity_ = 0;
  ShadeStyle style_;
  // Uniform locations and the frame-wide values last sent (sent again only when they change).
  struct {
    int mvp = -1, model = -1, normalMat = -1, color = -1, eye = -1, viewDir = -1, ortho = -1, alpha = -1, facets = -1,
        headOnly = -1, lightCount = -1, lightPos[4] = {-1, -1, -1, -1}, lightColor[4] = {-1, -1, -1, -1},
        lightDir[4] = {-1, -1, -1, -1};
    int lineMvp = -1;
  } loc_;
  bool frameSent_ = false;
  Vec3 sentEye_, sentViewDir_;
  bool sentOrtho_ = false;
  ShadeStyle sentStyle_;
  QOpenGLShaderProgram* bound_ = nullptr;
};

}  // namespace veraa
