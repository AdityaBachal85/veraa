#include "viewport.h"

#include <QDateTime>
#include <QDragEnterEvent>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QWheelEvent>

#include "dialogs.h"
#include "materialeditor.h"
#include "interfacedata.h"
#include "theme.h"
#include "uistate.h"

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
// Above this many triangles an object draws as a box while navigating with adaptive degradation.
constexpr size_t kDegradeTriangles = 3000;

float wrapDegrees(float a) {
  while (a > 180.0f) a -= 360.0f;
  while (a < -180.0f) a += 360.0f;
  return a;
}

// Working pivot (Hierarchy > Working Pivot) stored with the scene by the interface module.
bool workingPivot(const Scene& s, Mat4& m) {
  auto it = s.extensions.find("interface");
  if (it == s.extensions.end() || !it->is_object() || !it->contains("workingPivot")) return false;
  const json& w = (*it)["workingPivot"];
  if (!w.is_object()) return false;
  Transform t;
  t.fromJson(w);
  m = t.matrix();
  return true;
}

}  // namespace

bool Viewport::nodeShown(const Node& n) const {
  const Scene& scene = doc_->scene();
  if (!scene.isNodeVisible(n)) return false;
  const UiState& ui = UiState::instance();
  if (ui.categoryHidden(static_cast<int>(n.kind()))) return false;
  if (ui.hideFrozen() && scene.isNodeFrozen(n)) return false;
  return true;
}

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
  connect(&UiState::instance(), &UiState::changed, this, repaint);

  viewAnim_ = new QTimer(this);
  viewAnim_->setInterval(15);
  connect(viewAnim_, &QTimer::timeout, this, [this] {
    float t = clampf(static_cast<float>(QDateTime::currentMSecsSinceEpoch() - animStart_) / 280.0f, 0.0f, 1.0f);
    float s = t * t * (3.0f - 2.0f * t);
    cam_.yawDeg = animFromYaw_ + (animToYaw_ - animFromYaw_) * s;
    cam_.pitchDeg = animFromPitch_ + (animToPitch_ - animFromPitch_) * s;
    update();
    if (t >= 1.0f) {
      viewAnim_->stop();
      Q_EMIT viewChanged();
    }
  });
}

Viewport::~Viewport() {
  makeCurrent();
  gl_.reset();
  doneCurrent();
}

