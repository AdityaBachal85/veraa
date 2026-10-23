#include "glscene.h"

#include <QMatrix4x4>
#include <QOpenGLContext>
#include <QOpenGLFunctions>

#include "scene.h"

namespace veraa {

void LineBatch::line(const Vec3& a, const Vec3& b, const QColor& c) {
  float r = c.redF(), g = c.greenF(), bl = c.blueF(), al = c.alphaF();
  lines.push_back({a.x, a.y, a.z, r, g, bl, al});
  lines.push_back({b.x, b.y, b.z, r, g, bl, al});
}

void LineBatch::tri(const Vec3& a, const Vec3& b, const Vec3& c, const QColor& col) {
  float r = col.redF(), g = col.greenF(), bl = col.blueF(), al = col.alphaF();
  tris.push_back({a.x, a.y, a.z, r, g, bl, al});
  tris.push_back({b.x, b.y, b.z, r, g, bl, al});
  tris.push_back({c.x, c.y, c.z, r, g, bl, al});
}

namespace {

const char* kMeshVs = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNrm;
uniform mat4 uMVP;
uniform mat4 uModel;
uniform mat3 uNormalMat;
out vec3 vN;
out vec3 vW;
void main() {
  vW = (uModel * vec4(aPos, 1.0)).xyz;
  vN = uNormalMat * aNrm;
  gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

const char* kMeshFs = R"(#version 330 core
in vec3 vN;
in vec3 vW;
uniform vec3 uColor;
uniform vec3 uEye;
uniform vec3 uViewDir;
uniform int uOrtho;
uniform float uAlpha;
uniform int uFacets;
uniform int uHeadOnly;
uniform int uLightCount;
uniform vec3 uLightPos[4];
uniform vec3 uLightColor[4];
uniform int uLightDir[4];
out vec4 frag;
void main() {
  vec3 n = uFacets == 1 ? normalize(cross(dFdx(vW), dFdy(vW))) : normalize(vN);
  vec3 v = uOrtho == 1 ? -uViewDir : normalize(uEye - vW);
  if (dot(n, v) < 0.0) n = -n;
  float head = max(dot(n, v), 0.0);
  vec3 c;
  if (uLightCount > 0) {
    // Scene lights: Lambert per light plus a little ambient so unlit sides stay readable.
    c = uColor * 0.1;
    for (int i = 0; i < 4; ++i) {
      if (i >= uLightCount) break;
      vec3 l = uLightDir[i] == 1 ? normalize(uLightPos[i]) : normalize(uLightPos[i] - vW);
      float ndl = max(dot(n, l), 0.0);
      vec3 h = normalize(l + v);
      c += uColor * uLightColor[i] * ndl + uLightColor[i] * pow(max(dot(n, h), 0.0), 48.0) * 0.08;
    }
  } else if (uHeadOnly == 1) {
    c = uColor * (0.18 + 0.82 * head);
  } else {
    vec3 key = normalize(vec3(0.4, -0.55, 0.75));
    float k = max(dot(n, key), 0.0);
    float sky = 0.5 + 0.5 * n.z;
    vec3 h = normalize(key + v);
    float spec = pow(max(dot(n, h), 0.0), 48.0) * 0.12;
    c = uColor * (0.14 + 0.5 * head + 0.32 * k + 0.08 * sky) + vec3(spec);
  }
  frag = vec4(pow(c, vec3(0.95)), uAlpha);
}
)";

const char* kLineVs = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
uniform mat4 uMVP;
out vec4 vColor;
void main() {
  vColor = aColor;
  gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

const char* kLineFs = R"(#version 330 core
in vec4 vColor;
out vec4 frag;
void main() { frag = vColor; }
)";

QMatrix4x4 toQt(const Mat4& m) { return QMatrix4x4(&m.m[0][0]); }

uint64_t g_uploaded = 0;
bool g_skipDraws = false;  // benchmarks: measure CPU work without software rasterization

}  // namespace

GlScene::GlScene(QOpenGLFunctions_3_3_Core* gl) : gl_(gl) {}

uint64_t GlScene::uploadedBytes() { return g_uploaded; }
void GlScene::resetStats() { g_uploaded = 0; }
void GlScene::setSkipMeshDraws(bool skip) { g_skipDraws = skip; }

GlScene::~GlScene() {
  for (auto& [id, m] : meshes_) release(m);
  if (lineVbo_) gl_->glDeleteBuffers(1, &lineVbo_);
  if (lineVao_) gl_->glDeleteVertexArrays(1, &lineVao_);
}

bool GlScene::init(QString* error) {
  if (!meshProg_.addShaderFromSourceCode(QOpenGLShader::Vertex, kMeshVs) ||
      !meshProg_.addShaderFromSourceCode(QOpenGLShader::Fragment, kMeshFs) || !meshProg_.link()) {
    if (error) *error = meshProg_.log();
    return false;
  }
  if (!lineProg_.addShaderFromSourceCode(QOpenGLShader::Vertex, kLineVs) ||
      !lineProg_.addShaderFromSourceCode(QOpenGLShader::Fragment, kLineFs) || !lineProg_.link()) {
    if (error) *error = lineProg_.log();
    return false;
  }
  gl_->glGenVertexArrays(1, &lineVao_);
  gl_->glGenBuffers(1, &lineVbo_);
  gl_->glBindVertexArray(lineVao_);
  gl_->glBindBuffer(GL_ARRAY_BUFFER, lineVbo_);
  gl_->glEnableVertexAttribArray(0);
  gl_->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(LineVertex), nullptr);
  gl_->glEnableVertexAttribArray(1);
  gl_->glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(LineVertex),
                             reinterpret_cast<void*>(3 * sizeof(float)));
  gl_->glBindVertexArray(0);
  return true;
}

