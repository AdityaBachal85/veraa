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

// Buffers of one node, shared by the contexts of a share group (all viewports of a window).
struct GlScene::SharedMesh {
  GLuint triVbo = 0, triEbo = 0, wireVbo = 0, wireEbo = 0;
  GLsizei triCount = 0, wireCount = 0;
  uint64_t stamp = ~0ull;
  uint64_t generation = 0;
  uint64_t lastUsed = 0;
};

struct GlScene::SharedCache {
  std::unordered_map<uint32_t, SharedMesh> meshes;
  int users = 0;
  uint64_t frame = 0;
};

namespace {

std::unordered_map<QOpenGLContextGroup*, GlScene::SharedCache*>& sharedCaches() {
  static std::unordered_map<QOpenGLContextGroup*, GlScene::SharedCache*> caches;
  return caches;
}

uint64_t g_generation = 0;

QOpenGLContextGroup* currentGroup() {
  QOpenGLContext* c = QOpenGLContext::currentContext();
  return c ? c->shareGroup() : nullptr;
}

}  // namespace

GlScene::GlScene(QOpenGLFunctions_3_3_Core* gl) : gl_(gl) {
  SharedCache*& c = sharedCaches()[currentGroup()];
  if (!c) c = new SharedCache;
  shared_ = c;
  ++shared_->users;
}

uint64_t GlScene::uploadedBytes() { return g_uploaded; }
void GlScene::resetStats() { g_uploaded = 0; }
void GlScene::setSkipMeshDraws(bool skip) { g_skipDraws = skip; }