void Viewport::setViewType(ViewType t) {
  pushViewHistory();
  viewAnim_->stop();
  ViewType old = cam_.type;
  cam_.type = t;
  if (t == ViewType::Perspective && old != ViewType::Perspective && shading_ == ShadingMode::Wireframe)
    shading_ = ShadingMode::Shaded;
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
  viewAnim_->stop();
  cam_.type = ViewType::Camera;
  cam_.cameraNodeId = cameraNodeId;
  if (shading_ == ShadingMode::Wireframe) shading_ = ShadingMode::Shaded;
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

void Viewport::setLighting(ViewportLighting l) {
  lighting_ = l;
  update();
  Q_EMIT viewChanged();
}

void Viewport::setQuality(ViewportQuality q) {
  quality_ = q;
  update();
  Q_EMIT viewChanged();
}

void Viewport::setSafeFrame(bool on) {
  safeFrame_ = on;
  update();
  Q_EMIT viewChanged();
}

void Viewport::setBackground(const ViewportBackground& b) {
  background_ = b;
  if (b.imagePath != backgroundImagePath_) {
    backgroundImagePath_ = b.imagePath;
    backgroundImage_ = b.imagePath.isEmpty() ? QImage() : QImage(b.imagePath);
  }
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

QString Viewport::viewLabel() const {
  if (const Node* c = cameraNode()) return QString::fromStdString(c->name);
  return cam_.type == ViewType::User ? tr("Orthographic") : viewTypeName(cam_.type);
}

QString Viewport::qualityLabel() const {
  switch (quality_) {
    case ViewportQuality::HighQuality: return tr("High Quality");
    case ViewportQuality::Performance: return tr("Performance");
    default: return tr("Standard");
  }
}

QString Viewport::shadingLabel() const {
  QString s;
  switch (shading_) {
    case ShadingMode::Wireframe: s = tr("Wireframe"); break;
    case ShadingMode::Shaded: s = tr("Default Shading"); break;
    case ShadingMode::Facets: s = tr("Facets"); break;
    case ShadingMode::Clay: s = tr("Clay"); break;
    case ShadingMode::BoundingBox: s = tr("Bounding Box"); break;
  }
  if (edgedFaces_ && shading_ != ShadingMode::Wireframe && shading_ != ShadingMode::BoundingBox) s += tr(" + Edged Faces");
  return s;
}

QString Viewport::label() const {
  return QStringLiteral("[+] [%1] [%2] [%3]").arg(viewLabel(), qualityLabel(), shadingLabel());
}

QRect Viewport::labelSegment(int i) const {
  QFontMetrics fm(font());
  const QString parts[4] = {QStringLiteral("[+]"), QStringLiteral("[%1]").arg(viewLabel()),
                            QStringLiteral("[%1]").arg(qualityLabel()), QStringLiteral("[%1]").arg(shadingLabel())};
  int x = 8, gap = fm.horizontalAdvance(' ');
  for (int k = 0; k < 4; ++k) {
    int w = fm.horizontalAdvance(parts[k]);
    if (k == i) return QRect(x - 2, 3, w + 4, fm.height() + 4);
    x += w + gap;
  }
  return QRect();
}

QRect Viewport::labelRect() const { return labelSegment(0).united(labelSegment(3)); }

int Viewport::labelSegmentAt(const QPoint& p) const {
  for (int i = 0; i < 4; ++i)
    if (labelSegment(i).contains(p)) return i;
  return -1;
}

void Viewport::pushViewHistory() {
  history_.push_back(cam_);
  if (history_.size() > 30) history_.erase(history_.begin());
  redo_.clear();
}

void Viewport::undoViewChange() {
  if (history_.empty()) return;
  viewAnim_->stop();
  redo_.push_back(cam_);
  cam_ = history_.back();
  history_.pop_back();
  update();
  Q_EMIT viewChanged();
}

void Viewport::redoViewChange() {
  if (redo_.empty()) return;
  viewAnim_->stop();
  history_.push_back(cam_);
  cam_ = redo_.back();
  redo_.pop_back();
  update();
  Q_EMIT viewChanged();
}

void Viewport::saveNamedView() { savedView_ = cam_; }

bool Viewport::restoreNamedView() {
  if (!savedView_) return false;
  pushViewHistory();
  viewAnim_->stop();
  cam_ = *savedView_;
  update();
  Q_EMIT viewChanged();
  return true;
}

void Viewport::startViewAnimation(float yaw, float pitch) {
  animFromYaw_ = cam_.yawDeg;
  animFromPitch_ = cam_.pitchDeg;
  animToYaw_ = cam_.yawDeg + wrapDegrees(yaw - cam_.yawDeg);
  animToPitch_ = pitch;
  animStart_ = QDateTime::currentMSecsSinceEpoch();
  viewAnim_->start();
}

void Viewport::lookFrom(const Vec3& dirIn, bool animate) {
  if (cam_.type == ViewType::Camera) return;
  Vec3 dir = normalize(dirIn);
  updateCamera();
  // Pure axis directions map to the axis views.
  ViewType axis = ViewType::User;
  if (dir.z > 0.999f) axis = ViewType::Top;
  else if (dir.z < -0.999f) axis = ViewType::Bottom;
  else if (dir.y < -0.999f) axis = ViewType::Front;
  else if (dir.y > 0.999f) axis = ViewType::Back;
  else if (dir.x < -0.999f) axis = ViewType::Left;
  else if (dir.x > 0.999f) axis = ViewType::Right;
  float pitch = degrees(std::asin(clampf(dir.z, -1.0f, 1.0f)));
  float yaw = std::fabs(dir.z) > 0.999f ? -90.0f : degrees(std::atan2(dir.y, dir.x));
  if (cam_.type == ViewType::Perspective) {
    pushViewHistory();
    pitch = clampf(pitch, -89.5f, 89.5f);
    if (animate) {
      startViewAnimation(yaw, pitch);
    } else {
      cam_.yawDeg = yaw;
      cam_.pitchDeg = pitch;
      update();
      Q_EMIT viewChanged();
    }
    return;
  }
  // Orthographic views: axis directions become axis views, others an Orthographic (User) view.
  if (axis != ViewType::User) {
    setViewType(axis);
    return;
  }
  pushViewHistory();
  if (cam_.type != ViewType::User) {
    cam_.type = ViewType::User;
    cam_.yawDeg = yaw;
    cam_.pitchDeg = clampf(pitch, -89.9f, 89.9f);
    update();
    Q_EMIT viewChanged();
    return;
  }
  if (animate) {
    startViewAnimation(yaw, clampf(pitch, -89.9f, 89.9f));
  } else {
    cam_.yawDeg = yaw;
    cam_.pitchDeg = clampf(pitch, -89.9f, 89.9f);
    update();
    Q_EMIT viewChanged();
  }
}

void Viewport::goHome() {
  if (cam_.type == ViewType::Camera) return;
  pushViewHistory();
  viewAnim_->stop();
  if (cam_.type != ViewType::Perspective) cam_.type = ViewType::User;
  cam_.yawDeg = -60.0f;
  cam_.pitchDeg = 28.0f;
  updateCamera();
  Aabb b = doc_->scene().bounds(true);
  if (!b.valid()) b = doc_->scene().bounds(false);
  cam_.frame(b);
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
  // Moving the viewport to another splitter (layout change) can recreate the context.
  if (hookedContext_ != context()) {
    hookedContext_ = context();
    connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, [this] {
      makeCurrent();
      gl_.reset();
      doneCurrent();
    });
  }
}

void Viewport::resizeGL(int, int) { updateCamera(); }

void Viewport::drawBox(LineBatch& b, const Node& n, const QColor& c) {
  Mat4 m = n.objectWorldMatrix();
  Aabb lb = n.triMesh().bounds;
  for (const auto& line : n.shapeLines())
    for (const auto& p : line) lb.expand(p);
  if (!lb.valid()) return;
  static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                   {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
  auto corner = [&](int i) {
    return m.transformPoint(
        Vec3((i & 1) ? lb.hi.x : lb.lo.x, (i & 2) ? lb.hi.y : lb.lo.y, (i & 4) ? lb.hi.z : lb.lo.z));
  };
  for (const auto& e : edges) b.line(corner(e[0]), corner(e[1]), c);
}

void Viewport::drawBackgroundImage(QPainter& p) {
  if (backgroundImage_.isNull()) return;
  QSizeF img = backgroundImage_.size();
  QSizeF fit = img.scaled(size(), Qt::KeepAspectRatio);
  QRectF target(QPointF((width() - fit.width()) * 0.5, (height() - fit.height()) * 0.5), fit);
  p.setRenderHint(QPainter::SmoothPixmapTransform);
  p.drawImage(target, backgroundImage_);
}

void Viewport::paintGL() {
  updateCamera();
  const Mat4& vp = cam_.viewProj();
  const UiState& ui = UiState::instance();
  const Scene& scene = doc_->scene();

  // Background.
  QColor solid = background_.solid;
  bool gradient = false;
  switch (background_.mode) {
    case ViewportBackground::Gradient:
      gradient = !cam_.orthographic();
      solid = theme::viewportBg();
      break;
    case ViewportBackground::Solid: break;
    case ViewportBackground::Image: solid = QColor(0x24, 0x24, 0x24); break;
    case ViewportBackground::Environment:
      solid = qcolor(scene.renderSettings.has("envColor") ? scene.renderSettings.getColor("envColor") : Vec3(0.5f));
      break;
  }
  glClearColor(solid.redF(), solid.greenF(), solid.blueF(), 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  if (background_.mode == ViewportBackground::Image && !backgroundImage_.isNull()) {
    {
      QPainter bp(this);
      drawBackgroundImage(bp);
    }
    const qreal dpr = devicePixelRatioF();
    glViewport(0, 0, static_cast<GLsizei>(width() * dpr), static_cast<GLsizei>(height() * dpr));
    glClear(GL_DEPTH_BUFFER_BIT);
  }
  glDisable(GL_CULL_FACE);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  if (gl_ && glError_.isEmpty()) {
    glDisable(GL_DEPTH_TEST);
    if (gradient) gl_->drawBackground(background_.top, background_.bottom);

    // Grid as background: never occludes objects.
    if (state_->showGrid()) {
      LineBatch grid;
      drawGrid(grid);
      gl_->drawBatch(grid, vp);
    }

    // Lighting for shaded styles.
    ShadeStyle style;
    style.facets = shading_ == ShadingMode::Facets;
    style.headlightOnly = quality_ == ViewportQuality::Performance;
    if (lighting_ == ViewportLighting::Scene || quality_ == ViewportQuality::HighQuality) {
      for (const auto& up : scene.nodes) {
        const Node& n = *up;
        if (style.lightCount >= 4) break;
        if (!n.light() || !scene.isNodeVisible(n)) continue;
        const ParamBlock& p = n.object->params;
        if (p.has("enabled") && !p.getBool("enabled")) continue;
        Vec3 color = p.has("color") ? p.getColor("color") : Vec3(1.0f);
        float mult = p.has("intensity") ? p.getFloat("intensity") : 1.0f;
        float gain = clampf(0.55f + 0.25f * std::log2(1.0f + mult), 0.3f, 1.4f);
        int i = style.lightCount++;
        Mat4 m = n.worldMatrix();
        style.lightDirectional[i] = n.object->type() == "SunLight";
        style.lightPos[i] = style.lightDirectional[i] ? normalize(m.column(2)) : m.translation();
        style.lightColor[i] = color * gain;
      }
    }
    gl_->setShadeStyle(style);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    LineBatch brackets;
    LineBatch shapes;
    bool navigating = drag_ == Drag::Pan || drag_ == Drag::Orbit || drag_ == Drag::ZoomDrag || drag_ == Drag::Cube ||
                      drag_ == Drag::Ring || viewAnim_->isActive() || property("navigating").toBool();
    bool degrade = (ui.adaptiveDegradation() || quality_ == ViewportQuality::Performance) && navigating;
    for (const auto& up : scene.nodes) {
      const Node& n = *up;
      ObjectKind kind = n.kind();
      if ((kind != ObjectKind::Geometry && kind != ObjectKind::Shape) || !nodeShown(n)) continue;
      bool sel = doc_->isSelected(n.id);
      bool frozen = scene.isNodeFrozen(n) && ui.showFrozenInGray();
      const Material* mat = scene.findMaterial(n.materialId);
      Vec3 matColor = n.wireColor;
      if (mat) {
        matColor = mat->params.getColor("diffuse");
        if (!mat->params.getString("diffuseMap").empty()) matColor = Vec3(0.72f);
        Vec3 refr = mat->params.getColor("refract");
        if (maxComponent(refr) > 0.3f) matColor = lerp(matColor, Vec3(0.75f, 0.85f, 0.9f), 0.6f);
        matColor = vmax(matColor, Vec3(0.04f));
      }
      QColor wire = frozen ? theme::frozen() : qcolor(ui.wireUsesObjectColor() ? n.wireColor : matColor);
      if (kind == ObjectKind::Shape || n.isShapeResult()) {
        // Splines draw as lines; a viewport-renderable spline also shows its tube below.
        Mat4 m = n.objectWorldMatrix();
        QColor c = sel ? theme::selection() : wire;
        for (const auto& line : n.shapeLines())
          for (size_t i = 1; i < line.size(); ++i) shapes.line(m.transformPoint(line[i - 1]), m.transformPoint(line[i]), c);
        if (n.triMesh().empty()) continue;
      }
      bool asBox = n.displayAsBox || shading_ == ShadingMode::BoundingBox ||
                   (degrade && n.triMesh().indices.size() / 3 > kDegradeTriangles);
      if (asBox) {
        drawBox(shapes, n, sel ? theme::selection() : wire);
        continue;
      }
      uint64_t stamp = doc_->geometryStamp(n.id);
      bool shaded = shading_ != ShadingMode::Wireframe || (sel && ui.shadeSelected());
      if (shaded) {
        Vec3 c = ui.shadedUsesObjectColor() ? n.wireColor : matColor;
        if (shading_ == ShadingMode::Clay) c = Vec3(0.78f, 0.66f, 0.55f);
        if (frozen) c = Vec3(0.45f);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(1.0f, 1.0f);
        if (n.backfaceCull) {
          glEnable(GL_CULL_FACE);
          glCullFace(GL_BACK);
        }
        if (n.seeThrough) glDepthMask(GL_FALSE);
        gl_->drawShaded(n, stamp, vp, cam_.eye(), cam_.forward(), cam_.orthographic(), qcolor(c),
                        n.seeThrough ? 0.4f : 1.0f);
        glDepthMask(GL_TRUE);
        glDisable(GL_CULL_FACE);
        glDisable(GL_POLYGON_OFFSET_FILL);
        if (shading_ == ShadingMode::Wireframe) gl_->drawWire(n, stamp, vp, sel ? theme::selection() : wire);
        else if (edgedFaces_) gl_->drawWire(n, stamp, vp, sel ? theme::selection() : wire.darker(220));
        if (sel && ui.showSelectionBrackets()) drawBrackets(brackets, n);
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

    if (gizmoVisible()) {
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
    if (!nodeShown(n) || n.kind() == ObjectKind::Geometry || n.kind() == ObjectKind::Shape) continue;
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


void Viewport::drawRegion(QPainter& p) {
  if (drag_ != Drag::Region || (!moved_ && !fenceOpen_)) return;
  const UiState& ui = UiState::instance();
  QPen pen(QColor(255, 255, 255), 1, ui.windowSelection() ? Qt::SolidLine : Qt::DashLine);
  p.setPen(pen);
  p.setBrush(QColor(120, 170, 255, 30));
  switch (ui.regionShape()) {
    case RegionShape::Rectangle: p.drawRect(QRect(pressPos_, lastPos_).normalized()); break;
    case RegionShape::Circle: {
      double r = std::hypot(lastPos_.x() - pressPos_.x(), lastPos_.y() - pressPos_.y());
      p.drawEllipse(QPointF(pressPos_), r, r);
      break;
    }
    case RegionShape::Fence: {
      QPolygonF poly(regionPoints_);
      poly << QPointF(lastPos_);
      p.drawPolyline(poly);
      p.setPen(QPen(QColor(255, 255, 255, 120), 1, Qt::DotLine));
      p.drawLine(QPointF(lastPos_), regionPoints_.isEmpty() ? QPointF(pressPos_) : regionPoints_.front());
      break;
    }
    case RegionShape::Lasso: {
      QPolygonF poly(regionPoints_);
      poly << QPointF(lastPos_);
      p.drawPolygon(poly);
      break;
    }
    case RegionShape::Paint: {
      float r = ui.paintBrushRadius();
      p.setPen(Qt::NoPen);
      p.setBrush(QColor(120, 170, 255, 60));
      for (const QPointF& c : regionPoints_) p.drawEllipse(c, r, r);
      p.setPen(QPen(QColor(255, 255, 255), 1));
      p.setBrush(Qt::NoBrush);
      p.drawEllipse(QPointF(lastPos_), r, r);
      break;
    }
  }
}

void Viewport::drawOverlay(QPainter& p) {
  p.setRenderHint(QPainter::Antialiasing);
  p.setFont(font());
  const UiState& ui = UiState::instance();

  // View label: [+] [View] [Quality] [Shading]; the segment under the mouse is highlighted.
  {
    const QString parts[4] = {QStringLiteral("[+]"), QStringLiteral("[%1]").arg(viewLabel()),
                              QStringLiteral("[%1]").arg(qualityLabel()), QStringLiteral("[%1]").arg(shadingLabel())};
    int hoverSeg = labelSegmentAt(hover_.toPoint());
    for (int i = 0; i < 4; ++i) {
      QRect r = labelSegment(i);
      p.setPen(i == hoverSeg ? theme::activeBorder() : (active_ ? QColor(240, 240, 240) : QColor(200, 200, 200)));
      p.drawText(r, Qt::AlignCenter, parts[i]);
    }
  }

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
  if (gizmoVisible() && gizmo_.tool == TransformTool::Move) {
    const char* names[3] = {"X", "Y", "Z"};
    QColor cols[3] = {theme::axisX(), theme::axisY(), theme::axisZ()};
    for (int a = 0; a < 3; ++a) {
      float tx, ty;
      if (!gizmo_.axisTip(cam_, a, tx, ty)) continue;
      p.setPen(cols[a].lighter(120));
      p.drawText(QRectF(tx - 8, ty - 8, 16, 16), Qt::AlignCenter, names[a]);
    }
  }

  drawRegion(p);

  // Safe frame: camera views always show the render frame; Shift+F adds it (with action and
  // title safe areas) to any view.
  if (cam_.type == ViewType::Camera || safeFrame_) {
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
    p.setBrush(Qt::NoBrush);
    if (safeFrame_) {
      // Darken outside the render frame.
      QPainterPath outside;
      outside.addRect(QRectF(rect()));
      QPainterPath inside;
      inside.addRect(fr);
      p.fillPath(outside.subtracted(inside), QColor(0, 0, 0, 90));
      p.setPen(QPen(QColor(90, 200, 240, 170), 1, Qt::DashLine));
      p.drawRect(fr.adjusted(fr.width() * 0.05, fr.height() * 0.05, -fr.width() * 0.05, -fr.height() * 0.05));
      p.setPen(QPen(QColor(240, 140, 60, 170), 1, Qt::DashLine));
      p.drawRect(fr.adjusted(fr.width() * 0.1, fr.height() * 0.1, -fr.width() * 0.1, -fr.height() * 0.1));
    }
    p.setPen(QPen(QColor(230, 200, 60, 180), 1));
    p.drawRect(fr.adjusted(1, 1, -1, -1));
  }

  // ViewCube.
  if (ui.showViewCube() && cam_.type != ViewType::Camera) {
    cube_.layout(size());
    cube_.paint(p, cam_, hover_, active_);
  }

  // Selection lock / isolate markers.
  if (ui.selectionLocked()) {
    p.setPen(theme::activeBorder());
    p.drawText(QRect(0, height() - 22, width() - 10, 18), Qt::AlignRight | Qt::AlignVCenter, tr("Selection Locked"));
  }

  // Active border.
  p.setBrush(Qt::NoBrush);
  if (active_) {
    p.setPen(QPen(theme::activeBorder(), 2));
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

bool Viewport::gizmoVisible() const { return gizmoAvailable() && UiState::instance().showTransformGizmo(); }

void Viewport::transformFrame(Vec3 axes[3], Vec3& origin) const {
  const UiState& ui = UiState::instance();
  const Scene& scene = doc_->scene();
  axes[0] = Vec3(1, 0, 0);
  axes[1] = Vec3(0, 1, 0);
  axes[2] = Vec3(0, 0, 1);
  auto sel = doc_->selectedNodes();
  Node* first = sel.isEmpty() ? nullptr : sel[0];
  auto fromMatrix = [&](const Mat4& m) {
    Vec3 a[3] = {m.column(0), m.column(1), m.column(2)};
    for (const Vec3& v : a)
      if (length(v) < 1e-6f) return;
    for (int i = 0; i < 3; ++i) axes[i] = normalize(a[i]);
  };
  auto screenAxes = [&] {
    axes[0] = cam_.right();
    axes[1] = cam_.up();
    axes[2] = -cam_.forward();
  };
  RefCoordSys rc = ui.refCoordSys();
  Mat4 wp;
  Node* picked = scene.findNode(ui.pickCoordNode());
  if (state_->tool() == TransformTool::Scale && first) {
    // Scale factors apply along the object's own axes (no shear), so the scale gizmo is local.
    fromMatrix(first->worldMatrix());
  } else {
    switch (rc) {
      case RefCoordSys::View:
        if (cam_.orthographic()) screenAxes();
        break;
      case RefCoordSys::Screen: screenAxes(); break;
      case RefCoordSys::World:
      case RefCoordSys::Grid: break;
      case RefCoordSys::Parent:
        if (first) fromMatrix(first->parentWorldMatrix());
        break;
      case RefCoordSys::Local:
      case RefCoordSys::Gimbal:
      case RefCoordSys::LocalAligned:
        if (first) fromMatrix(first->worldMatrix());
        break;
      case RefCoordSys::Working:
        if (workingPivot(scene, wp)) fromMatrix(wp);
        break;
      case RefCoordSys::Pick:
        if (picked) fromMatrix(picked->worldMatrix());
        break;
    }
  }
  // Transform center.
  switch (ui.pivotCenter()) {
    case PivotCenter::PivotPoint:
      origin = sel.size() == 1 ? first->worldPosition() : doc_->selectionCenter();
      break;
    case PivotCenter::Selection: {
      Aabb b = doc_->selectionBounds();
      origin = sel.size() > 1 && b.valid() ? b.center() : doc_->selectionCenter();
      break;
    }
    case PivotCenter::Coordinate:
      origin = Vec3();
      if (rc == RefCoordSys::Parent && first && first->parent()) origin = first->parent()->worldPosition();
      else if ((rc == RefCoordSys::Local || rc == RefCoordSys::Gimbal || rc == RefCoordSys::LocalAligned) && first)
        origin = first->worldPosition();
      else if (rc == RefCoordSys::Pick && picked) origin = picked->worldPosition();
      else if (rc == RefCoordSys::Working && workingPivot(scene, wp)) origin = wp.translation();
      break;
  }
}

void Viewport::setupGizmo() {
  updateCamera();
  Vec3 axes[3];
  Vec3 origin;
  transformFrame(axes, origin);
  if (drag_ == Drag::Gizmo) {
    for (int i = 0; i < 3; ++i) axes[i] = dragAxes_[i];
    if (state_->tool() != TransformTool::Move) origin = dragCenter_;
  }
  gizmo_.setup(cam_, state_->tool(), origin, axes, UiState::instance().gizmoSize());
}

uint32_t Viewport::pick(float px, float py, float* depthOut) const {
  Ray ray = cam_.rayAt(px, py);
  uint32_t best = 0;
  float bestT = kInfinity;
  const Scene& scene = doc_->scene();
  for (const auto& up : scene.nodes) {
    const Node& n = *up;
    if (!nodeShown(n) || scene.isNodeFrozen(n)) continue;
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
          Vec3 wpt = lerp(wa, wb, u);
          float t = cam_.orthographic() ? dot(wpt - ray.o, ray.d) : length(wpt - ray.o);
          if (t < bestT) {
            bestT = t;
            best = n.id;
          }
        }
    } else {
      if (cam_.type == ViewType::Camera && n.id == cam_.cameraNodeId) continue;
      Vec3 wpt = n.worldPosition();
      float sx, sy, depth;
      if (!cam_.project(wpt, sx, sy, &depth)) continue;
      if (std::hypot(sx - px, sy - py) < 12.0f) {
        float t = cam_.orthographic() ? dot(wpt - ray.o, ray.d) : depth;
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

bool Viewport::nodeScreenPoints(const Node& n, std::vector<QPointF>& pts,
                                std::vector<std::pair<int, int>>* edges) const {
  const Scene& scene = doc_->scene();
  pts.clear();
  if (edges) edges->clear();
  if (!nodeShown(n) || scene.isNodeFrozen(n) || !state_->selectionFilterAccepts(n)) return false;
  if (cam_.type == ViewType::Camera && n.id == cam_.cameraNodeId) return false;
  if (n.kind() != ObjectKind::Geometry && n.kind() != ObjectKind::Shape) {
    float sx, sy;
    if (cam_.project(n.worldPosition(), sx, sy)) pts.emplace_back(sx, sy);
    return true;
  }
  Mat4 m = n.objectWorldMatrix();
  for (const auto& line : n.shapeLines()) {
    int prev = -1;
    for (const auto& p : line) {
      float sx, sy;
      if (!cam_.project(m.transformPoint(p), sx, sy)) {
        prev = -1;
        continue;
      }
      pts.emplace_back(sx, sy);
      int cur = static_cast<int>(pts.size()) - 1;
      if (edges && prev >= 0) edges->emplace_back(prev, cur);
      prev = cur;
    }
  }
  const TriMesh& t = n.triMesh();
  size_t count = t.wirePositions.size();
  if (count == 0) return true;
  size_t step = std::max<size_t>(1, count / 4000);
  std::vector<int> index(count, -1);
  for (size_t i = 0; i < count; i += step) {
    float sx, sy;
    if (!cam_.project(m.transformPoint(t.wirePositions[i]), sx, sy)) continue;
    index[i] = static_cast<int>(pts.size());
    pts.emplace_back(sx, sy);
  }
  if (edges) {
    for (size_t i = 0; i + 1 < t.wireIndices.size(); i += 2 * step) {
      uint32_t a = t.wireIndices[i], b = t.wireIndices[i + 1];
      auto at = [&](uint32_t v) {
        if (index[v] >= 0) return index[v];
        float sx, sy;
        if (!cam_.project(m.transformPoint(t.wirePositions[v]), sx, sy)) return -1;
        index[v] = static_cast<int>(pts.size());
        pts.emplace_back(sx, sy);
        return index[v];
      };
      int ia = at(a), ib = at(b);
      if (ia >= 0 && ib >= 0) edges->emplace_back(ia, ib);
    }
  }
  return true;
}

QVector<uint32_t> Viewport::pickNodesInPolygon(const QPolygonF& poly, bool window) const {
  QVector<uint32_t> out;
  if (poly.size() < 3) return out;
  QRectF bounds = poly.boundingRect();
  std::vector<QPointF> pts;
  std::vector<std::pair<int, int>> edges;
  for (const auto& up : doc_->scene().nodes) {
    const Node& n = *up;
    if (!nodeScreenPoints(n, pts, window ? nullptr : &edges) || pts.empty()) continue;
    auto inside = [&](const QPointF& q) { return bounds.contains(q) && poly.containsPoint(q, Qt::OddEvenFill); };
    bool hit;
    if (window) {
      hit = true;
      for (const QPointF& q : pts)
        if (!inside(q)) {
          hit = false;
          break;
        }
    } else {
      hit = false;
      for (const QPointF& q : pts)
        if (inside(q)) {
          hit = true;
          break;
        }
      // Crossing: also accept edges passing through the region (sampled).
      for (size_t e = 0; e < edges.size() && !hit; ++e) {
        const QPointF& a = pts[edges[e].first];
        const QPointF& b = pts[edges[e].second];
        for (int k = 1; k < 8 && !hit; ++k) {
          double f = k / 8.0;
          if (inside(a + (b - a) * f)) hit = true;
        }
      }
      // A region drawn inside a surface touches it too.
      QPointF c = bounds.center();
      if (!hit && !n.triMesh().empty() && inside(c)) {
        Ray ray = cam_.rayAt(static_cast<float>(c.x()), static_cast<float>(c.y()));
        Mat4 inv = n.objectWorldMatrix().inverse();
        hit = intersectTriMesh(n.triMesh(), Ray{inv.transformPoint(ray.o), inv.transformDir(ray.d)}) > 0;
      }
    }
    if (hit) out.push_back(n.id);
  }
  return out;
}

QVector<uint32_t> Viewport::pickNodesWithBrush(const QVector<QPointF>& centers, float radius) const {
  QVector<uint32_t> out;
  if (centers.isEmpty()) return out;
  std::vector<QPointF> pts;
  std::vector<std::pair<int, int>> edges;
  float r2 = radius * radius;
  auto near = [&](const QPointF& q) {
    for (const QPointF& c : centers) {
      double dx = q.x() - c.x(), dy = q.y() - c.y();
      if (dx * dx + dy * dy <= r2) return true;
    }
    return false;
  };
  for (const auto& up : doc_->scene().nodes) {
    const Node& n = *up;
    if (!nodeScreenPoints(n, pts, &edges)) continue;
    bool hit = false;
    for (const QPointF& q : pts)
      if (near(q)) {
        hit = true;
        break;
      }
    for (size_t e = 0; e < edges.size() && !hit; ++e) {
      const QPointF& a = pts[edges[e].first];
      const QPointF& b = pts[edges[e].second];
      for (int k = 1; k < 8 && !hit; ++k)
        if (near(a + (b - a) * (k / 8.0))) hit = true;
    }
    if (hit) out.push_back(n.id);
  }
  return out;
}

QVector<uint32_t> Viewport::regionPick(const QRect& r) const { return pickNodesInPolygon(QPolygonF(QRectF(r)), false); }

void Viewport::applyRegionSelection(const QVector<uint32_t>& ids) {
  bool ctrl = pressMods_ & Qt::ControlModifier, alt = pressMods_ & Qt::AltModifier;
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
}

void Viewport::finishRegion(const QPoint& releasePos) {
  const UiState& ui = UiState::instance();
  bool window = ui.windowSelection();
  QVector<uint32_t> ids;
  switch (ui.regionShape()) {
    case RegionShape::Rectangle:
      ids = pickNodesInPolygon(QPolygonF(QRectF(QRect(pressPos_, releasePos).normalized())), window);
      break;
    case RegionShape::Circle: {
      double r = std::hypot(releasePos.x() - pressPos_.x(), releasePos.y() - pressPos_.y());
      QPolygonF circle;
      for (int i = 0; i < 48; ++i) {
        double a = 2.0 * 3.14159265358979 * i / 48.0;
        circle << QPointF(pressPos_) + QPointF(std::cos(a) * r, std::sin(a) * r);
      }
      ids = pickNodesInPolygon(circle, window);
      break;
    }
    case RegionShape::Fence:
    case RegionShape::Lasso: {
      QPolygonF poly(regionPoints_);
      if (ui.regionShape() == RegionShape::Lasso) poly << QPointF(releasePos);
      ids = pickNodesInPolygon(poly, window);
      break;
    }
    case RegionShape::Paint: {
      QVector<QPointF> centers = regionPoints_;
      centers << QPointF(releasePos);
      ids = pickNodesWithBrush(centers, ui.paintBrushRadius());
      break;
    }
  }
  drag_ = Drag::None;
  fenceOpen_ = false;
  regionPoints_.clear();
  applyRegionSelection(ids);
  update();
}

void Viewport::beginTransformDrag(GizmoHandle h, float px, float py, bool cloneFirst) {
  TransformTool tool = state_->tool();
  QString label = tool == TransformTool::Move ? tr("Move") : tool == TransformTool::Rotate ? tr("Rotate") : tr("Scale");
  doc_->beginEdit(cloneFirst ? tr("Clone") : label);
  cloneDrag_ = cloneFirst;
  cloneSources_.clear();
  if (cloneFirst) {
    QVector<uint32_t> originals = doc_->selection();
    QVector<uint32_t> clones = doc_->cloneSelected(true);
    for (int i = 0; i < clones.size() && i < originals.size(); ++i) cloneSources_.push_back({clones[i], originals[i]});
  }
  drag_ = Drag::None;
  setupGizmo();
  dragCenter_ = gizmo_.origin;
  for (int i = 0; i < 3; ++i) dragAxes_[i] = gizmo_.axes[i];
  dragStart_.clear();
  for (Node* n : doc_->selectedNodes()) dragStart_.push_back({n->id, n->transform, n->worldPosition()});
  gizmoDrag_.begin(cam_, gizmo_, h, px, py);
  if (tool != TransformTool::Rotate) state_->setConstraint(h);
  drag_ = Drag::Gizmo;
}

void Viewport::updateTransformDrag(float px, float py) {
  // Drags work in world space; results are written back into each node's local transform so
  // linked children move correctly.
  const UiState& ui = UiState::instance();
  Scene& scene = doc_->scene();
  QVector<uint32_t> ids;
  TransformTool tool = state_->tool();
  bool multi = dragStart_.size() > 1;
  // Rotate/scale about each pivot (Pivot Point Center) or about the shared center.
  bool aroundCenter = ui.pivotCenter() == PivotCenter::Coordinate || (multi && ui.pivotCenter() != PivotCenter::PivotPoint);
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
        applyTransformLocks(scene, s.id, s.xf, n->transform);
        ids.push_back(s.id);
      }
  } else if (tool == TransformTool::Rotate) {
    Vec3 axis;
    float angle = gizmoDrag_.rotation(cam_, px, py, axis);
    angle = state_->snapAngle(angle);
    dragDisplay_ = Vec3(degrees(angle), 0, 0);
    Mat4 r = Mat4::rotateAxis(axis, angle);
    for (const auto& s : dragStart_)
      if (Node* n = scene.findNode(s.id)) {
        n->transform = s.xf;
        Vec3 localAxis = normalize(n->parentWorldMatrix().inverse().transformDir(axis));
        Mat4 rl = Mat4::rotateAxis(localAxis, angle);
        n->transform.rotation = eulerDegFromMatrix(rl * Mat4::fromEulerDeg(s.xf.rotation));
        if (aroundCenter) n->setWorldPosition(dragCenter_ + r.transformDir(s.worldPos - dragCenter_));
        applyTransformLocks(scene, s.id, s.xf, n->transform);
        ids.push_back(s.id);
      }
  } else if (tool == TransformTool::Scale) {
    Vec3 f = gizmoDrag_.scaling(cam_, px, py);
    int mask = handleAxisMask(gizmoDrag_.handle());
    float primary = std::fabs(f.x - 1.0f) > 1e-6f ? f.x : std::fabs(f.y - 1.0f) > 1e-6f ? f.y : f.z;
    ScaleMode mode = ui.scaleMode();
    if (mode == ScaleMode::Uniform || gizmoDrag_.handle() == GizmoHandle::Center || mask == 0 || mask == 7) {
      f = Vec3(primary);
    } else if (mode == ScaleMode::Squash) {
      // Volume-preserving: constrained axes scale by s, the others by s^(-k/(3-k)).
      int k = ((mask & 1) ? 1 : 0) + ((mask & 2) ? 1 : 0) + ((mask & 4) ? 1 : 0);
      float other = std::pow(std::max(primary, 0.01f), -static_cast<float>(k) / static_cast<float>(3 - k));
      f = Vec3((mask & 1) ? primary : other, (mask & 2) ? primary : other, (mask & 4) ? primary : other);
    }
    f = state_->snapPercent(f);
    dragDisplay_ = f * 100.0f;
    for (const auto& s : dragStart_)
      if (Node* n = scene.findNode(s.id)) {
        n->transform = s.xf;
        n->transform.scale = s.xf.scale * f;
        if (aroundCenter) n->setWorldPosition(dragCenter_ + (s.worldPos - dragCenter_) * f);
        applyTransformLocks(scene, s.id, s.xf, n->transform);
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
    const CloneOptionsDialog::Mode mode = dlg.mode();
    for (const auto& [cloneId, srcId] : cloneSources_) applyCloneMode(scene, srcId, cloneId, mode);
    QVector<uint32_t> created;
    for (const auto& s : dragStart_) created.push_back(s.id);
    for (int k = 2; k <= dlg.copies(); ++k)
      for (const auto& s : dragStart_) {
        Node* src = scene.findNode(s.id);
        if (!src) continue;
        Vec3 delta = src->transform.position - s.xf.position;
        auto copy = src->clone();
        copy->transform.position = s.xf.position + delta * static_cast<float>(k);
        uint32_t cid = scene.addNode(std::move(copy))->id;
        created.push_back(cid);
        applyCloneMode(scene, s.id, cid, mode);
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
    fenceOpen_ = false;
    regionPoints_.clear();
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

// ---------------------------------------------------------------------------------------------
// Label menus: [+] general, [View] point of view, [Quality] presets, [Shading] visual style.

void Viewport::showLabelMenu(int segment, const QPoint& pos) {
  QMenu menu(this);
  UiState& ui = UiState::instance();
  auto check = [](QAction* a, bool on) {
    a->setCheckable(true);
    a->setChecked(on);
    return a;
  };
  if (segment == 0) {
    menu.addAction(tr("Maximize Viewport"), this, [this] { Q_EMIT maximizeToggleRequested(); });
    menu.addSeparator();
    check(menu.addAction(tr("Show Grids"), this, [this] { state_->setShowGrid(!state_->showGrid()); }),
          state_->showGrid());
    check(menu.addAction(tr("ViewCube"), this, [&ui] { ui.setShowViewCube(!ui.showViewCube()); }), ui.showViewCube());
    check(menu.addAction(tr("Show Safe Frame"), this, [this] { setSafeFrame(!safeFrame_); }), safeFrame_);
    menu.addSeparator();
    QMenu* bg = menu.addMenu(tr("Viewport Background"));
    const struct {
      const char* text;
      ViewportBackground::Mode mode;
    } modes[] = {{"Gradient Color", ViewportBackground::Gradient},
                 {"Solid Color", ViewportBackground::Solid},
                 {"Environment Background", ViewportBackground::Environment},
                 {"Custom Image File", ViewportBackground::Image}};
    for (const auto& m : modes) {
      ViewportBackground::Mode mode = m.mode;
      check(bg->addAction(tr(m.text), this,
                          [this, mode] {
                            if (mode == ViewportBackground::Image && background_.imagePath.isEmpty()) {
                              Q_EMIT configureRequested(2);
                              return;
                            }
                            ViewportBackground b = background_;
                            b.mode = mode;
                            setBackground(b);
                          }),
            background_.mode == mode);
    }
    bg->addSeparator();
    bg->addAction(tr("Configure Viewport Background..."), this, [this] { Q_EMIT configureRequested(2); });
    menu.addSeparator();
    menu.addAction(tr("Undo View Change"), this, &Viewport::undoViewChange)->setEnabled(!history_.empty());
    menu.addAction(tr("Redo View Change"), this, &Viewport::redoViewChange)->setEnabled(!redo_.empty());
    menu.addAction(tr("Zoom Extents"), this, [this] { zoomExtents(false); });
    menu.addSeparator();
    menu.addAction(tr("Configure Viewports..."), this, [this] { Q_EMIT configureRequested(0); });
  } else if (segment == 1) {
    for (ViewType t : {ViewType::Perspective, ViewType::User, ViewType::Top, ViewType::Bottom, ViewType::Front,
                       ViewType::Back, ViewType::Left, ViewType::Right}) {
      QString name = t == ViewType::User ? tr("Orthographic") : viewTypeName(t);
      check(menu.addAction(name, this, [this, t] { setViewType(t); }), cam_.type == t);
    }
    QMenu* cams = menu.addMenu(tr("Cameras"));
    for (const auto& n : doc_->scene().nodes)
      if (n->camera()) {
        uint32_t id = n->id;
        check(cams->addAction(QString::fromStdString(n->name), this, [this, id] { setCameraView(id); }),
              cam_.type == ViewType::Camera && cam_.cameraNodeId == id);
      }
    cams->setEnabled(!cams->isEmpty());
    menu.addSeparator();
    check(menu.addAction(tr("Show Safe Frame"), this, [this] { setSafeFrame(!safeFrame_); }), safeFrame_);
    menu.addSeparator();
    menu.addAction(tr("Create Camera From View"), this, [this] { createCameraFromView(); })
        ->setEnabled(cam_.type == ViewType::Perspective);
    menu.addAction(tr("Save Active View"), this, &Viewport::saveNamedView);
    menu.addAction(tr("Restore Active View"), this, [this] { restoreNamedView(); })->setEnabled(savedView_.has_value());
  } else if (segment == 2) {
    const struct {
      const char* text;
      ViewportQuality q;
      const char* tip;
    } qs[] = {{"High Quality", ViewportQuality::HighQuality, "Scene lights and full detail"},
              {"Standard", ViewportQuality::Standard, "Default lights"},
              {"Performance", ViewportQuality::Performance, "Single light; heavy objects show as boxes while navigating"}};
    for (const auto& q : qs) {
      ViewportQuality quality = q.q;
      QAction* a = check(menu.addAction(tr(q.text), this, [this, quality] { setQuality(quality); }), quality_ == q.q);
      a->setToolTip(tr(q.tip));
    }
    menu.setToolTipsVisible(true);
  } else {
    const struct {
      const char* text;
      ShadingMode m;
    } styles[] = {{"Default Shading", ShadingMode::Shaded}, {"Facets", ShadingMode::Facets},
                  {"Clay", ShadingMode::Clay},              {"Bounding Box", ShadingMode::BoundingBox},
                  {"Wireframe Override", ShadingMode::Wireframe}};
    for (const auto& s : styles) {
      ShadingMode m = s.m;
      check(menu.addAction(tr(s.text), this, [this, m] { setShading(m); }), shading_ == m);
    }
    menu.addSeparator();
    check(menu.addAction(tr("Edged Faces"), this, [this] { setEdgedFaces(!edgedFaces_); }), edgedFaces_);
    QMenu* sel = menu.addMenu(tr("Display Selected"));
    check(sel->addAction(tr("Shade Selected"), this, [&ui] { ui.setShadeSelected(!ui.shadeSelected()); }),
          ui.shadeSelected());
    check(sel->addAction(tr("Selection Brackets"), this,
                         [&ui] { ui.setShowSelectionBrackets(!ui.showSelectionBrackets()); }),
          ui.showSelectionBrackets());
    QMenu* light = menu.addMenu(tr("Lighting and Shadows"));
    check(light->addAction(tr("Illuminate with Default Lights"), this, [this] { setLighting(ViewportLighting::Default); }),
          lighting_ == ViewportLighting::Default);
    check(light->addAction(tr("Illuminate with Scene Lights"), this, [this] { setLighting(ViewportLighting::Scene); }),
          lighting_ == ViewportLighting::Scene);
    menu.addSeparator();
    menu.addAction(tr("Configure..."), this, [this] { Q_EMIT configureRequested(1); });
  }
  menu.exec(pos);
}

// ---------------------------------------------------------------------------------------------
// Input

void Viewport::mousePressEvent(QMouseEvent* e) {
  if (!active_) Q_EMIT activated(this);
  setFocus();
  updateCamera();
  float px = static_cast<float>(e->position().x()), py = static_cast<float>(e->position().y());
  QPoint pos = e->position().toPoint();
  const UiState& ui = UiState::instance();

  // Fence region in progress: clicks add corners; right-click cancels; a click near the first
  // corner closes the fence.
  if (fenceOpen_) {
    if (e->button() == Qt::RightButton) {
      cancelInteraction();
    } else if (e->button() == Qt::LeftButton) {
      QPointF first = regionPoints_.isEmpty() ? QPointF(pressPos_) : regionPoints_.front();
      if (regionPoints_.size() >= 3 && std::hypot(first.x() - px, first.y() - py) < 8.0) finishRegion(pos);
      else regionPoints_ << QPointF(pos);
      update();
    }
    return;
  }

  pressPos_ = lastPos_ = pos;
  moved_ = false;
  pressMods_ = e->modifiers();

  if (drag_ == Drag::Gizmo || drag_ == Drag::Create) {
    if (e->button() == Qt::RightButton) cancelInteraction();
    return;
  }

  int seg = labelSegmentAt(pressPos_);
  if (seg >= 0 && e->button() != Qt::MiddleButton) {
    showLabelMenu(seg, e->globalPosition().toPoint());
    return;
  }

  // ViewCube.
  if (ui.showViewCube() && cam_.type != ViewType::Camera && e->button() == Qt::LeftButton) {
    cube_.layout(size());
    Vec3 dir;
    ViewCube::Hit hit = cube_.hitTest(cam_, e->position(), &dir);
    if (hit == ViewCube::Hit::Home) {
      goHome();
      return;
    }
    if (hit == ViewCube::Hit::Region || hit == ViewCube::Hit::Body) {
      pushViewHistory();
      viewAnim_->stop();
      drag_ = Drag::Cube;
      return;
    }
    if (hit == ViewCube::Hit::Ring) {
      pushViewHistory();
      viewAnim_->stop();
      drag_ = Drag::Ring;
      return;
    }
  }

  // An active viewport tool (sub-object editing, custom creation, ...) sees clicks first.
  if (e->button() != Qt::MiddleButton && drag_ == Drag::None)
    if (auto tool = state_->viewportToolShared(); tool && tool->mousePress(*this, e)) {
      update();
      return;
    }

  if (e->button() == Qt::MiddleButton) {
    pushViewHistory();
    viewAnim_->stop();
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
  if (gizmoVisible() && !ctrl && !alt) {
    setupGizmo();
    GizmoHandle h = gizmo_.hitTest(cam_, px, py);
    if (h != GizmoHandle::None) {
      beginTransformDrag(h, px, py, shift && tool == TransformTool::Move);
      return;
    }
  }
  auto constraintHandle = [&] {
    GizmoHandle h = state_->constraint();
    if (tool == TransformTool::Rotate)
      h = handleAxisMask(h) == 1 ? GizmoHandle::X : handleAxisMask(h) == 2 ? GizmoHandle::Y : GizmoHandle::Z;
    return h;
  };
  // Selection lock (Space): the selection cannot change; with a transform tool a drag anywhere
  // transforms the locked selection.
  if (ui.selectionLocked()) {
    if (tool != TransformTool::Select && gizmoAvailable()) {
      setupGizmo();
      beginTransformDrag(constraintHandle(), px, py, shift && tool == TransformTool::Move);
    }
    return;
  }
  uint32_t hit = pick(px, py);
  if (hit && !ctrl && !alt) {
    if (!doc_->isSelected(hit)) doc_->select(hit, false);
    if (tool != TransformTool::Select) {
      setupGizmo();
      beginTransformDrag(constraintHandle(), px, py, shift && tool == TransformTool::Move);
      return;
    }
    drag_ = Drag::None;
    return;
  }
  drag_ = Drag::Region;
  regionPoints_.clear();
  regionPoints_ << QPointF(pos);
}

void Viewport::mouseMoveEvent(QMouseEvent* e) {
  float px = static_cast<float>(e->position().x()), py = static_cast<float>(e->position().y());
  QPoint pos = e->position().toPoint();
  QPoint delta = pos - lastPos_;
  if ((pos - pressPos_).manhattanLength() > 3) moved_ = true;
  updateCamera();
  QPointF oldHover = hover_;
  hover_ = e->position();
  if (e->buttons() == Qt::NoButton && drag_ == Drag::None) {
    // Repaint for label / ViewCube hover highlights.
    bool overChrome = labelRect().contains(pos) || labelRect().contains(oldHover.toPoint()) ||
                      cube_.rect().adjusted(-8, -8, 8, 20).contains(hover_) ||
                      cube_.rect().adjusted(-8, -8, 8, 20).contains(oldHover);
    if (overChrome) update();
  }

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
    case Drag::Cube:
      cam_.orbit(static_cast<float>(delta.x()), static_cast<float>(delta.y()));
      update();
      Q_EMIT viewChanged();
      break;
    case Drag::Ring:
      cam_.orbit(static_cast<float>(delta.x()), 0.0f);
      update();
      Q_EMIT viewChanged();
      break;
    case Drag::ZoomDrag:
      cam_.zoom(std::pow(1.01f, static_cast<float>(delta.y())), static_cast<float>(width()) * 0.5f,
                static_cast<float>(height()) * 0.5f);
      update();
      break;
    case Drag::Region: {
      RegionShape shape = UiState::instance().regionShape();
      if ((shape == RegionShape::Lasso || shape == RegionShape::Paint) && (e->buttons() & Qt::LeftButton)) {
        QPointF last = regionPoints_.isEmpty() ? QPointF(pressPos_) : regionPoints_.back();
        if (std::hypot(last.x() - pos.x(), last.y() - pos.y()) > 3.0) regionPoints_ << QPointF(pos);
      }
      update();
      break;
    }
    case Drag::Gizmo: updateTransformDrag(px, py); break;
    case Drag::Create: createMove(px, py, true); break;
    case Drag::None:
      if (createStage_ == 2) {
        createMove(px, py, false);
      } else if (gizmoVisible() && e->buttons() == Qt::NoButton) {
        setupGizmo();
        GizmoHandle h = gizmo_.hitTest(cam_, px, py);
        if (h != hot_) {
          hot_ = h;
          update();
        }
      }
      if (state_->creating()) setCursor(Qt::CrossCursor);
      else if (hot_ != GizmoHandle::None && gizmoVisible()) setCursor(Qt::SizeAllCursor);
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
    case Drag::Cube: {
      drag_ = Drag::None;
      if (!moved_) {
        // A click (not a drag): turn the view to the clicked face, edge or corner.
        history_.pop_back();
        Vec3 dir;
        if (cube_.hitTest(cam_, e->position(), &dir) == ViewCube::Hit::Region) lookFrom(dir, true);
      }
      update();
      return;
    }
    case Drag::Ring:
      drag_ = Drag::None;
      update();
      return;
    case Drag::Region: {
      if (fenceOpen_) return;
      bool ctrl = pressMods_ & Qt::ControlModifier, alt = pressMods_ & Qt::AltModifier;
      if (moved_) {
        if (UiState::instance().regionShape() == RegionShape::Fence) {
          // The first drag sets the first fence segment; then each click adds a corner.
          regionPoints_ << e->position();
          fenceOpen_ = true;
          state_->setPrompt(tr("Click to add fence corners; click the first corner or double-click to close"));
          update();
          return;
        }
        finishRegion(e->position().toPoint());
        return;
      }
      drag_ = Drag::None;
      regionPoints_.clear();
      uint32_t hit = pick(px, py);
      if (hit && ctrl) {
        if (doc_->isSelected(hit)) doc_->deselect(hit);
        else doc_->select(hit, true);
      } else if (hit && alt) {
        doc_->deselect(hit);
      } else if (!hit && !ctrl && !alt) {
        doc_->clearSelection();
      }
      update();
      return;
    }
    case Drag::Pan:
    case Drag::Orbit:
    case Drag::ZoomDrag:
      if (e->button() == Qt::MiddleButton) drag_ = Drag::None;
      update();
      return;
    case Drag::None:
      if (e->button() == Qt::RightButton && !state_->creating() && createStage_ == 0 && !moved_ &&
          labelSegmentAt(e->position().toPoint()) < 0)
        Q_EMIT contextMenuRequested(this, e->globalPosition().toPoint());
      return;
  }
}

void Viewport::mouseDoubleClickEvent(QMouseEvent* e) {
  if (fenceOpen_ && e->button() == Qt::LeftButton) {
    finishRegion(e->position().toPoint());
    return;
  }
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
  viewAnim_->stop();
  updateCamera();
  float steps = static_cast<float>(e->angleDelta().y()) / 120.0f;
  cam_.zoom(std::pow(0.87f, steps), static_cast<float>(e->position().x()), static_cast<float>(e->position().y()));
  update();
  Q_EMIT viewChanged();
}

void Viewport::leaveEvent(QEvent*) {
  hover_ = QPointF(-100, -100);
  if (hot_ != GizmoHandle::None && drag_ == Drag::None) hot_ = GizmoHandle::None;
  update();
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
    case QEvent::MouseButtonDblClick: mouseDoubleClickEvent(&ev); break;
    default: break;
  }
}

void Viewport::simulateDoubleClick(QPointF pos, Qt::MouseButton button) {
  simulateMouse(QEvent::MouseButtonDblClick, pos, button, button);
}

}  // namespace veraa
