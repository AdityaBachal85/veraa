#include "viewport.h"

#include <QDateTime>
#include <QDragEnterEvent>
#include <QMimeData>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include "dialogs.h"
#include "materialeditor.h"
#include "theme.h"

namespace veraa {

namespace {

QColor qcolor(const Vec3& c, float a = 1.0f) {
  return QColor::fromRgbF(saturate(c.x), saturate(c.y), saturate(c.z), a);
}

bool isLiftedLight(const QString& t) {
  return t == "RectLight" || t == "SphereLight" || t == "OmniLight" || t == "SpotLight";
}

bool needsHeightStage(const QString& t) {
  return t == "Box" || t == "ChamferBox" || t == "Cylinder" || t == "Cone" || t == "Tube" || t == "Pyramid" ||
         t == "Torus";
}

constexpr float kIconPixels = 22.0f;

}  // namespace

Viewport::Viewport(SceneDocument* doc, EditorState* state, ViewType type, QWidget* parent)
    : QOpenGLWidget(parent), doc_(doc), state_(state) {
  cam_.type = type;
  if (type == ViewType::Perspective) {
    shading_ = ShadingMode::Shaded;
  }
  cam_.center = Vec3(0, 0, isOrthoAxisView(type) && type != ViewType::Top ? 60.0f : 0.0f);
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);
  setAcceptDrops(true);
  setMinimumSize(120, 90);
  auto repaint = [this] { update(); };
  connect(doc_, &SceneDocument::nodesChanged, this, repaint);
  connect(doc_, &SceneDocument::structureChanged, this, repaint);
  connect(doc_, &SceneDocument::selectionChanged, this, repaint);
  connect(doc_, &SceneDocument::materialsChanged, this, repaint);
  connect(doc_, &SceneDocument::renderSettingsChanged, this, repaint);
  connect(state_, &EditorState::toolChanged, this, repaint);
  connect(state_, &EditorState::redrawRequested, this, repaint);
  connect(state_, &EditorState::viewportToolChanged, this, repaint);
  connect(state_, &EditorState::showGridChanged, this, repaint);
  connect(state_, &EditorState::createTypeChanged, this, [this](const QString&) {
    if (createStage_ > 0) cancelInteraction();
    update();
  });
}

Viewport::~Viewport() {
  makeCurrent();
  gl_.reset();
  doneCurrent();
}

void Viewport::setViewType(ViewType t) {
  pushViewHistory();
  ViewType old = cam_.type;
  cam_.type = t;
  if (t == ViewType::Perspective && old != ViewType::Perspective) shading_ = ShadingMode::Shaded;
  if (isOrthoAxisView(t) && !isOrthoAxisView(old) && old != ViewType::User) {
    // Coming from a perspective view: frame the same region.
    cam_.center = cam_.target;
    cam_.orthoHeight = std::max(cam_.distance * 0.75f, 10.0f);
  }
  updateCamera();
  update();
  Q_EMIT viewChanged();
}

void Viewport::setCameraView(uint32_t cameraNodeId) {
  pushViewHistory();
  cam_.type = ViewType::Camera;
  cam_.cameraNodeId = cameraNodeId;
  shading_ = ShadingMode::Shaded;
  update();
  Q_EMIT viewChanged();
}

void Viewport::setShading(ShadingMode m) {
  shading_ = m;
  update();
  Q_EMIT viewChanged();
}

void Viewport::setEdgedFaces(bool on) {
  edgedFaces_ = on;
  update();
  Q_EMIT viewChanged();
}

void Viewport::setActive(bool a) {
  if (active_ == a) return;
  active_ = a;
  update();
}

const Node* Viewport::cameraNode() const {
  if (cam_.type != ViewType::Camera) return nullptr;
  Node* n = doc_->scene().findNode(cam_.cameraNodeId);
  return n && n->camera() ? n : nullptr;
}

void Viewport::updateCamera() {
  if (cam_.type == ViewType::Camera && !cameraNode()) cam_.type = ViewType::Perspective;
  cam_.update(width(), height(), cameraNode());
}

QString Viewport::label() const {
  QString view = viewTypeName(cam_.type);
  if (const Node* c = cameraNode()) view = QString::fromStdString(c->name);
  QString shade = shading_ == ShadingMode::Wireframe ? tr("Wireframe") : tr("Default Shading");
  if (edgedFaces_ && shading_ == ShadingMode::Shaded) shade += tr(" + Edged Faces");
  return QStringLiteral("[+] [%1] [%2]").arg(view, shade);
}

QRect Viewport::labelRect() const {
  QFontMetrics fm(font());
  return QRect(4, 2, fm.horizontalAdvance(label()) + 12, fm.height() + 6);
}

void Viewport::pushViewHistory() {
  history_.push_back(cam_);
  if (history_.size() > 30) history_.erase(history_.begin());
}

void Viewport::undoViewChange() {
  if (history_.empty()) return;
  cam_ = history_.back();
  history_.pop_back();
  update();
  Q_EMIT viewChanged();
}

void Viewport::zoomExtents(bool selectedOnly) {
  pushViewHistory();
  updateCamera();
  Aabb b;
  if (selectedOnly && !doc_->selection().isEmpty()) b = doc_->selectionBounds();
  if (!b.valid()) b = doc_->scene().bounds(true);
  if (!b.valid()) b = doc_->scene().bounds(false);
  cam_.frame(b);
  update();
}

uint32_t Viewport::createCameraFromView() {
  updateCamera();
  doc_->beginEdit(tr("Create Camera From View"));
  Node* n = doc_->createNode("Camera");
  if (!n) {
    doc_->cancelEdit();
    return 0;
  }
  n->transform.position = cam_.eye();
  n->transform.rotation = rotationLookingAlong(cam_.forward());
  float aspect = cam_.aspect();
  float hfov = degrees(2.0f * std::atan(std::tan(radians(cam_.verticalFov()) * 0.5f) * aspect));
  n->object->params.setNumber("fov", hfov);
  n->object->params.setNumber("focusDistance", cam_.type == ViewType::Perspective ? cam_.distance : 300.0f);
  doc_->notifyNodeChanged(n, ChangeAll);
  doc_->select(n->id, false);
  doc_->endEdit();
  if (cam_.type == ViewType::Perspective) setCameraView(n->id);
  return n->id;
}

// ---------------------------------------------------------------------------------------------
// Rendering

void Viewport::initializeGL() {
  initializeOpenGLFunctions();
  gl_ = std::make_unique<GlScene>(static_cast<QOpenGLFunctions_3_3_Core*>(this));
  if (!gl_->init(&glError_)) qWarning("Viewport shader error: %s", qPrintable(glError_));
}

void Viewport::resizeGL(int, int) { updateCamera(); }