GlScene::~GlScene() {
  for (auto& [id, m] : meshes_) releaseLocal(m);
  if (lineVbo_) gl_->glDeleteBuffers(1, &lineVbo_);
  if (lineVao_) gl_->glDeleteVertexArrays(1, &lineVao_);
  if (--shared_->users == 0) {
    // Last viewport of the share group: free the shared buffers too.
    for (auto& [id, s] : shared_->meshes) {
      GLuint bufs[4] = {s.triVbo, s.triEbo, s.wireVbo, s.wireEbo};
      gl_->glDeleteBuffers(4, bufs);
    }
    auto& caches = sharedCaches();
    for (auto it = caches.begin(); it != caches.end(); ++it)
      if (it->second == shared_) {
        caches.erase(it);
        break;
      }
    delete shared_;
  }
  shared_ = nullptr;
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
  loc_.mvp = meshProg_.uniformLocation("uMVP");
  loc_.model = meshProg_.uniformLocation("uModel");
  loc_.normalMat = meshProg_.uniformLocation("uNormalMat");
  loc_.color = meshProg_.uniformLocation("uColor");
  loc_.eye = meshProg_.uniformLocation("uEye");
  loc_.viewDir = meshProg_.uniformLocation("uViewDir");
  loc_.ortho = meshProg_.uniformLocation("uOrtho");
  loc_.alpha = meshProg_.uniformLocation("uAlpha");
  loc_.facets = meshProg_.uniformLocation("uFacets");
  loc_.headOnly = meshProg_.uniformLocation("uHeadOnly");
  loc_.lightCount = meshProg_.uniformLocation("uLightCount");
  for (int i = 0; i < 4; ++i) {
    QByteArray idx = QByteArray::number(i);
    loc_.lightPos[i] = meshProg_.uniformLocation(("uLightPos[" + idx + "]").constData());
    loc_.lightColor[i] = meshProg_.uniformLocation(("uLightColor[" + idx + "]").constData());
    loc_.lightDir[i] = meshProg_.uniformLocation(("uLightDir[" + idx + "]").constData());
  }
  loc_.lineMvp = lineProg_.uniformLocation("uMVP");
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

void GlScene::releaseLocal(LocalMesh& m) {
  GLuint vaos[2] = {m.triVao, m.wireVao};
  gl_->glDeleteVertexArrays(2, vaos);
  m = LocalMesh();
}

void GlScene::useProgram(QOpenGLShaderProgram& p) {
  if (bound_ == &p) return;
  p.bind();
  bound_ = &p;
}

const GlScene::LocalMesh* GlScene::meshFor(const Node& node, uint64_t stamp, GLsizei* triCount, GLsizei* wireCount) {
  SharedMesh& s = shared_->meshes[node.id];
  s.lastUsed = shared_->frame;
  if (s.stamp != stamp || !s.triVbo) {
    const TriMesh& t = node.triMesh();
    std::vector<float> verts(t.positions.size() * 6);
    for (size_t i = 0; i < t.positions.size(); ++i) {
      const Vec3& p = t.positions[i];
      const Vec3& n = t.normals[i];
      float* v = &verts[i * 6];
      v[0] = p.x, v[1] = p.y, v[2] = p.z, v[3] = n.x, v[4] = n.y, v[5] = n.z;
    }
    if (!s.triVbo) {
      GLuint bufs[4];
      gl_->glGenBuffers(4, bufs);
      s.triVbo = bufs[0], s.triEbo = bufs[1], s.wireVbo = bufs[2], s.wireEbo = bufs[3];
    }
    // Buffer data is only uploaded through GL_ARRAY_BUFFER so no vertex array state changes.
    gl_->glBindVertexArray(0);
    auto upload = [&](GLuint buf, size_t bytes, const void* data) {
      gl_->glBindBuffer(GL_ARRAY_BUFFER, buf);
      gl_->glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes), bytes ? data : nullptr, GL_STATIC_DRAW);
      g_uploaded += bytes;
    };
    upload(s.triVbo, verts.size() * sizeof(float), verts.data());
    upload(s.triEbo, t.indices.size() * sizeof(uint32_t), t.indices.data());
    upload(s.wireVbo, t.wirePositions.size() * sizeof(Vec3), t.wirePositions.data());
    upload(s.wireEbo, t.wireIndices.size() * sizeof(uint32_t), t.wireIndices.data());
    gl_->glBindBuffer(GL_ARRAY_BUFFER, 0);
    s.triCount = static_cast<GLsizei>(t.indices.size());
    s.wireCount = static_cast<GLsizei>(t.wireIndices.size());
    s.stamp = stamp;
    s.generation = ++g_generation;
  }
  LocalMesh& m = meshes_[node.id];
  m.used = true;
  if (m.generation != s.generation) {
    if (!m.triVao) {
      GLuint vaos[2];
      gl_->glGenVertexArrays(2, vaos);
      m.triVao = vaos[0], m.wireVao = vaos[1];
    }
    gl_->glBindVertexArray(m.triVao);
    gl_->glBindBuffer(GL_ARRAY_BUFFER, s.triVbo);
    gl_->glEnableVertexAttribArray(0);
    gl_->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), nullptr);
    gl_->glEnableVertexAttribArray(1);
    gl_->glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float)));
    gl_->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.triEbo);
    gl_->glBindVertexArray(m.wireVao);
    gl_->glBindBuffer(GL_ARRAY_BUFFER, s.wireVbo);
    gl_->glEnableVertexAttribArray(0);
    gl_->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vec3), nullptr);
    gl_->glDisableVertexAttribArray(1);
    gl_->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.wireEbo);
    gl_->glBindVertexArray(0);
    gl_->glBindBuffer(GL_ARRAY_BUFFER, 0);
    m.generation = s.generation;
  }
  *triCount = s.triCount;
  *wireCount = s.wireCount;
  return &m;
}