void GlScene::release(GpuMesh& m) {
  GLuint bufs[4] = {m.triVbo, m.triEbo, m.wireVbo, m.wireEbo};
  gl_->glDeleteBuffers(4, bufs);
  GLuint vaos[2] = {m.triVao, m.wireVao};
  gl_->glDeleteVertexArrays(2, vaos);
  m = GpuMesh();
}

GlScene::GpuMesh& GlScene::meshFor(const Node& node, uint64_t stamp) {
  GpuMesh& m = meshes_[node.id];
  m.used = true;
  if (m.stamp == stamp && m.triVao) return m;
  if (m.triVao) release(m);
  m.used = true;
  const TriMesh& t = node.triMesh();

  std::vector<float> verts;
  verts.reserve(t.positions.size() * 6);
  for (size_t i = 0; i < t.positions.size(); ++i) {
    const Vec3& p = t.positions[i];
    const Vec3& n = t.normals[i];
    verts.insert(verts.end(), {p.x, p.y, p.z, n.x, n.y, n.z});
  }
  gl_->glGenVertexArrays(1, &m.triVao);
  gl_->glGenBuffers(1, &m.triVbo);
  gl_->glGenBuffers(1, &m.triEbo);
  gl_->glBindVertexArray(m.triVao);
  gl_->glBindBuffer(GL_ARRAY_BUFFER, m.triVbo);
  gl_->glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)), verts.data(),
                    GL_STATIC_DRAW);
  gl_->glEnableVertexAttribArray(0);
  gl_->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), nullptr);
  gl_->glEnableVertexAttribArray(1);
  gl_->glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float)));
  gl_->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.triEbo);
  gl_->glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(t.indices.size() * sizeof(uint32_t)),
                    t.indices.data(), GL_STATIC_DRAW);
  m.triCount = static_cast<GLsizei>(t.indices.size());

  gl_->glGenVertexArrays(1, &m.wireVao);
  gl_->glGenBuffers(1, &m.wireVbo);
  gl_->glGenBuffers(1, &m.wireEbo);
  gl_->glBindVertexArray(m.wireVao);
  gl_->glBindBuffer(GL_ARRAY_BUFFER, m.wireVbo);
  gl_->glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(t.wirePositions.size() * sizeof(Vec3)),
                    t.wirePositions.data(), GL_STATIC_DRAW);
  gl_->glEnableVertexAttribArray(0);
  gl_->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vec3), nullptr);
  gl_->glDisableVertexAttribArray(1);
  gl_->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.wireEbo);
  gl_->glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(t.wireIndices.size() * sizeof(uint32_t)),
                    t.wireIndices.data(), GL_STATIC_DRAW);
  m.wireCount = static_cast<GLsizei>(t.wireIndices.size());
  g_uploaded += verts.size() * sizeof(float) + t.indices.size() * sizeof(uint32_t) +
                t.wirePositions.size() * sizeof(Vec3) + t.wireIndices.size() * sizeof(uint32_t);
  gl_->glBindVertexArray(0);
  m.stamp = stamp;
  return m;
}