void Viewport::paintGL() {
  updateCamera();
  const Mat4& vp = cam_.viewProj();
  QColor bg = theme::viewportBg();
  glClearColor(bg.redF(), bg.greenF(), bg.blueF(), 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glDisable(GL_CULL_FACE);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  if (gl_ && glError_.isEmpty()) {
    glDisable(GL_DEPTH_TEST);
    if (!cam_.orthographic()) gl_->drawBackground(theme::viewportBgTop(), theme::viewportBgBottom());

    // Grid as background: never occludes objects.
    if (state_->showGrid()) {
      LineBatch grid;
      drawGrid(grid);
      gl_->drawBatch(grid, vp);
    }

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    LineBatch brackets;
    const Scene& scene = doc_->scene();
    LineBatch shapes;
    for (const auto& up : scene.nodes) {
      const Node& n = *up;
      ObjectKind kind = n.kind();
      if ((kind != ObjectKind::Geometry && kind != ObjectKind::Shape) || !scene.isNodeVisible(n)) continue;
      bool sel = doc_->isSelected(n.id);
      bool frozen = scene.isNodeFrozen(n);
      QColor wire = frozen ? theme::frozen() : qcolor(n.wireColor);
      if (kind == ObjectKind::Shape || n.isShapeResult()) {
        // Splines draw as lines; a viewport-renderable spline also shows its tube below.
        Mat4 m = n.objectWorldMatrix();
        QColor c = sel ? theme::selection() : wire;
        for (const auto& line : n.shapeLines())
          for (size_t i = 1; i < line.size(); ++i) shapes.line(m.transformPoint(line[i - 1]), m.transformPoint(line[i]), c);
        if (n.triMesh().empty()) continue;
      }
      if (n.displayAsBox) {
        Mat4 m = n.objectWorldMatrix();
        const Aabb& lb = n.triMesh().bounds;
        if (lb.valid()) {
          QColor c = sel ? theme::selection() : wire;
          for (int e = 0; e < 12; ++e) {
            static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                             {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
            auto corner = [&](int i) {
              return m.transformPoint(Vec3((i & 1) ? lb.hi.x : lb.lo.x, (i & 2) ? lb.hi.y : lb.lo.y,
                                           (i & 4) ? lb.hi.z : lb.lo.z));
            };
            shapes.line(corner(edges[e][0]), corner(edges[e][1]), c);
          }
        }
        continue;
      }
      uint64_t stamp = doc_->geometryStamp(n.id);
      if (shading_ == ShadingMode::Shaded) {
        Vec3 c = n.wireColor;
        if (const Material* m = scene.findMaterial(n.materialId)) {
          c = m->params.getColor("diffuse");
          if (!m->params.getString("diffuseMap").empty()) c = Vec3(0.72f);
          Vec3 refr = m->params.getColor("refract");
          if (maxComponent(refr) > 0.3f) c = lerp(c, Vec3(0.75f, 0.85f, 0.9f), 0.6f);
          c = vmax(c, Vec3(0.04f));
        }
        if (frozen) c = Vec3(0.45f);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(1.0f, 1.0f);
        gl_->drawShaded(n, stamp, vp, cam_.eye(), cam_.forward(), cam_.orthographic(), qcolor(c));
        glDisable(GL_POLYGON_OFFSET_FILL);
        if (edgedFaces_) gl_->drawWire(n, stamp, vp, sel ? theme::selection() : wire.darker(220));
        if (sel) drawBrackets(brackets, n);
      } else {
        gl_->drawWire(n, stamp, vp, sel ? theme::selection() : wire);
      }
    }
    gl_->drawBatch(shapes, vp);
    LineBatch icons;
    drawIcons(icons);
    for (const auto& d : state_->decorators()) d->drawLines(*this, icons);
    if (auto tool = state_->viewportToolShared()) tool->drawLines(*this, icons);
    gl_->drawBatch(icons, vp);
    glDisable(GL_DEPTH_TEST);
    gl_->drawBatch(brackets, vp);

    if (gizmoAvailable()) {
      setupGizmo();
      LineBatch g;
      gizmo_.draw(g, cam_, drag_ == Drag::Gizmo ? gizmoDrag_.handle() : hot_);
      gl_->drawBatch(g, vp);
    }
    gl_->collectGarbage();
  }

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  glUseProgram(0);

  QPainter p(this);
  drawOverlay(p);
  for (const auto& d : state_->decorators()) d->drawOverlay(*this, p);
  if (auto tool = state_->viewportToolShared()) tool->drawOverlay(*this, p);
}

void Viewport::drawGrid(LineBatch& b) {
  QColor minor = theme::gridMinor(), major = theme::gridMajor(), axis = theme::gridAxis();
  if (isOrthoAxisView(cam_.type)) {
    int ua, va;  // world axes spanning the view plane
    switch (cam_.type) {
      case ViewType::Top:
      case ViewType::Bottom: ua = 0; va = 1; break;
      case ViewType::Front:
      case ViewType::Back: ua = 0; va = 2; break;
      default: ua = 1; va = 2; break;
    }
    float hh = cam_.orthoHalfHeight(), hw = hh * cam_.aspect();
    Aabb vis;
    for (int sx = -1; sx <= 1; sx += 2)
      for (int sy = -1; sy <= 1; sy += 2) vis.expand(cam_.center + cam_.right() * (hw * sx) + cam_.up() * (hh * sy));
    float wpp = cam_.worldPerPixel(cam_.center);
    float s = 0.001f;
    while (s / wpp < 9.0f) s *= 10.0f;
    float umin = std::floor(vis.lo[ua] / s) * s, umax = vis.hi[ua], vmin = std::floor(vis.lo[va] / s) * s,
          vmax = vis.hi[va];
    auto point = [&](float u, float v) {
      Vec3 p(0, 0, 0);
      p[ua] = u;
      p[va] = v;
      return p;
    };
    int count = 0;
    for (float u = umin; u <= umax && count < 2000; u += s, ++count) {
      long k = std::lround(u / s);
      QColor c = k == 0 ? axis : (k % 10 == 0 ? major : minor);
      b.line(point(u, vis.lo[va]), point(u, vmax), c);
    }
    count = 0;
    for (float v = vmin; v <= vmax && count < 2000; v += s, ++count) {
      long k = std::lround(v / s);
      QColor c = k == 0 ? axis : (k % 10 == 0 ? major : minor);
      b.line(point(vis.lo[ua], v), point(umax, v), c);
    }
    return;
  }
  // Home grid on the XY plane for perspective, user and camera views.
  float ref = cam_.type == ViewType::Perspective ? cam_.distance
              : cam_.type == ViewType::User      ? cam_.orthoHeight * 1.5f
                                                 : std::max(std::fabs(cam_.eye().z) * 2.0f, 100.0f);
  float s = std::pow(10.0f, std::floor(std::log10(std::max(ref, 1.0f) / 30.0f)));
  s = std::max(s, 0.01f);
  const int n = 100;
  float big = s * 10.0f;
  Vec3 c = cam_.type == ViewType::Camera ? cam_.eye() : (cam_.type == ViewType::User ? cam_.center : cam_.target);
  float cx = std::round(c.x / big) * big, cy = std::round(c.y / big) * big;
  for (int i = -n; i <= n; ++i) {
    float x = cx + i * s, y = cy + i * s;
    long kx = std::lround(x / s), ky = std::lround(y / s);
    QColor colx = kx == 0 ? axis : (kx % 10 == 0 ? major : minor);
    QColor coly = ky == 0 ? axis : (ky % 10 == 0 ? major : minor);
    b.line(Vec3(x, cy - n * s, 0), Vec3(x, cy + n * s, 0), colx);
    b.line(Vec3(cx - n * s, y, 0), Vec3(cx + n * s, y, 0), coly);
  }
}

void Viewport::drawBrackets(LineBatch& b, const Node& n) {
  Aabb lb = n.triMesh().bounds;
  for (const auto& line : n.shapeLines())
    for (const auto& p : line) lb.expand(p);
  if (!lb.valid()) return;
  Mat4 m = n.objectWorldMatrix();
  Vec3 e = lb.extent() * 0.22f;
  QColor c = theme::selection();
  for (int i = 0; i < 8; ++i) {
    Vec3 corner((i & 1) ? lb.hi.x : lb.lo.x, (i & 2) ? lb.hi.y : lb.lo.y, (i & 4) ? lb.hi.z : lb.lo.z);
    Vec3 dir((i & 1) ? -1.0f : 1.0f, (i & 2) ? -1.0f : 1.0f, (i & 4) ? -1.0f : 1.0f);
    Vec3 wc = m.transformPoint(corner);
    b.line(wc, m.transformPoint(corner + Vec3(dir.x * e.x, 0, 0)), c);
    b.line(wc, m.transformPoint(corner + Vec3(0, dir.y * e.y, 0)), c);
    b.line(wc, m.transformPoint(corner + Vec3(0, 0, dir.z * e.z)), c);
  }
}

void Viewport::drawIcons(LineBatch& b) {
  const Scene& scene = doc_->scene();
  auto circle = [&](const Vec3& c, const Vec3& u, const Vec3& v, float r, const QColor& col, int segs = 32) {
    Vec3 prev = c + u * r;
    for (int i = 1; i <= segs; ++i) {
      float a = kTwoPi * i / segs;
      Vec3 p = c + (u * std::cos(a) + v * std::sin(a)) * r;
      b.line(prev, p, col);
      prev = p;
    }
  };
  for (const auto& up : scene.nodes) {
    const Node& n = *up;
    if (!scene.isNodeVisible(n) || n.kind() == ObjectKind::Geometry || n.kind() == ObjectKind::Shape) continue;
    if (cam_.type == ViewType::Camera && n.id == cam_.cameraNodeId) continue;
    bool sel = doc_->isSelected(n.id);
    QColor col = sel ? theme::selection() : qcolor(n.wireColor);
    Mat4 m = n.worldMatrix();
    Vec3 o = m.translation();
    Vec3 ax = normalize(m.column(0)), ay = normalize(m.column(1)), az = normalize(m.column(2));
    float sz = cam_.worldPerPixel(o) * kIconPixels;
    std::string type = n.object->type();
    const ParamBlock& p = n.object->params;
    if (type == "OmniLight") {
      circle(o, ax, ay, sz * 0.45f, col, 20);
      circle(o, ay, az, sz * 0.45f, col, 20);
      circle(o, az, ax, sz * 0.45f, col, 20);
      for (const Vec3& d : {ax, ay, az, -ax, -ay, -az}) b.line(o + d * (sz * 0.55f), o + d * (sz * 0.85f), col);
    } else if (type == "SpotLight") {
      float len = sz * 3.0f;
      float rf = len * std::tan(radians(p.getFloat("falloff")) * 0.5f);
      float rh = len * std::tan(radians(p.getFloat("hotspot")) * 0.5f);
      Vec3 c = o - az * len;
      circle(c, ax, ay, rf, col);
      QColor hc = col;
      hc.setAlphaF(0.6f);
      circle(c, ax, ay, rh, hc);
      for (const Vec3& d : {ax, ay, -ax, -ay}) b.line(o, c + d * rf, col);
      circle(o, ax, ay, sz * 0.25f, col, 12);
    } else if (type == "RectLight") {
      float w = p.getFloat("width") * 0.5f, l = p.getFloat("length") * 0.5f;
      Vec3 c00 = m.transformPoint({-w, -l, 0}), c10 = m.transformPoint({w, -l, 0}), c11 = m.transformPoint({w, l, 0}),
           c01 = m.transformPoint({-w, l, 0});
      b.line(c00, c10, col); b.line(c10, c11, col); b.line(c11, c01, col); b.line(c01, c00, col);
      b.line(c00, c11, col); b.line(c10, c01, col);
      float arrow = std::min(w, l) * 0.8f;
      Vec3 tip = m.transformPoint({0, 0, -arrow});
      b.line(o, tip, col);
      Vec3 side = ax * (std::min(w, l) * 0.12f);
      b.line(tip, tip + az * (arrow * 0.2f) + side, col);
      b.line(tip, tip + az * (arrow * 0.2f) - side, col);
    } else if (type == "SphereLight") {
      float r = p.getFloat("radius") * std::max(length(m.column(0)), 1e-6f);
      circle(o, ax, ay, r, col);
      circle(o, ay, az, r, col);
      circle(o, az, ax, r, col);
    } else if (type == "SunLight") {
      circle(o, cam_.right(), cam_.up(), sz * 0.5f, col, 20);
      for (int i = 0; i < 8; ++i) {
        float a = kTwoPi * i / 8;
        Vec3 d = cam_.right() * std::cos(a) + cam_.up() * std::sin(a);
        b.line(o + d * (sz * 0.65f), o + d * (sz * 0.95f), col);
      }
      Vec3 tip = o - az * (sz * 5.0f);
      b.line(o, tip, col);
      Vec3 side = normalize(cross(az, cam_.forward())) * (sz * 0.3f);
      b.line(tip, tip + az * (sz * 0.6f) + side, col);
      b.line(tip, tip + az * (sz * 0.6f) - side, col);
    } else if (n.camera()) {
      float aspect = static_cast<float>(scene.renderSettings.getInt("width")) /
                     std::max(1, scene.renderSettings.getInt("height"));
      float d = sz * 2.4f;
      float hh = d * std::tan(radians(n.camera()->verticalFovDeg(aspect)) * 0.5f), hw = hh * aspect;
      Vec3 c[4] = {o + (-ax * hw - ay * hh - az * d), o + (ax * hw - ay * hh - az * d),
                   o + (ax * hw + ay * hh - az * d), o + (-ax * hw + ay * hh - az * d)};
      for (int i = 0; i < 4; ++i) {
        b.line(o, c[i], col);
        b.line(c[i], c[(i + 1) % 4], col);
      }
      Vec3 t0 = o + (-ax * hw * 0.5f + ay * hh * 1.15f - az * d), t1 = o + (ax * hw * 0.5f + ay * hh * 1.15f - az * d),
           t2 = o + (ay * hh * 1.6f - az * d);
      b.line(t0, t1, col); b.line(t1, t2, col); b.line(t2, t0, col);
      // Camera body.
      Vec3 bw = ax * (sz * 0.35f), bh = ay * (sz * 0.3f), bd = az * (sz * 0.7f);
      Vec3 q[8];
      for (int i = 0; i < 8; ++i)
        q[i] = o + bw * ((i & 1) ? 1.f : -1.f) + bh * ((i & 2) ? 1.f : -1.f) + bd * ((i & 4) ? 1.f : 0.f);
      const int edges[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7}, {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
      for (const auto& e : edges) b.line(q[e[0]], q[e[1]], col);
    }
  }
}

void Viewport::drawOverlay(QPainter& p) {
  p.setRenderHint(QPainter::Antialiasing);
  QFont f = font();
  f.setPointSizeF(std::max(8.0, f.pointSizeF() * 0.95));
  p.setFont(f);

  // View label.
  p.setPen(active_ ? QColor(240, 240, 240) : QColor(200, 200, 200));
  p.drawText(labelRect().adjusted(4, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, label());

  if (!glError_.isEmpty()) {
    p.setPen(QColor(255, 120, 120));
    p.drawText(rect().adjusted(10, 30, -10, -10), Qt::TextWordWrap, tr("OpenGL error:\n") + glError_);
  }

  // Axis tripod (bottom left).
  QPointF o(34, height() - 32);
  struct AxisInfo {
    Vec3 d;
    QColor c;
    const char* name;
  };
  AxisInfo axes[3] = {{{1, 0, 0}, theme::axisX(), "x"}, {{0, 1, 0}, theme::axisY(), "y"}, {{0, 0, 1}, theme::axisZ(), "z"}};
  std::sort(std::begin(axes), std::end(axes),
            [&](const AxisInfo& a, const AxisInfo& b) { return dot(a.d, cam_.forward()) > dot(b.d, cam_.forward()); });
  for (const auto& a : axes) {
    QPointF dir(dot(a.d, cam_.right()), -dot(a.d, cam_.up()));
    QPointF tip = o + dir * 22.0;
    p.setPen(QPen(a.c, 2.0));
    p.drawLine(o, tip);
    if (std::hypot(dir.x(), dir.y()) > 0.15) {
      p.setPen(a.c.lighter(130));
      p.drawText(QRectF(tip.x() + dir.x() * 6 - 6, tip.y() + dir.y() * 6 - 8, 12, 16), Qt::AlignCenter, a.name);
    }
  }

  // Gizmo axis labels.
  if (gizmoAvailable() && gizmo_.tool == TransformTool::Move) {
    const char* names[3] = {"X", "Y", "Z"};
    QColor cols[3] = {theme::axisX(), theme::axisY(), theme::axisZ()};
    for (int a = 0; a < 3; ++a) {
      float tx, ty;
      if (!gizmo_.axisTip(cam_, a, tx, ty)) continue;
      p.setPen(cols[a].lighter(120));
      p.drawText(QRectF(tx - 8, ty - 8, 16, 16), Qt::AlignCenter, names[a]);
    }
  }

  // Region selection rectangle.
  if (drag_ == Drag::Region && moved_) {
    QRect r = QRect(pressPos_, lastPos_).normalized();
    p.setPen(QPen(QColor(255, 255, 255), 1, Qt::DashLine));
    p.setBrush(QColor(120, 170, 255, 30));
    p.drawRect(r);
  }

  // Safe frame for camera views.
  if (cam_.type == ViewType::Camera) {
    const Scene& s = doc_->scene();
    float ra = static_cast<float>(s.renderSettings.getInt("width")) / std::max(1, s.renderSettings.getInt("height"));
    QRectF fr;
    if (cam_.aspect() > ra) {
      float w = height() * ra;
      fr = QRectF((width() - w) * 0.5, 0, w, height());
    } else {
      float h = width() / ra;
      fr = QRectF(0, (height() - h) * 0.5, width(), h);
    }
    p.setPen(QPen(QColor(230, 200, 60, 160), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(fr.adjusted(1, 1, -1, -1));
  }

  // Active border.
  if (active_) {
    p.setPen(QPen(theme::activeBorder(), 2));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(1, 1, width() - 2, height() - 2));
  } else {
    p.setPen(QPen(QColor(30, 30, 30), 1));
    p.drawRect(QRectF(0.5, 0.5, width() - 1, height() - 1));
  }
}

// ---------------------------------------------------------------------------------------------
// Picking and gizmo

bool Viewport::gizmoAvailable() const {
  if (const ViewportTool* tool = state_->viewportTool(); tool && !tool->allowsGizmo()) return false;
  return state_->tool() != TransformTool::Select && !doc_->selection().isEmpty() && !state_->creating() &&
         createStage_ == 0;
}

void Viewport::setupGizmo() {
  updateCamera();
  Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  auto sel = doc_->selectedNodes();
  if (sel.size() == 1 && state_->tool() != TransformTool::Move) {
    // Rotate and scale work in the object's local axes.
    Mat4 m = sel[0]->worldMatrix();
    for (int i = 0; i < 3; ++i) axes[i] = normalize(m.column(i));
  }
  Vec3 origin = drag_ == Drag::Gizmo && state_->tool() != TransformTool::Move ? dragCenter_ : doc_->selectionCenter();
  gizmo_.setup(cam_, state_->tool(), origin, axes);
}

uint32_t Viewport::pick(float px, float py, float* depthOut) const {
  Ray ray = cam_.rayAt(px, py);
  uint32_t best = 0;
  float bestT = kInfinity;
  const Scene& scene = doc_->scene();
  for (const auto& up : scene.nodes) {
    const Node& n = *up;
    if (!scene.isNodeVisible(n) || scene.isNodeFrozen(n)) continue;
    if (!state_->selectionFilterAccepts(n)) continue;
    ObjectKind kind = n.kind();
    if (kind == ObjectKind::Geometry || kind == ObjectKind::Shape) {
      Mat4 m = n.objectWorldMatrix();
      if (!n.triMesh().empty()) {
        Mat4 inv = m.inverse();
        Ray local{inv.transformPoint(ray.o), inv.transformDir(ray.d)};
        float t = intersectTriMesh(n.triMesh(), local);
        if (t > 0 && t < bestT) {
          bestT = t;
          best = n.id;
        }
      }
      // Splines: pick within a few pixels of their lines.
      for (const auto& line : n.shapeLines())
        for (size_t i = 1; i < line.size(); ++i) {
          float ax, ay, bx, by, da, db;
          Vec3 wa = m.transformPoint(line[i - 1]), wb = m.transformPoint(line[i]);
          if (!cam_.project(wa, ax, ay, &da) || !cam_.project(wb, bx, by, &db)) continue;
          float vx = bx - ax, vy = by - ay, len2 = vx * vx + vy * vy;
          float u = len2 > 0 ? clampf(((px - ax) * vx + (py - ay) * vy) / len2, 0.0f, 1.0f) : 0.0f;
          float dist = std::hypot(ax + vx * u - px, ay + vy * u - py);
          if (dist > 5.0f) continue;
          Vec3 wp = lerp(wa, wb, u);
          float t = cam_.orthographic() ? dot(wp - ray.o, ray.d) : length(wp - ray.o);
          if (t < bestT) {
            bestT = t;
            best = n.id;
          }
        }
    } else {
      if (cam_.type == ViewType::Camera && n.id == cam_.cameraNodeId) continue;
      Vec3 wp = n.worldPosition();
      float sx, sy, depth;
      if (!cam_.project(wp, sx, sy, &depth)) continue;
      if (std::hypot(sx - px, sy - py) < 12.0f) {
        float t = cam_.orthographic() ? dot(wp - ray.o, ray.d) : depth;
        if (t < bestT) {
          bestT = t;
          best = n.id;
        }
      }
    }
  }
  if (depthOut) *depthOut = bestT;
  return best;
}

QVector<uint32_t> Viewport::regionPick(const QRect& r) const {
  QVector<uint32_t> out;
  QRectF rf(r);
  const Scene& scene = doc_->scene();
  for (const auto& up : scene.nodes) {
    const Node& n = *up;
    if (!scene.isNodeVisible(n) || scene.isNodeFrozen(n)) continue;
    if (!state_->selectionFilterAccepts(n)) continue;
    if (n.kind() != ObjectKind::Geometry && n.kind() != ObjectKind::Shape) {
      float sx, sy;
      if (cam_.project(n.worldPosition(), sx, sy) && rf.contains(sx, sy)) out.push_back(n.id);
      continue;
    }
    Mat4 m = n.objectWorldMatrix();
    bool shapeHit = false;
    for (const auto& line : n.shapeLines())
      for (const auto& p : line) {
        float sx, sy;
        if (!shapeHit && cam_.project(m.transformPoint(p), sx, sy) && rf.contains(sx, sy)) shapeHit = true;
      }
    if (shapeHit) {
      out.push_back(n.id);
      continue;
    }
    const TriMesh& t = n.triMesh();
    size_t count = t.wirePositions.size();
    size_t step = std::max<size_t>(1, count / 4000);
    bool hit = false;
    for (size_t i = 0; i < count && !hit; i += step) {
      float sx, sy;
      if (cam_.project(m.transformPoint(t.wirePositions[i]), sx, sy) && rf.contains(sx, sy)) hit = true;
    }
    // Crossing: also accept edges passing through the rectangle (sampled).
    for (size_t i = 0; i + 1 < t.wireIndices.size() && !hit; i += 2 * step) {
      float ax, ay, bx, by;
      if (!cam_.project(m.transformPoint(t.wirePositions[t.wireIndices[i]]), ax, ay)) continue;
      if (!cam_.project(m.transformPoint(t.wirePositions[t.wireIndices[i + 1]]), bx, by)) continue;
      for (int k = 1; k < 8 && !hit; ++k) {
        float f = k / 8.0f;
        if (rf.contains(ax + (bx - ax) * f, ay + (by - ay) * f)) hit = true;
      }
    }
    if (hit) out.push_back(n.id);
  }
  return out;
}

void Viewport::beginTransformDrag(GizmoHandle h, float px, float py, bool cloneFirst) {
  TransformTool tool = state_->tool();
  QString label = tool == TransformTool::Move ? tr("Move") : tool == TransformTool::Rotate ? tr("Rotate") : tr("Scale");
  doc_->beginEdit(cloneFirst ? tr("Clone") : label);
  cloneDrag_ = cloneFirst;
  cloneSources_.clear();
  if (cloneFirst) {
    for (Node* n : doc_->selectedNodes()) cloneSources_.push_back(n->id);
    doc_->cloneSelected(true);
  }
  setupGizmo();
  dragCenter_ = gizmo_.origin;
  dragStart_.clear();
  for (Node* n : doc_->selectedNodes()) dragStart_.push_back({n->id, n->transform, n->worldPosition()});
  gizmoDrag_.begin(cam_, gizmo_, h, px, py);
  if (tool != TransformTool::Rotate) state_->setConstraint(h);
  drag_ = Drag::Gizmo;
}

void Viewport::updateTransformDrag(float px, float py) {
  // Drags work in world space; results are written back into each node's local transform so
  // linked children move correctly.
  Scene& scene = doc_->scene();
  QVector<uint32_t> ids;
  TransformTool tool = state_->tool();
  if (tool == TransformTool::Move) {
    Vec3 d = gizmoDrag_.translation(cam_, px, py);
    Vec3 snapped;
    if (!dragStart_.empty() && state_->snapPoint(*this, px, py, dragStart_[0].worldPos + d, snapped))
      d = snapped - dragStart_[0].worldPos;
    dragDisplay_ = d;
    for (const auto& s : dragStart_)
      if (Node* n = scene.findNode(s.id)) {
        n->transform = s.xf;
        n->setWorldPosition(s.worldPos + d);
        ids.push_back(s.id);
      }
  } else if (tool == TransformTool::Rotate) {
    Vec3 axis;
    float angle = gizmoDrag_.rotation(cam_, px, py, axis);
    angle = state_->snapAngle(angle);
    dragDisplay_ = Vec3(degrees(angle), 0, 0);
    Mat4 r = Mat4::rotateAxis(axis, angle);
    bool multi = dragStart_.size() > 1;
    for (const auto& s : dragStart_)
      if (Node* n = scene.findNode(s.id)) {
        n->transform = s.xf;
        Vec3 localAxis = normalize(n->parentWorldMatrix().inverse().transformDir(axis));
        Mat4 rl = Mat4::rotateAxis(localAxis, angle);
        n->transform.rotation = eulerDegFromMatrix(rl * Mat4::fromEulerDeg(s.xf.rotation));
        if (multi) n->setWorldPosition(dragCenter_ + r.transformDir(s.worldPos - dragCenter_));
        ids.push_back(s.id);
      }
  } else if (tool == TransformTool::Scale) {
    Vec3 f = gizmoDrag_.scaling(cam_, px, py);
    f = state_->snapPercent(f);
    dragDisplay_ = f * 100.0f;
    bool multi = dragStart_.size() > 1;
    for (const auto& s : dragStart_)
      if (Node* n = scene.findNode(s.id)) {
        n->transform = s.xf;
        n->transform.scale = s.xf.scale * f;
        if (multi) n->setWorldPosition(dragCenter_ + (s.worldPos - dragCenter_) * f);
        ids.push_back(s.id);
      }
  }
  doc_->notifyNodesChanged(ids, ChangeTransform);
}

void Viewport::endTransformDrag() {
  drag_ = Drag::None;
  if (cloneDrag_) {
    // Like 3ds Max: after a Shift+drag clone, ask how many copies to make.
    cloneDrag_ = false;
    Scene& scene = doc_->scene();
    Node* first = dragStart_.empty() ? nullptr : scene.findNode(dragStart_[0].id);
    CloneOptionsDialog dlg(first ? QString::fromStdString(first->name) : QString(), true, this);
    if (dlg.exec() != QDialog::Accepted) {
      dragStart_.clear();
      doc_->cancelEdit();
      update();
      return;
    }
    if (first && dragStart_.size() == 1 && !dlg.name().isEmpty()) first->name = dlg.name().toStdString();
    QVector<uint32_t> created;
    for (size_t i = 0; i < dragStart_.size(); ++i) {
      created.push_back(dragStart_[i].id);
      if (i < static_cast<size_t>(cloneSources_.size())) applyCloneMode(scene, cloneSources_[i], dragStart_[i].id, dlg.mode());
    }
    for (int k = 2; k <= dlg.copies(); ++k)
      for (size_t i = 0; i < dragStart_.size(); ++i) {
        const auto& s = dragStart_[i];
        Node* src = scene.findNode(s.id);
        if (!src) continue;
        Vec3 delta = src->transform.position - s.xf.position;
        auto copy = src->clone();
        copy->transform.position = s.xf.position + delta * static_cast<float>(k);
        uint32_t cid = scene.addNode(std::move(copy))->id;
        created.push_back(cid);
        if (i < static_cast<size_t>(cloneSources_.size())) applyCloneMode(scene, cloneSources_[i], cid, dlg.mode());
      }
    doc_->notifyStructureChanged();
    doc_->setSelection(created);
  }
  dragStart_.clear();
  doc_->endEdit();
  update();
}

bool Viewport::cancelInteraction() {
  if (auto tool = state_->viewportToolShared(); tool && tool->cancel()) {
    update();
    return true;
  }
  if (createStage_ > 0) {
    createStage_ = 0;
    createNode_ = 0;
    drag_ = Drag::None;
    doc_->cancelEdit();
    update();
    return true;
  }
  if (drag_ == Drag::Gizmo) {
    drag_ = Drag::None;
    dragStart_.clear();
    doc_->cancelEdit();
    update();
    return true;
  }
  if (drag_ != Drag::None) {
    drag_ = Drag::None;
    update();
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------------------------
// Creation

bool Viewport::constructionHit(float px, float py, Vec3& hit) const {
  Vec3 p0, n;
  cam_.constructionPlane(p0, n);
  Ray r = cam_.rayAt(px, py);
  float denom = dot(r.d, n);
  if (std::fabs(denom) < 1e-5f) return false;
  float t = dot(p0 - r.o, n) / denom;
  if (!cam_.orthographic() && t < 0) return false;
  hit = r.o + r.d * t;
  Vec3 snapped;
  if (state_->snapPoint(*this, px, py, hit, snapped)) hit = snapped;
  return true;
}

void Viewport::createPress(float px, float py) {
  updateCamera();
  Vec3 hit;
  if (!constructionHit(px, py, hit)) return;
  QString type = state_->createType();
  doc_->beginEdit(tr("Create %1").arg(QString::fromStdString(objectDisplayName(type.toStdString()))));
  Node* n = doc_->createNode(type);
  if (!n) {
    doc_->cancelEdit();
    return;
  }
  createNode_ = n->id;
  createKind_ = type;
  createP0_ = hit;
  Vec3 p0, normal;
  cam_.constructionPlane(p0, normal);
  createPlaneN_ = normal;
  Vec3 rot = cam_.creationRotation();
  createRot_ = Mat4::fromEulerDeg(rot);
  n->transform.rotation = rot;
  n->transform.position = hit;
  bool ground = std::fabs(normal.z) > 0.5f;
  ParamBlock& p = n->object->params;

  if (isLiftedLight(type) && ground) n->transform.position.z += 250.0f;
  if (type == "Camera") {
    if (ground) n->transform.position.z += 150.0f;
    Vec3 f = cam_.forward();
    Vec3 dir = std::fabs(f.z) > 0.95f ? Vec3(0, 1, 0) : normalize(Vec3(f.x, f.y, 0));
    n->transform.rotation = rotationLookingAlong(dir);
  } else if (type == "SunLight") {
    n->transform.position.z += ground ? 500.0f : 0.0f;
    n->transform.rotation = rotationLookingAlong(normalize(Vec3(-0.5f, 0.6f, -0.65f)));
    n->transform.position = hit - normalize(Vec3(-0.5f, 0.6f, -0.65f)) * 500.0f;
  }
  // Start growing geometry from zero size.
  for (const char* key : {"length", "width", "height", "radius", "radius1", "depth"})
    if (p.has(key)) p.setNumber(key, 0.0);
  if (type == "Tube" || type == "Torus") p.setNumber("radius2", 0.0);
  if (type == "RectLight") { p.setNumber("width", 0.01); p.setNumber("length", 0.01); }
  if (type == "SphereLight") p.setNumber("radius", 0.01);

  doc_->select(n->id, false);
  doc_->notifyNodeChanged(n, ChangeAll);
  createStage_ = 1;
  drag_ = Drag::Create;
}

void Viewport::createMove(float px, float py, bool buttonDown) {
  Node* n = doc_->scene().findNode(createNode_);
  if (!n) return;
  ParamBlock& p = n->object->params;
  const QString& t = createKind_;
  if (createStage_ == 1 && buttonDown) {
    Vec3 hit;
    if (!constructionHit(px, py, hit)) return;
    Vec3 d = createRot_.transposed().transformDir(hit - createP0_);  // into the creation frame
    float r = std::sqrt(d.x * d.x + d.y * d.y);
    Vec3 lift = n->transform.position - createP0_;
    if (t == "Box" || t == "ChamferBox" || t == "Plane" || t == "Pyramid" || t == "RectLight") {
      if (t == "Pyramid") {
        p.setNumber("width", std::fabs(d.x));
        p.setNumber("depth", std::fabs(d.y));
      } else {
        p.setNumber("width", std::max(std::fabs(d.x), 0.01f));
        p.setNumber("length", std::max(std::fabs(d.y), 0.01f));
      }
      Vec3 mid = createP0_ + createRot_.transformDir(Vec3(d.x * 0.5f, d.y * 0.5f, 0));
      Vec3 keepLift = t == "RectLight" ? Vec3(0, 0, lift.z) : Vec3();
      n->transform.position = mid + keepLift;
    } else if (t == "Sphere" || t == "GeoSphere" || t == "Cylinder" || t == "SphereLight") {
      p.setNumber("radius", std::max(r, t == "SphereLight" ? 0.01f : 0.0f));
    } else if (t == "Cone" || t == "Tube" || t == "Torus") {
      p.setNumber("radius1", r);
      if (t == "Tube") p.setNumber("radius2", r * 0.8f);
      if (t == "Torus") p.setNumber("radius2", r * 0.25f);
    } else if (t == "Camera") {
      Vec3 dir = hit - n->transform.position;
      if (length(dir) > 1e-3f) n->transform.rotation = rotationLookingAlong(dir);
    } else if (t == "SunLight" || t == "SpotLight") {
      Vec3 dir = hit - n->transform.position;
      if (length(dir) > 1e-3f) n->transform.rotation = rotationLookingAlong(dir);
    }
    doc_->notifyNodeChanged(n, ChangeGeometry | ChangeTransform);
  } else if (createStage_ == 2) {
    float h = (createStartY_ - py) * cam_.worldPerPixel(n->transform.position);
    if (t == "Torus") {
      p.setNumber("radius2", std::max(0.1f, createBase_ + h * 0.5f));
    } else if (t == "ChamferBox") {
      p.setNumber("height", std::fabs(h));
    } else {
      p.setNumber("height", h);
    }
    doc_->notifyNodeChanged(n, ChangeGeometry);
  }
}

void Viewport::createRelease(float px, float py) {
  if (createStage_ != 1) return;
  Node* n = doc_->scene().findNode(createNode_);
  if (!n) {
    cancelInteraction();
    return;
  }
  float dist = std::hypot(px - pressPos_.x(), py - pressPos_.y());
  bool geometry = n->kind() == ObjectKind::Geometry;
  if (dist < 3.0f) {
    if (geometry) {
      cancelInteraction();  // a click without dragging creates nothing
      return;
    }
    // Lights and cameras get default sizes on a single click.
    ParamBlock& p = n->object->params;
    if (createKind_ == "RectLight") { p.setNumber("width", 100); p.setNumber("length", 100); }
    if (createKind_ == "SphereLight") p.setNumber("radius", 10);
    doc_->notifyNodeChanged(n, ChangeGeometry);
  }
  if (needsHeightStage(createKind_)) {
    createStage_ = 2;
    createStartY_ = py;
    createBase_ = createKind_ == "Torus" ? n->object->params.getFloat("radius2") : 0.0f;
    drag_ = Drag::None;
    state_->setPrompt(createKind_ == "Torus" ? tr("Move the mouse to set Radius 2, click to finish")
                                             : tr("Move the mouse to set the height, click to finish"));
    return;
  }
  finishCreate();
}

void Viewport::finishCreate() {
  createStage_ = 0;
  createNode_ = 0;
  drag_ = Drag::None;
  doc_->endEdit();
  state_->setPrompt(tr("Click and drag to create another %1, right-click to stop")
                        .arg(QString::fromStdString(objectDisplayName(createKind_.toStdString()))));
  update();
}

// ---------------------------------------------------------------------------------------------
// Input

void Viewport::showLabelMenu(const QPoint& pos) {
  QMenu menu(this);
  QMenu* views = menu.addMenu(tr("Views"));
  for (ViewType t : {ViewType::Perspective, ViewType::User, ViewType::Top, ViewType::Bottom, ViewType::Front,
                     ViewType::Back, ViewType::Left, ViewType::Right}) {
    QAction* a = views->addAction(viewTypeName(t), this, [this, t] { setViewType(t); });
    a->setCheckable(true);
    a->setChecked(cam_.type == t);
  }
  bool hasCam = false;
  for (const auto& n : doc_->scene().nodes)
    if (n->camera()) {
      if (!hasCam) views->addSeparator();
      hasCam = true;
      uint32_t id = n->id;
      QAction* a = views->addAction(QString::fromStdString(n->name), this, [this, id] { setCameraView(id); });
      a->setCheckable(true);
      a->setChecked(cam_.type == ViewType::Camera && cam_.cameraNodeId == id);
    }
  menu.addSeparator();
  QAction* shaded = menu.addAction(tr("Default Shading"), this, [this] { setShading(ShadingMode::Shaded); });
  shaded->setCheckable(true);
  shaded->setChecked(shading_ == ShadingMode::Shaded);
  QAction* wire = menu.addAction(tr("Wireframe"), this, [this] { setShading(ShadingMode::Wireframe); });
  wire->setCheckable(true);
  wire->setChecked(shading_ == ShadingMode::Wireframe);
  QAction* edged = menu.addAction(tr("Edged Faces"), this, [this] { setEdgedFaces(!edgedFaces_); });
  edged->setCheckable(true);
  edged->setChecked(edgedFaces_);
  menu.addSeparator();
  QAction* grid = menu.addAction(tr("Show Grid"), this, [this] { state_->setShowGrid(!state_->showGrid()); });
  grid->setCheckable(true);
  grid->setChecked(state_->showGrid());
  menu.addAction(tr("Maximize Viewport Toggle"), this, [this] { Q_EMIT maximizeToggleRequested(); });
  menu.addAction(tr("Zoom Extents"), this, [this] { zoomExtents(false); });
  menu.exec(pos);
}

void Viewport::mousePressEvent(QMouseEvent* e) {
  if (!active_) Q_EMIT activated(this);
  setFocus();
  updateCamera();
  float px = static_cast<float>(e->position().x()), py = static_cast<float>(e->position().y());
  pressPos_ = lastPos_ = e->position().toPoint();
  moved_ = false;
  pressMods_ = e->modifiers();

  if (drag_ == Drag::Gizmo || drag_ == Drag::Create) {
    if (e->button() == Qt::RightButton) cancelInteraction();
    return;
  }

  if (labelRect().contains(pressPos_) && e->button() != Qt::MiddleButton) {
    showLabelMenu(e->globalPosition().toPoint());
    return;
  }

  // An active viewport tool (sub-object editing, custom creation, ...) sees clicks first.
  if (e->button() != Qt::MiddleButton && drag_ == Drag::None)
    if (auto tool = state_->viewportToolShared(); tool && tool->mousePress(*this, e)) {
      update();
      return;
    }

  if (e->button() == Qt::MiddleButton) {
    pushViewHistory();
    if ((e->modifiers() & Qt::AltModifier) && (e->modifiers() & Qt::ControlModifier)) drag_ = Drag::ZoomDrag;
    else if (e->modifiers() & Qt::AltModifier) drag_ = Drag::Orbit;
    else drag_ = Drag::Pan;
    return;
  }

  // Creation mode.
  if (state_->creating()) {
    if (e->button() == Qt::RightButton) {
      if (createStage_ > 0) cancelInteraction();
      else state_->setCreateType({});
      return;
    }
    if (e->button() == Qt::LeftButton) {
      if (createStage_ == 2) {
        finishCreate();
        return;
      }
      if (createStage_ == 0) {
        if (CreationToolFactory factory = creationToolFor(state_->createType())) {
          auto tool = factory();
          state_->setViewportTool(tool);
          if (tool) tool->mousePress(*this, e);
          update();
          return;
        }
        createPress(px, py);
      }
    }
    return;
  }

  if (e->button() == Qt::RightButton) {
    drag_ = Drag::None;
    return;
  }
  if (e->button() != Qt::LeftButton) return;

  bool ctrl = e->modifiers() & Qt::ControlModifier, alt = e->modifiers() & Qt::AltModifier,
       shift = e->modifiers() & Qt::ShiftModifier;
  TransformTool tool = state_->tool();
  if (gizmoAvailable() && !ctrl && !alt) {
    setupGizmo();
    GizmoHandle h = gizmo_.hitTest(cam_, px, py);
    if (h != GizmoHandle::None) {
      beginTransformDrag(h, px, py, shift && tool == TransformTool::Move);
      return;
    }
  }
  uint32_t hit = pick(px, py);
  if (hit && !ctrl && !alt) {
    if (!doc_->isSelected(hit)) doc_->select(hit, false);
    if (tool != TransformTool::Select) {
      setupGizmo();
      GizmoHandle h = state_->constraint();
      if (tool == TransformTool::Rotate)
        h = handleAxisMask(h) == 1 ? GizmoHandle::X : handleAxisMask(h) == 2 ? GizmoHandle::Y : GizmoHandle::Z;
      beginTransformDrag(h, px, py, shift && tool == TransformTool::Move);
      return;
    }
    drag_ = Drag::None;
    return;
  }
  drag_ = Drag::Region;
}

void Viewport::mouseMoveEvent(QMouseEvent* e) {
  float px = static_cast<float>(e->position().x()), py = static_cast<float>(e->position().y());
  QPoint pos = e->position().toPoint();
  QPoint delta = pos - lastPos_;
  if ((pos - pressPos_).manhattanLength() > 3) moved_ = true;
  updateCamera();

  if (drag_ == Drag::None)
    if (auto tool = state_->viewportToolShared(); tool && tool->mouseMove(*this, e)) {
      lastPos_ = pos;
      update();
      return;
    }

  switch (drag_) {
    case Drag::Pan:
      cam_.pan(static_cast<float>(delta.x()), static_cast<float>(delta.y()));
      update();
      break;
    case Drag::Orbit:
      cam_.orbit(static_cast<float>(delta.x()), static_cast<float>(delta.y()));
      update();
      Q_EMIT viewChanged();
      break;
    case Drag::ZoomDrag:
      cam_.zoom(std::pow(1.01f, static_cast<float>(delta.y())), static_cast<float>(width()) * 0.5f,
                static_cast<float>(height()) * 0.5f);
      update();
      break;
    case Drag::Region: update(); break;
    case Drag::Gizmo: updateTransformDrag(px, py); break;
    case Drag::Create: createMove(px, py, true); break;
    case Drag::None:
      if (createStage_ == 2) {
        createMove(px, py, false);
      } else if (gizmoAvailable() && e->buttons() == Qt::NoButton) {
        setupGizmo();
        GizmoHandle h = gizmo_.hitTest(cam_, px, py);
        if (h != hot_) {
          hot_ = h;
          update();
        }
      }
      if (state_->creating()) setCursor(Qt::CrossCursor);
      else if (hot_ != GizmoHandle::None && gizmoAvailable()) setCursor(Qt::SizeAllCursor);
      else unsetCursor();
      break;
  }
  lastPos_ = pos;
}

void Viewport::mouseReleaseEvent(QMouseEvent* e) {
  float px = static_cast<float>(e->position().x()), py = static_cast<float>(e->position().y());
  if (drag_ == Drag::None && e->button() != Qt::MiddleButton)
    if (auto tool = state_->viewportToolShared(); tool && tool->mouseRelease(*this, e)) {
      update();
      return;
    }
  switch (drag_) {
    case Drag::Gizmo:
      if (e->button() == Qt::LeftButton) endTransformDrag();
      return;
    case Drag::Create:
      if (e->button() == Qt::LeftButton) createRelease(px, py);
      return;
    case Drag::Region: {
      drag_ = Drag::None;
      bool ctrl = pressMods_ & Qt::ControlModifier, alt = pressMods_ & Qt::AltModifier;
      if (moved_) {
        QVector<uint32_t> ids = regionPick(QRect(pressPos_, e->position().toPoint()).normalized());
        QVector<uint32_t> sel = doc_->selection();
        if (alt) {
          for (uint32_t id : ids) sel.removeAll(id);
        } else if (ctrl) {
          for (uint32_t id : ids)
            if (!sel.contains(id)) sel.push_back(id);
        } else {
          sel = ids;
        }
        doc_->setSelection(sel);
      } else {
        uint32_t hit = pick(px, py);
        if (hit && ctrl) {
          if (doc_->isSelected(hit)) doc_->deselect(hit);
          else doc_->select(hit, true);
        } else if (hit && alt) {
          doc_->deselect(hit);
        } else if (!hit && !ctrl && !alt) {
          doc_->clearSelection();
        }
      }
      update();
      return;
    }
    case Drag::Pan:
    case Drag::Orbit:
    case Drag::ZoomDrag:
      if (e->button() == Qt::MiddleButton) drag_ = Drag::None;
      return;
    case Drag::None:
      if (e->button() == Qt::RightButton && !state_->creating() && createStage_ == 0 && !moved_)
        Q_EMIT contextMenuRequested(this, e->globalPosition().toPoint());
      return;
  }
}

void Viewport::mouseDoubleClickEvent(QMouseEvent* e) {
  if (e->button() != Qt::MiddleButton)
    if (auto tool = state_->viewportToolShared(); tool && tool->mouseDoubleClick(*this, e)) {
      update();
      return;
    }
  if (e->button() == Qt::MiddleButton) {
    zoomExtents(false);
    return;
  }
  QOpenGLWidget::mouseDoubleClickEvent(e);
}

void Viewport::keyPressEvent(QKeyEvent* e) {
  if (auto tool = state_->viewportToolShared(); tool && tool->keyPress(*this, e)) {
    update();
    return;
  }
  QOpenGLWidget::keyPressEvent(e);
}

void Viewport::wheelEvent(QWheelEvent* e) {
  static qint64 lastPush = 0;
  qint64 now = QDateTime::currentMSecsSinceEpoch();
  if (now - lastPush > 800) pushViewHistory();
  lastPush = now;
  updateCamera();
  float steps = static_cast<float>(e->angleDelta().y()) / 120.0f;
  cam_.zoom(std::pow(0.87f, steps), static_cast<float>(e->position().x()), static_cast<float>(e->position().y()));
  update();
  Q_EMIT viewChanged();
}

void Viewport::leaveEvent(QEvent*) {
  if (hot_ != GizmoHandle::None && drag_ == Drag::None) {
    hot_ = GizmoHandle::None;
    update();
  }
}

void Viewport::dragEnterEvent(QDragEnterEvent* e) {
  if (e->mimeData()->hasFormat(kMaterialMime)) e->acceptProposedAction();
}

void Viewport::dragMoveEvent(QDragMoveEvent* e) {
  if (!e->mimeData()->hasFormat(kMaterialMime)) return;
  updateCamera();
  uint32_t hit = pick(static_cast<float>(e->position().x()), static_cast<float>(e->position().y()));
  Node* n = doc_->scene().findNode(hit);
  if (n && n->kind() == ObjectKind::Geometry) e->acceptProposedAction();
  else e->ignore();
}

void Viewport::dropEvent(QDropEvent* e) {
  if (!e->mimeData()->hasFormat(kMaterialMime)) return;
  uint32_t matId = e->mimeData()->data(kMaterialMime).toUInt();
  updateCamera();
  uint32_t hit = pick(static_cast<float>(e->position().x()), static_cast<float>(e->position().y()));
  Node* n = doc_->scene().findNode(hit);
  if (!n || n->kind() != ObjectKind::Geometry || !doc_->scene().findMaterial(matId)) return;
  doc_->beginEdit(tr("Assign Material"));
  n->materialId = matId;
  doc_->notifyNodeChanged(n, ChangeMaterial);
  doc_->endEdit();
  e->acceptProposedAction();
}

void Viewport::simulateMouse(QEvent::Type type, QPointF pos, Qt::MouseButton button, Qt::MouseButtons buttons,
                             Qt::KeyboardModifiers mods) {
  QPointF global = mapToGlobal(pos.toPoint());
  QMouseEvent ev(type, pos, global, button, buttons, mods);
  switch (type) {
    case QEvent::MouseButtonPress: mousePressEvent(&ev); break;
    case QEvent::MouseMove: mouseMoveEvent(&ev); break;
    case QEvent::MouseButtonRelease: mouseReleaseEvent(&ev); break;
    default: break;
  }
}

}  // namespace veraa