void GlScene::applyFrameUniforms(const Vec3& eye, const Vec3& viewDir, bool ortho) {
  const ShadeStyle& st = style_;
  bool styleSame = frameSent_ && st.facets == sentStyle_.facets && st.headlightOnly == sentStyle_.headlightOnly &&
                   st.lightCount == sentStyle_.lightCount;
  for (int i = 0; styleSame && i < 4; ++i)
    styleSame = st.lightPos[i] == sentStyle_.lightPos[i] && st.lightColor[i] == sentStyle_.lightColor[i] &&
                st.lightDirectional[i] == sentStyle_.lightDirectional[i];
  if (!styleSame) {
    meshProg_.setUniformValue(loc_.facets, st.facets ? 1 : 0);
    meshProg_.setUniformValue(loc_.headOnly, st.headlightOnly ? 1 : 0);
    meshProg_.setUniformValue(loc_.lightCount, st.lightCount);
    for (int i = 0; i < 4; ++i) {
      const Vec3& lp = st.lightPos[i];
      const Vec3& lc = st.lightColor[i];
      meshProg_.setUniformValue(loc_.lightPos[i], QVector3D(lp.x, lp.y, lp.z));
      meshProg_.setUniformValue(loc_.lightColor[i], QVector3D(lc.x, lc.y, lc.z));
      meshProg_.setUniformValue(loc_.lightDir[i], st.lightDirectional[i] ? 1 : 0);
    }
    sentStyle_ = st;
  }
  if (!frameSent_ || !(eye == sentEye_) || !(viewDir == sentViewDir_) || ortho != sentOrtho_) {
    meshProg_.setUniformValue(loc_.eye, QVector3D(eye.x, eye.y, eye.z));
    meshProg_.setUniformValue(loc_.viewDir, QVector3D(viewDir.x, viewDir.y, viewDir.z));
    meshProg_.setUniformValue(loc_.ortho, ortho ? 1 : 0);
    sentEye_ = eye;
    sentViewDir_ = viewDir;
    sentOrtho_ = ortho;
  }
  frameSent_ = true;
}

void GlScene::drawShaded(const Node& node, uint64_t stamp, const Mat4& viewProj, const Vec3& eye,
                         const Vec3& viewDir, bool ortho, const QColor& color, float alpha) {
  GLsizei tris = 0, wires = 0;
  const LocalMesh* m = meshFor(node, stamp, &tris, &wires);
  if (!tris) return;
  Mat4 model = node.objectWorldMatrix();
  QMatrix4x4 qModel = toQt(model);
  useProgram(meshProg_);
  applyFrameUniforms(eye, viewDir, ortho);
  meshProg_.setUniformValue(loc_.mvp, toQt(viewProj * model));
  meshProg_.setUniformValue(loc_.model, qModel);
  meshProg_.setUniformValue(loc_.normalMat, qModel.normalMatrix());
  meshProg_.setUniformValue(loc_.color, QVector3D(color.redF(), color.greenF(), color.blueF()));
  meshProg_.setUniformValue(loc_.alpha, alpha);
  gl_->glBindVertexArray(m->triVao);
  if (!g_skipDraws) gl_->glDrawElements(GL_TRIANGLES, tris, GL_UNSIGNED_INT, nullptr);
  gl_->glBindVertexArray(0);
}

void GlScene::drawWire(const Node& node, uint64_t stamp, const Mat4& viewProj, const QColor& color) {
  GLsizei tris = 0, wires = 0;
  const LocalMesh* m = meshFor(node, stamp, &tris, &wires);
  if (!wires) return;
  useProgram(lineProg_);
  lineProg_.setUniformValue(loc_.lineMvp, toQt(viewProj * node.objectWorldMatrix()));
  gl_->glBindVertexArray(m->wireVao);
  gl_->glVertexAttrib4f(1, color.redF(), color.greenF(), color.blueF(), color.alphaF());
  if (!g_skipDraws) gl_->glDrawElements(GL_LINES, wires, GL_UNSIGNED_INT, nullptr);
  gl_->glBindVertexArray(0);
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
  useProgram(lineProg_);
  lineProg_.setUniformValue(loc_.lineMvp, toQt(viewProj));
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
      releaseLocal(it->second);
      it = meshes_.erase(it);
    } else {
      it->second.used = false;
      ++it;
    }
  }
  // Shared buffers no viewport has drawn for a while (deleted or long hidden nodes).
  uint64_t frame = ++shared_->frame;
  uint64_t keep = static_cast<uint64_t>(shared_->users) * 4 + 4;
  for (auto it = shared_->meshes.begin(); it != shared_->meshes.end();) {
    if (frame - it->second.lastUsed > keep) {
      GLuint bufs[4] = {it->second.triVbo, it->second.triEbo, it->second.wireVbo, it->second.wireEbo};
      gl_->glDeleteBuffers(4, bufs);
      it = shared_->meshes.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace veraa