void GlScene::drawShaded(const Node& node, uint64_t stamp, const Mat4& viewProj, const Vec3& eye,
                         const Vec3& viewDir, bool ortho, const QColor& color, float alpha) {
  GpuMesh& m = meshFor(node, stamp);
  if (!m.triCount) return;
  Mat4 model = node.objectWorldMatrix();
  QMatrix4x4 qModel = toQt(model);
  meshProg_.bind();
  meshProg_.setUniformValue("uMVP", toQt(viewProj * model));
  meshProg_.setUniformValue("uModel", qModel);
  meshProg_.setUniformValue("uNormalMat", qModel.normalMatrix());
  meshProg_.setUniformValue("uColor", QVector3D(color.redF(), color.greenF(), color.blueF()));
  meshProg_.setUniformValue("uEye", QVector3D(eye.x, eye.y, eye.z));
  meshProg_.setUniformValue("uViewDir", QVector3D(viewDir.x, viewDir.y, viewDir.z));
  meshProg_.setUniformValue("uOrtho", ortho ? 1 : 0);
  meshProg_.setUniformValue("uAlpha", alpha);
  meshProg_.setUniformValue("uFacets", style_.facets ? 1 : 0);
  meshProg_.setUniformValue("uHeadOnly", style_.headlightOnly ? 1 : 0);
  meshProg_.setUniformValue("uLightCount", style_.lightCount);
  for (int i = 0; i < 4; ++i) {
    const Vec3& lp = style_.lightPos[i];
    const Vec3& lc = style_.lightColor[i];
    QByteArray idx = QByteArray::number(i);
    meshProg_.setUniformValue(("uLightPos[" + idx + "]").constData(), QVector3D(lp.x, lp.y, lp.z));
    meshProg_.setUniformValue(("uLightColor[" + idx + "]").constData(), QVector3D(lc.x, lc.y, lc.z));
    meshProg_.setUniformValue(("uLightDir[" + idx + "]").constData(), style_.lightDirectional[i] ? 1 : 0);
  }
  gl_->glBindVertexArray(m.triVao);
  if (!g_skipDraws) gl_->glDrawElements(GL_TRIANGLES, m.triCount, GL_UNSIGNED_INT, nullptr);
  gl_->glBindVertexArray(0);
  meshProg_.release();
}

void GlScene::drawWire(const Node& node, uint64_t stamp, const Mat4& viewProj, const QColor& color) {
  GpuMesh& m = meshFor(node, stamp);
  if (!m.wireCount) return;
  lineProg_.bind();
  lineProg_.setUniformValue("uMVP", toQt(viewProj * node.objectWorldMatrix()));
  gl_->glBindVertexArray(m.wireVao);
  QOpenGLContext::currentContext()->functions()->glVertexAttrib4f(1, color.redF(), color.greenF(), color.blueF(),
                                                                  color.alphaF());
  if (!g_skipDraws) gl_->glDrawElements(GL_LINES, m.wireCount, GL_UNSIGNED_INT, nullptr);
  gl_->glBindVertexArray(0);
  lineProg_.release();
}

void GlScene::uploadLines(const std::vector<LineVertex>& v) {
  gl_->glBindBuffer(GL_ARRAY_BUFFER, lineVbo_);
  size_t bytes = v.size() * sizeof(LineVertex);
  if (bytes > lineCapacity_) {
    lineCapacity_ = std::max(bytes, lineCapacity_ * 2);
    gl_->glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(lineCapacity_), nullptr, GL_STREAM_DRAW);
  }
  gl_->glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(bytes), v.data());
}

void GlScene::drawBatch(const LineBatch& batch, const Mat4& viewProj) {
  if (batch.empty()) return;
  lineProg_.bind();
  lineProg_.setUniformValue("uMVP", toQt(viewProj));
  gl_->glBindVertexArray(lineVao_);
  if (!batch.tris.empty()) {
    uploadLines(batch.tris);
    gl_->glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(batch.tris.size()));
  }
  if (!batch.lines.empty()) {
    uploadLines(batch.lines);
    gl_->glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(batch.lines.size()));
  }
  gl_->glBindVertexArray(0);
  lineProg_.release();
}

void GlScene::drawBackground(const QColor& top, const QColor& bottom) {
  LineBatch b;
  Vec3 p00(-1, -1, 0.999f), p10(1, -1, 0.999f), p11(1, 1, 0.999f), p01(-1, 1, 0.999f);
  auto push = [&](const Vec3& p, const QColor& c) {
    b.tris.push_back({p.x, p.y, p.z, static_cast<float>(c.redF()), static_cast<float>(c.greenF()),
                      static_cast<float>(c.blueF()), 1.0f});
  };
  push(p00, bottom); push(p10, bottom); push(p11, top);
  push(p00, bottom); push(p11, top); push(p01, top);
  drawBatch(b, Mat4::identity());
}

void GlScene::collectGarbage() {
  for (auto it = meshes_.begin(); it != meshes_.end();) {
    if (!it->second.used) {
      release(it->second);
      it = meshes_.erase(it);
    } else {
      it->second.used = false;
      ++it;
    }
  }
}

}  // namespace veraa
