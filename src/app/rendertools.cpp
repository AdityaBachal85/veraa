// Rendering module UI: Rendering menu commands (Environment and Effects, Batch Render, Print
// Size Assistant, Slate Material Editor, proxy export, Sun and Sky), interactive creation of
// the Veraa lights, camera and render helpers, their viewport gizmos and the displacement map
// rollout of VeraaDisplacementMod.
#include "rendertools.h"

#include <QAction>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <map>
#include <memory>

#include "editorstate.h"
#include "extension.h"
#include "mainwindow.h"
#include "materialeditor.h"
#include "materialwidgets.h"
#include "paramform.h"
#include "photometry.h"
#include "renderdialogs.h"
#include "renderobjects.h"
#include "rendersetup.h"
#include "slateeditor.h"
#include "theme.h"
#include "viewport.h"

namespace veraa {

QString findBundledAsset(const QString& relative) {
  QList<QDir> roots = {QDir(QCoreApplication::applicationDirPath()), QDir::current()};
#ifdef VERAA_SOURCE_DIR
  roots.push_back(QDir(QStringLiteral(VERAA_SOURCE_DIR)));
#endif
  for (QDir d : roots) {
    for (int i = 0; i < 5; ++i) {
      QString p = d.filePath("assets/" + relative);
      if (QFileInfo::exists(p)) return QFileInfo(p).absoluteFilePath();
      if (!d.cdUp()) break;
    }
  }
  return {};
}

Node* createFurOn(SceneDocument* doc, const Node* source) {
  if (!source) return nullptr;
  Node* fur = doc->createNode("VeraaFur");
  if (!fur) return nullptr;
  fur->object->params.setString("sourceNode", source->name);
  fur->transform.position = source->worldPosition();
  fur->materialId = source->materialId;
  fur->wireColor = source->wireColor;
  doc->notifyNodeChanged(fur, ChangeAll);
  return fur;
}

uint32_t exportProxy(SceneDocument* doc, const QVector<uint32_t>& nodeIds, const QString& path, bool replace,
                     QString* error) {
  Scene& scene = doc->scene();
  std::vector<Node*> nodes;
  for (uint32_t id : nodeIds)
    if (Node* n = scene.findNode(id); n && n->geometry() && !n->triMesh().empty()) nodes.push_back(n);
  if (nodes.empty()) {
    if (error) *error = QObject::tr("Select one or more geometry objects to export.");
    return 0;
  }
  // Combine everything in the space of the first node's pivot.
  Mat4 ref = nodes.front()->worldMatrix();
  Mat4 toRef = ref.inverse();
  TriMesh out;
  for (Node* n : nodes) {
    const TriMesh& t = n->triMesh();
    Mat4 m = toRef * n->objectWorldMatrix();
    Mat4 inv = m.inverse();
    auto base = static_cast<uint32_t>(out.positions.size());
    for (size_t i = 0; i < t.positions.size(); ++i) {
      Vec3 p = m.transformPoint(t.positions[i]);
      out.positions.push_back(p);
      out.bounds.expand(p);
      Vec3 nn = i < t.normals.size() ? inv.transformNormalWithInverse(t.normals[i]) : Vec3(0, 0, 1);
      out.normals.push_back(length2(nn) > 0.0f ? normalize(nn) : Vec3(0, 0, 1));
      out.uvs.push_back(i < t.uvs.size() ? t.uvs[i] : Vec2());
    }
    for (uint32_t idx : t.indices) out.indices.push_back(base + idx);
    for (size_t k = 0; k < t.triangleCount(); ++k) out.triMatIds.push_back(k < t.triMatIds.size() ? t.triMatIds[k] : 0);
  }
  std::string err;
  if (!writeProxyFile(path.toStdString(), out, &err)) {
    if (error) *error = QString::fromStdString(err);
    return 0;
  }
  Node* first = nodes.front();
  uint32_t materialId = first->materialId;
  Vec3 wire = first->wireColor;
  QString baseName = QFileInfo(path).completeBaseName();
  std::vector<uint32_t> sources;
  for (Node* n : nodes) sources.push_back(n->id);

  doc->beginEdit(QObject::tr("Export Proxy"));
  Node* proxy = doc->createNode("VeraaProxy");
  if (!proxy) {
    doc->cancelEdit();
    if (error) *error = QObject::tr("Could not create the proxy object.");
    return 0;
  }
  proxy->name = baseName.toStdString();
  proxy->object->params.setString("file", QFileInfo(path).absoluteFilePath().toStdString());
  proxy->setWorldMatrix(ref);
  proxy->materialId = materialId;
  proxy->wireColor = wire;
  proxy->invalidate();
  if (replace)
    for (uint32_t id : sources) scene.removeNode(id);
  doc->notifyStructureChanged();
  doc->setSelection({proxy->id});
  doc->endEdit();
  return proxy->id;
}

namespace {

QColor qcol(const Vec3& c) {
  return QColor::fromRgbF(clampf(c.x, 0.0f, 1.0f), clampf(c.y, 0.0f, 1.0f), clampf(c.z, 0.0f, 1.0f));
}

void circle(LineBatch& b, const Vec3& c, const Vec3& u, const Vec3& v, float r, const QColor& col, int segs = 32) {
  Vec3 prev = c + u * r;
  for (int i = 1; i <= segs; ++i) {
    float a = kTwoPi * static_cast<float>(i) / static_cast<float>(segs);
    Vec3 p = c + (u * std::cos(a) + v * std::sin(a)) * r;
    b.line(prev, p, col);
    prev = p;
  }
}

void arrow(LineBatch& b, const Vec3& from, const Vec3& to, const Vec3& viewDir, const QColor& col) {
  b.line(from, to, col);
  Vec3 d = to - from;
  float len = length(d);
  if (len <= 0.0f) return;
  d = d / len;
  Vec3 side = cross(d, viewDir);
  if (length2(side) < 1e-8f) side = cross(d, std::fabs(d.z) < 0.9f ? Vec3(0, 0, 1) : Vec3(1, 0, 0));
  side = normalize(side) * (len * 0.08f);
  b.line(to, to - d * (len * 0.2f) + side, col);
  b.line(to, to - d * (len * 0.2f) - side, col);
}

// Turns on the physical sky (and physical exposure when none is set) for a new sun.
bool enableSkyForSun(SceneDocument* doc) {
  ParamBlock& rs = doc->scene().renderSettings;
  bool changed = false;
  if (rs.getInt("envMode") != 3) changed |= rs.setNumber("envMode", 3);
  if (rs.getInt("exposureControl") == 0) {
    changed |= rs.setNumber("exposureControl", 1);
    rs.setNumber("ev100", 14.0);
  }
  if (changed) doc->notifyRenderSettingsChanged();
  return changed;
}

// ---------------------------------------------------------------------------------------------
// Interactive creation

class RenderCreateTool : public ViewportTool {
 public:
  explicit RenderCreateTool(QString type) : type_(std::move(type)) {}

  bool mousePress(Viewport& vp, QMouseEvent* e) override {
    EditorState* st = vp.editorState();
    if (st->createType() != type_) {
      st->setViewportTool(nullptr);
      return false;
    }
    if (e->button() == Qt::RightButton) {
      if (dragging_) {
        cancel();
      } else {
        st->setCreateType({});
        st->setViewportTool(nullptr);
      }
      return true;
    }
    if (e->button() != Qt::LeftButton) return false;
    if (dragging_) return true;
    doc_ = vp.document();
    float px = static_cast<float>(e->position().x()), py = static_cast<float>(e->position().y());

    if (type_ == "VeraaFur") {
      uint32_t id = vp.pickNode(px, py);
      Node* src = id ? doc_->scene().findNode(id) : nullptr;
      if (!src || !src->geometry() || src->object->type() == "VeraaFur") {
        st->setPrompt(QObject::tr("Click a geometry object to grow fur on it"));
        return true;
      }
      doc_->beginEdit(QObject::tr("Create VeraaFur"));
      Node* fur = createFurOn(doc_, src);
      doc_->endEdit();
      if (fur) doc_->select(fur->id, false);
      st->setPrompt(QObject::tr("Fur grows on %1. Click another object, or right-click to stop")
                        .arg(QString::fromStdString(src->name)));
      return true;
    }

    Vec3 hit;
    if (!vp.hitConstructionPlane(px, py, hit)) return true;
    Vec3 planePoint, normal;
    vp.camera().constructionPlane(planePoint, normal);
    ground_ = std::fabs(normal.z) > 0.5f;
    up_ = normalize(normal);
    if (dot(up_, vp.camera().forward()) > 0.0f) up_ = -up_;  // toward the viewer
    Vec3 rot = vp.camera().creationRotation();
    rot_ = Mat4::fromEulerDeg(rot);

    doc_->beginEdit(QObject::tr("Create %1").arg(QString::fromStdString(objectDisplayName(type_.toStdString()))));
    Node* n = doc_->createNode(type_);
    if (!n) {
      doc_->cancelEdit();
      return true;
    }
    node_ = n->id;
    p0_ = hit;
    pressPos_ = e->position();
    dragging_ = true;
    moved_ = false;
    n->transform.rotation = rot;
    n->transform.position = hit;
    ParamBlock& p = n->object->params;
    if (type_ == "VeraaLight") {
      lift_ = ground_ ? Vec3(0, 0, 250.0f) : Vec3();
      n->transform.position = hit + lift_;
      p.setNumber("width", 0.01);
      p.setNumber("length", 0.01);
    } else if (type_ == "VeraaIES") {
      n->transform.position = hit + (ground_ ? Vec3(0, 0, 250.0f) : Vec3());
      n->transform.rotation = Vec3();  // shining down
      QString ies = findBundledAsset("ies/downlight_wide.ies");
      if (!ies.isEmpty()) p.setString("iesFile", ies.toStdString());
    } else if (type_ == "VeraaSun") {
      Vec3 toSun = normalize(Vec3(0.5f, -0.6f, 0.65f));
      n->transform.position = hit + toSun * 1000.0f;
      n->transform.rotation = rotationLookingAlong(-toSun);
    } else if (type_ == "VeraaPhysicalCamera") {
      if (ground_) n->transform.position.z += 150.0f;
      Vec3 f = vp.camera().forward();
      Vec3 dir = std::fabs(f.z) > 0.95f ? Vec3(0, 1, 0) : normalize(Vec3(f.x, f.y, 0));
      n->transform.rotation = rotationLookingAlong(dir);
    } else if (type_ == "SunPositioner" || type_ == "VeraaInfinitePlane") {
      n->transform.rotation = ground_ ? Vec3() : rot;
    }
    doc_->select(n->id, false);
    doc_->notifyNodeChanged(n, ChangeAll);
    return true;
  }

  bool mouseMove(Viewport& vp, QMouseEvent* e) override {
    if (!dragging_) return false;
    if (!(e->buttons() & Qt::LeftButton)) return true;
    float px = static_cast<float>(e->position().x()), py = static_cast<float>(e->position().y());
    Vec3 hit;
    if (!vp.hitConstructionPlane(px, py, hit)) return true;
    if ((e->position() - pressPos_).manhattanLength() > 3.0) moved_ = true;
    if (!moved_) return true;
    Node* n = doc_->scene().findNode(node_);
    if (!n) return true;
    ParamBlock& p = n->object->params;
    Vec3 d = rot_.transposed().transformDir(hit - p0_);  // in the creation frame
    float dist = length(hit - p0_);
    if (type_ == "VeraaLight") {
      auto shape = static_cast<VeraaLightShape>(p.getInt("lightType"));
      if (shape == VeraaLightShape::Plane) {
        p.setNumber("width", std::max(std::fabs(d.x), 0.01f));
        p.setNumber("length", std::max(std::fabs(d.y), 0.01f));
        n->transform.position = p0_ + rot_.transformDir(Vec3(d.x * 0.5f, d.y * 0.5f, 0.0f)) + lift_;
      } else if (shape == VeraaLightShape::Disc || shape == VeraaLightShape::Sphere) {
        p.setNumber("lightRadius", std::max(std::sqrt(d.x * d.x + d.y * d.y), 0.01f));
      }
    } else if (type_ == "VeraaIES" || type_ == "VeraaPhysicalCamera") {
      Vec3 dir = hit - n->transform.position;
      if (length(dir) > 1e-3f) n->transform.rotation = rotationLookingAlong(dir);
      if (type_ == "VeraaPhysicalCamera") p.setNumber("focusDistance", std::max(length(dir), 1.0f));
    } else if (type_ == "VeraaSun") {
      if (dist > 1.0f) {
        Vec3 sunPos = hit + up_ * dist;  // 45 degrees above the dragged point
        n->transform.position = sunPos;
        n->transform.rotation = rotationLookingAlong(p0_ - sunPos);
      }
    } else if (type_ == "SunPositioner") {
      p.setNumber("compassRadius", std::max(dist, 10.0f));
    } else {
      n->transform.position = hit;
    }
    doc_->notifyNodeChanged(n, ChangeGeometry | ChangeTransform);
    return true;
  }

  bool mouseRelease(Viewport& vp, QMouseEvent* e) override {
    if (!dragging_) return false;
    if (e->button() != Qt::LeftButton) return true;
    Node* n = doc_->scene().findNode(node_);
    if (n && !moved_ && type_ == "VeraaLight") {
      // A click creates a 1 m panel.
      n->object->params.setNumber("width", 100.0);
      n->object->params.setNumber("length", 100.0);
      n->transform.position = p0_ + lift_;
      doc_->notifyNodeChanged(n, ChangeGeometry | ChangeTransform);
    }
    bool sky = false;
    if (n && (type_ == "VeraaSun" || type_ == "SunPositioner")) sky = enableSkyForSun(doc_);
    dragging_ = false;
    node_ = 0;
    doc_->endEdit();
    QString name = QString::fromStdString(objectDisplayName(type_.toStdString()));
    vp.editorState()->setPrompt(
        sky ? QObject::tr("Physical sky and physical exposure turned on for the sun (undo to revert). "
                          "Click and drag to create another %1")
                  .arg(name)
            : QObject::tr("Click and drag to create another %1, right-click to stop").arg(name));
    return true;
  }

  bool keyPress(Viewport&, QKeyEvent* e) override { return e->key() == Qt::Key_Escape && cancel(); }

  bool cancel() override {
    if (!dragging_) return false;
    dragging_ = false;
    node_ = 0;
    doc_->cancelEdit();
    return true;
  }

  QString prompt() const override {
    if (type_ == "VeraaLight") return QObject::tr("Click and drag to set the light's size (a click makes a 1 m panel)");
    if (type_ == "VeraaIES") return QObject::tr("Click to place the IES light, drag to aim it");
    if (type_ == "VeraaSun") return QObject::tr("Click the sun's target, drag to place the sun");
    if (type_ == "SunPositioner") return QObject::tr("Click to place the compass, drag to set its radius");
    if (type_ == "VeraaPhysicalCamera") return QObject::tr("Click to place the camera, drag to aim and set focus");
    if (type_ == "VeraaFur") return QObject::tr("Click a geometry object to grow fur on it");
    if (type_ == "VeraaProxy") return QObject::tr("Click to place the proxy, then pick its .vxp file in the Modify panel");
    return QObject::tr("Click to place the %1").arg(QString::fromStdString(objectDisplayName(type_.toStdString())));
  }

 private:
  QString type_;
  SceneDocument* doc_ = nullptr;
  uint32_t node_ = 0;
  bool dragging_ = false, moved_ = false, ground_ = true;
  QPointF pressPos_;
  Vec3 p0_, lift_, up_{0, 0, 1};
  Mat4 rot_;
};

// ---------------------------------------------------------------------------------------------
// Viewport gizmos

class RenderGizmos : public ViewportDecorator {
 public:
  void drawLines(Viewport& vp, LineBatch& b) override {
    SceneDocument* doc = vp.document();
    const Scene& scene = doc->scene();
    const ViewCamera& cam = vp.camera();
    for (const auto& up : scene.nodes) {
      const Node& n = *up;
      if (!n.object || !scene.isNodeVisible(n)) continue;
      const std::string type = n.object->type();
      if (type != "VeraaLight" && type != "VeraaIES" && type != "VeraaSun" && type != "SunPositioner" &&
          type != "VeraaPhysicalCamera" && type != "VeraaFur")
        continue;
      if (cam.type == ViewType::Camera && cam.cameraNodeId == n.id) continue;
      bool sel = doc->isSelected(n.id);
      QColor col = sel ? theme::selection() : qcol(n.wireColor);
      Mat4 m = n.worldMatrix();
      Vec3 o = m.translation();
      Vec3 ax = normalize(m.column(0)), ay = normalize(m.column(1)), az = normalize(m.column(2));
      float sz = cam.worldPerPixel(o) * 22.0f;
      const ParamBlock& p = n.object->params;
      if (type == "VeraaLight") drawVeraaLight(b, scene, n, m, o, ax, ay, az, sz, p, col, cam);
      else if (type == "VeraaIES") drawIes(b, scene, o, ax, ay, az, sz, p, col, cam);
      else if (type == "VeraaSun" || type == "SunPositioner") drawSun(b, n, o, az, sz, p, col, cam);
      else if (type == "VeraaPhysicalCamera") drawFocus(b, o, az, p, col, sz);
      else if (type == "VeraaFur") drawFur(b, scene, n, p, col);
    }
  }

 private:
  static void sunIcon(LineBatch& b, const Vec3& o, float sz, const QColor& col, const ViewCamera& cam) {
    circle(b, o, cam.right(), cam.up(), sz * 0.45f, col, 20);
    for (int i = 0; i < 8; ++i) {
      float a = kTwoPi * static_cast<float>(i) / 8.0f;
      Vec3 d = cam.right() * std::cos(a) + cam.up() * std::sin(a);
      b.line(o + d * (sz * 0.6f), o + d * (sz * 0.9f), col);
    }
  }

  void drawVeraaLight(LineBatch& b, const Scene& scene, const Node& n, const Mat4& m, const Vec3& o, const Vec3& ax,
                      const Vec3& ay, const Vec3& az, float sz, const ParamBlock& p, const QColor& col,
                      const ViewCamera& cam) {
    (void)n;
    auto shape = static_cast<VeraaLightShape>(p.getInt("lightType"));
    float scale = std::max(length(m.column(0)), 1e-6f);
    switch (shape) {
      case VeraaLightShape::Plane: {
        float w = p.getFloat("width") * 0.5f, l = p.getFloat("length") * 0.5f;
        Vec3 c00 = m.transformPoint({-w, -l, 0}), c10 = m.transformPoint({w, -l, 0}), c11 = m.transformPoint({w, l, 0}),
             c01 = m.transformPoint({-w, l, 0});
        b.line(c00, c10, col);
        b.line(c10, c11, col);
        b.line(c11, c01, col);
        b.line(c01, c00, col);
        b.line(c00, c11, col);
        b.line(c10, c01, col);
        float len = std::max(std::min(w, l) * scale, sz * 0.5f);
        arrow(b, o, o - az * len, cam.forward(), col);
        if (p.getBool("doubleSided")) arrow(b, o, o + az * len, cam.forward(), col);
        if (p.getBool("skylightPortal")) {
          QColor pc = col;
          pc.setAlphaF(0.6f);
          for (float f : {-0.5f, 0.0f, 0.5f}) b.line(m.transformPoint({w * f, -l, 0}), m.transformPoint({w * f, l, 0}), pc);
        }
        break;
      }
      case VeraaLightShape::Disc: {
        float r = p.getFloat("lightRadius") * scale;
        circle(b, o, ax, ay, r, col);
        b.line(o - ax * r, o + ax * r, col);
        b.line(o - ay * r, o + ay * r, col);
        arrow(b, o, o - az * std::max(r, sz * 0.5f), cam.forward(), col);
        break;
      }
      case VeraaLightShape::Sphere: {
        float r = p.getFloat("lightRadius") * scale;
        circle(b, o, ax, ay, r, col);
        circle(b, o, ay, az, r, col);
        circle(b, o, az, ax, r, col);
        break;
      }
      case VeraaLightShape::Mesh: {
        sunIcon(b, o, sz * 0.6f, col, cam);
        if (const Node* src = scene.findNodeByName(p.getString("meshNode"))) {
          Aabb box = src->worldBounds();
          QColor lc = col;
          lc.setAlphaF(0.7f);
          b.line(o, (box.lo + box.hi) * 0.5f, lc);
          Vec3 lo = box.lo, hi = box.hi;
          for (int i = 0; i < 4; ++i) {
            Vec3 a((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, lo.z), c((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, hi.z);
            b.line(a, c, lc);
          }
          for (float z : {lo.z, hi.z}) {
            b.line({lo.x, lo.y, z}, {hi.x, lo.y, z}, lc);
            b.line({hi.x, lo.y, z}, {hi.x, hi.y, z}, lc);
            b.line({hi.x, hi.y, z}, {lo.x, hi.y, z}, lc);
            b.line({lo.x, hi.y, z}, {lo.x, lo.y, z}, lc);
          }
        }
        break;
      }
      case VeraaLightShape::Dome: {
        float r = sz * 2.5f;
        Vec3 wx(1, 0, 0), wy(0, 1, 0), wz(0, 0, 1);
        circle(b, o, wx, wy, r, col);
        int segs = 16;
        bool full = p.getBool("domeSpherical");
        for (const Vec3& side : {wx, wy}) {
          Vec3 prev = o + side * r;
          for (int i = 1; i <= segs; ++i) {
            float a = (full ? kTwoPi : kPi) * static_cast<float>(i) / static_cast<float>(segs);
            Vec3 q = o + (side * std::cos(a) + wz * std::sin(a)) * r;
            b.line(prev, q, col);
            prev = q;
          }
        }
        arrow(b, o + wz * (r * 1.4f), o + wz * (r * 0.4f), cam.forward(), col);
        break;
      }
    }
  }

  void drawIes(LineBatch& b, const Scene& scene, const Vec3& o, const Vec3& ax, const Vec3& ay, const Vec3& az,
               float sz, const ParamBlock& p, const QColor& col, const ViewCamera& cam) {
    circle(b, o, cam.right(), cam.up(), sz * 0.25f, col, 12);
    std::shared_ptr<IesProfile> prof = profile(scene.resolvePath(p.getString("iesFile")));
    float R = sz * 3.0f;
    if (!prof || prof->maxCandela <= 0.0f) {
      // No file: a plain downward cone.
      Vec3 c = o - az * R;
      circle(b, c, ax, ay, R * 0.4f, col);
      for (const Vec3& d : {ax, ay, -ax, -ay}) b.line(o, c + d * (R * 0.4f), col);
      return;
    }
    // Photometric web: two vertical slices (0/180 and 90/270 degrees).
    for (int slice = 0; slice < 2; ++slice) {
      Vec3 side = slice == 0 ? ax : ay;
      for (int sign = -1; sign <= 1; sign += 2) {
        float h = static_cast<float>(slice * 90 + (sign < 0 ? 180 : 0));
        Vec3 prev = o;
        for (int i = 0; i <= 36; ++i) {
          float v = static_cast<float>(i) * 5.0f;
          float r = R * prof->intensity(v, h) / prof->maxCandela;
          float t = radians(v);
          Vec3 q = o + (-az * std::cos(t) + side * (static_cast<float>(sign) * std::sin(t))) * r;
          b.line(prev, q, col);
          prev = q;
        }
      }
    }
  }

  void drawSun(LineBatch& b, const Node& n, const Vec3& o, const Vec3& az, float sz, const ParamBlock& p,
               const QColor& col, const ViewCamera& cam) {
    const auto* sun = static_cast<const VeraaSunObject*>(n.object.get());
    bool positioned = p.getInt("positionMode") == 1;
    if (!positioned) {
      sunIcon(b, o, sz, col, cam);
      arrow(b, o, o - az * (sz * 5.0f), cam.forward(), col);
      return;
    }
    // Compass rose in the world XY plane, north rotated by northDirection.
    float r = p.getFloat("compassRadius");
    float north = radians(p.getFloat("northDirection"));
    Vec3 nDir(-std::sin(north), std::cos(north), 0.0f), eDir(std::cos(north), std::sin(north), 0.0f);
    circle(b, o, Vec3(1, 0, 0), Vec3(0, 1, 0), r, col, 48);
    b.line(o - nDir * r, o + nDir * (r * 1.25f), col);
    b.line(o - eDir * r, o + eDir * r, col);
    for (int i = 0; i < 8; ++i) {
      float a = kTwoPi * static_cast<float>(i) / 8.0f;
      Vec3 d = nDir * std::cos(a) + eDir * std::sin(a);
      b.line(o + d * (r * 0.92f), o + d * r, col);
    }
    // "N" glyph beyond the north tip.
    Vec3 nc = o + nDir * (r * 1.4f);
    float g = r * 0.08f;
    Vec3 l0 = nc - eDir * g - nDir * g, l1 = nc - eDir * g + nDir * g, r0 = nc + eDir * g - nDir * g,
         r1 = nc + eDir * g + nDir * g;
    b.line(l0, l1, col);
    b.line(l1, r0, col);
    b.line(r0, r1, col);
    // The sun on its sky dome.
    Vec3 toSun = sun->sunDirection(az);
    // Selected: white; below the horizon: dim.
    QColor sc = toSun.z < 0.0f ? QColor(120, 110, 90) : col == theme::selection() ? col : QColor(255, 200, 60);
    Vec3 sp = o + toSun * (r * 1.2f);
    sunIcon(b, sp, sz, sc, cam);
    b.line(sp, o, sc);
    // Sun path for the day (hourly), to show where the sun travels.
    VeraaSunObject probe(*sun);
    Vec3 prev;
    bool have = false;
    for (int hour = 0; hour <= 24; ++hour) {
      probe.params.setNumber("hour", static_cast<double>(hour));
      Vec3 d = probe.sunDirection(az);
      Vec3 q = o + d * (r * 1.2f);
      if (have && d.z > -0.05f) b.line(prev, q, QColor(200, 170, 80, 140));
      prev = q;
      have = true;
    }
  }

  static void drawFocus(LineBatch& b, const Vec3& o, const Vec3& az, const ParamBlock& p, const QColor& col, float sz) {
    if (!p.getBool("dof")) return;
    Vec3 f = o - az * p.getFloat("focusDistance");
    QColor fc = col;
    fc.setAlphaF(0.6f);
    b.line(o, f, fc);
    float s = sz * 0.3f;
    Vec3 u = std::fabs(az.z) < 0.9f ? normalize(cross(az, Vec3(0, 0, 1))) : Vec3(1, 0, 0);
    Vec3 v = cross(az, u);
    b.line(f - u * s, f + u * s, fc);
    b.line(f - v * s, f + v * s, fc);
  }

  void drawFur(LineBatch& b, const Scene& scene, const Node& n, const ParamBlock& p, const QColor& col) {
    const Node* src = scene.findNodeByName(p.getString("sourceNode"));
    if (!src || src == &n || src->triMesh().empty()) {
      furCache_.erase(n.id);
      return;
    }
    FurCache& c = furCache_[n.id];
    Mat4 m = src->objectWorldMatrix();
    std::string key = p.toJson().dump();
    bool same = c.stamp == src->geometryStamp() && c.params == key && c.valid;
    for (int i = 0; same && i < 4; ++i)
      for (int j = 0; same && j < 4; ++j) same = c.matrix.m[i][j] == m.m[i][j];
    if (!same) {
      TriMesh world = src->triMesh();
      Mat4 inv = m.inverse();
      for (auto& v : world.positions) v = m.transformPoint(v);
      for (auto& nn : world.normals) nn = normalize(inv.transformNormalWithInverse(nn));
      auto strands = generateFur(p, world, static_cast<size_t>(std::max(p.getInt("previewStrands"), 0)));
      c.lines.clear();
      for (const auto& s : strands)
        for (size_t i = 1; i < s.points.size(); ++i) c.lines.emplace_back(s.points[i - 1], s.points[i]);
      c.stamp = src->geometryStamp();
      c.params = key;
      c.matrix = m;
      c.valid = true;
    }
    for (const auto& l : c.lines) b.line(l.first, l.second, col);
  }

  std::shared_ptr<IesProfile> profile(const std::string& path) {
    if (path.empty()) return nullptr;
    QFileInfo fi(QString::fromStdString(path));
    if (!fi.exists()) return nullptr;
    qint64 stamp = fi.lastModified().toMSecsSinceEpoch();
    auto it = ies_.find(path);
    if (it != ies_.end() && it->second.first == stamp) return it->second.second;
    auto prof = std::make_shared<IesProfile>();
    if (!prof->load(path)) prof.reset();
    ies_[path] = {stamp, prof};
    return prof;
  }

  struct FurCache {
    uint64_t stamp = 0;
    std::string params;
    Mat4 matrix;
    bool valid = false;
    std::vector<std::pair<Vec3, Vec3>> lines;
  };
  std::map<uint32_t, FurCache> furCache_;
  std::map<std::string, std::pair<qint64, std::shared_ptr<IesProfile>>> ies_;
};

// ---------------------------------------------------------------------------------------------
// VeraaDisplacementMod: texture map rollout

QWidget* displacementRollout(const ModifyPanelContext& ctx, QWidget* parent) {
  SceneDocument* doc = ctx.doc;
  auto* roll = new Rollout(QObject::tr("Displacement map"), parent);
  auto* body = new QVBoxLayout;
  roll->form()->addRow(body);
  auto* row = new QHBoxLayout;
  auto* mapBtn = new QPushButton(roll->body());
  auto* clearBtn = new QPushButton(QObject::tr("Clear"), roll->body());
  row->addWidget(mapBtn, 1);
  row->addWidget(clearBtn);
  body->addLayout(row);
  auto* form = new ParamForm(roll->body());
  body->addWidget(form);
  auto* hint = new QLabel(QObject::tr("The map's value (gray) moves vertices along their normals by Amount. Without a "
                                      "map the Bitmap file parameter below is used."),
                          roll->body());
  hint->setWordWrap(true);
  hint->setStyleSheet("color: #a8a8a8;");
  body->addWidget(hint);

  auto resolve = [doc, ctx]() -> std::pair<Node*, VeraaDisplacementModifier*> {
    Node* n = doc->scene().findNode(ctx.nodeId);
    if (!n || ctx.stackIndex < 0 || ctx.stackIndex >= static_cast<int>(n->modifiers.size())) return {n, nullptr};
    return {n, dynamic_cast<VeraaDisplacementModifier*>(n->modifiers[static_cast<size_t>(ctx.stackIndex)].get())};
  };
  auto refresh = [resolve, mapBtn, clearBtn, form, doc] {
    auto [n, mod] = resolve();
    TexMap* map = mod ? mod->texmap.map.get() : nullptr;
    mapBtn->setText(mapButtonText(map));
    clearBtn->setEnabled(map != nullptr);
    QString file = doc->filePath();
    form->browseDir = file.isEmpty() ? QString() : QFileInfo(file).absolutePath();
    if (form->block() != (map ? &map->params : nullptr)) form->setBlock(map ? &map->params : nullptr);
    else form->refresh();
  };
  form->beforeChange = [doc, ctx](const std::string& key) {
    doc->softEdit(QObject::tr("Displacement Map"),
                  QString("dispmap/%1/%2").arg(ctx.nodeId).arg(QString::fromStdString(key)));
  };
  form->afterChange = [resolve, doc](const std::string&) {
    auto [n, mod] = resolve();
    if (n) doc->notifyNodeChanged(n, ChangeGeometry);
  };
  QObject::connect(mapBtn, &QPushButton::clicked, roll, [resolve, refresh, doc, roll] {
    auto [n, mod] = resolve();
    if (!mod) return;
    QString cur = mod->texmap.map ? QString::fromStdString(mod->texmap.map->type) : QString();
    QString type = browseMapType(roll, cur);
    if (type.isEmpty()) return;
    doc->beginEdit(QObject::tr("Displacement Map"));
    mod->texmap.map = createTexMap(type.toStdString());
    mod->texmap.enabled = true;
    doc->notifyNodeChanged(n, ChangeGeometry);
    doc->endEdit();
    refresh();
  });
  QObject::connect(clearBtn, &QPushButton::clicked, roll, [resolve, refresh, doc] {
    auto [n, mod] = resolve();
    if (!mod || !mod->texmap.map) return;
    doc->beginEdit(QObject::tr("Clear Displacement Map"));
    mod->texmap.map.reset();
    doc->notifyNodeChanged(n, ChangeGeometry);
    doc->endEdit();
    refresh();
  });
  QObject::connect(doc, &SceneDocument::structureChanged, roll, refresh);
  QObject::connect(doc, &SceneDocument::nodesChanged, roll, [refresh, ctx](const QVector<uint32_t>& ids, int) {
    if (ids.contains(ctx.nodeId)) refresh();
  });
  refresh();
  return roll;
}

// ---------------------------------------------------------------------------------------------
// Windows

QPointer<EnvironmentDialog> gEnvironment;
QPointer<BatchRenderDialog> gBatch;
QPointer<PrintSizeDialog> gPrintSize;
QPointer<SlateEditor> gSlate;

void showWindow(QWidget* w) {
  w->show();
  w->raise();
  w->activateWindow();
}

}  // namespace

EnvironmentDialog* environmentDialog(MainWindow& w) {
  if (!gEnvironment) gEnvironment = new EnvironmentDialog(w.document(), &w);
  return gEnvironment;
}

BatchRenderDialog* batchRenderDialog(MainWindow& w) {
  if (!gBatch) gBatch = new BatchRenderDialog(w.document(), &w);
  return gBatch;
}

PrintSizeDialog* printSizeDialog(MainWindow& w) {
  if (!gPrintSize) {
    gPrintSize = new PrintSizeDialog(w.document(), &w);
    QObject::connect(gPrintSize, &PrintSizeDialog::renderRequested, &w, &MainWindow::render);
  }
  return gPrintSize;
}

SlateEditor* slateEditor(MainWindow& w) {
  if (!gSlate) gSlate = new SlateEditor(w.document(), &w);
  return gSlate;
}

namespace {

void installRendering(MainWindow& w) {
  SceneDocument* doc = w.document();
  EditorState* state = w.editorState();
  auto action = [&w](const QString& id, const QString& text, const QKeySequence& key = {}) {
    auto* a = new QAction(text, &w);
    if (!key.isEmpty()) a->setShortcut(key);
    w.addAction(a);
    w.registerCommand(id, a);
    return a;
  };

  QAction* envAct = action("rendering.environment", QObject::tr("&Environment and Effects..."), QKeySequence("8"));
  QObject::connect(envAct, &QAction::triggered, &w, [&w] { showWindow(environmentDialog(w)); });
  QAction* sunSkyAct = action("rendering.sunAndSky", QObject::tr("Add Sun and Sky"));
  QObject::connect(sunSkyAct, &QAction::triggered, &w, [doc] {
    doc->beginEdit(QObject::tr("Add Sun and Sky"));
    Node* sun = addSunAndSky(doc);
    doc->endEdit();
    if (sun) doc->select(sun->id, false);
  });
  QAction* slateAct = action("rendering.slateEditor", QObject::tr("&Slate Material Editor..."));
  QObject::connect(slateAct, &QAction::triggered, &w, [&w, doc] {
    SlateEditor* s = slateEditor(w);
    uint32_t id = 0;
    if (Node* n = doc->firstSelected(); n && n->materialId) id = n->materialId;
    if (!id && w.materialEditor()) id = w.materialEditor()->currentMaterial();
    if (!id && !doc->scene().materials.empty()) id = doc->scene().materials.front()->id;
    s->showMaterial(id);
    showWindow(s);
  });
  QAction* batchAct = action("rendering.batchRender", QObject::tr("&Batch Render..."));
  QObject::connect(batchAct, &QAction::triggered, &w, [&w] { showWindow(batchRenderDialog(w)); });
  QAction* printAct = action("rendering.printSize", QObject::tr("&Print Size Assistant..."));
  QObject::connect(printAct, &QAction::triggered, &w, [&w] { showWindow(printSizeDialog(w)); });
  QAction* proxyAct = action("rendering.exportProxy", QObject::tr("Export &Proxy (.vxp)..."));
  QObject::connect(proxyAct, &QAction::triggered, &w, [&w, doc] {
    if (doc->selection().isEmpty()) {
      QMessageBox::information(&w, QObject::tr("Export Proxy"), QObject::tr("Select the geometry to export first."));
      return;
    }
    QString dir = doc->filePath().isEmpty() ? QString() : QFileInfo(doc->filePath()).absolutePath();
    QString path = QFileDialog::getSaveFileName(&w, QObject::tr("Export Proxy"), dir,
                                                QObject::tr("Veraa proxy (*.vxp)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(".vxp", Qt::CaseInsensitive)) path += ".vxp";
    auto answer = QMessageBox::question(&w, QObject::tr("Export Proxy"),
                                        QObject::tr("Replace the selected objects with the proxy?"),
                                        QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::Yes);
    if (answer == QMessageBox::Cancel) return;
    QString err;
    if (!exportProxy(doc, doc->selection(), path, answer == QMessageBox::Yes, &err))
      QMessageBox::warning(&w, QObject::tr("Export Proxy"), err);
  });

  QMenu* rendering = w.menuAt("Rendering");
  rendering->addSeparator();
  rendering->addAction(envAct);
  rendering->addAction(sunSkyAct);
  rendering->addSeparator();
  rendering->addAction(slateAct);
  rendering->addSeparator();
  rendering->addAction(batchAct);
  rendering->addAction(printAct);
  rendering->addAction(proxyAct);

  // Render Setup and Material Editor hooks.
  RenderSetupDialog* rs = w.renderSetup();
  QObject::connect(rs, &RenderSetupDialog::printSizeRequested, &w, [&w] { showWindow(printSizeDialog(w)); });
  QObject::connect(rs, &RenderSetupDialog::batchRenderRequested, &w, [&w] { showWindow(batchRenderDialog(w)); });
  QObject::connect(rs, &RenderSetupDialog::environmentRequested, &w, [&w] { showWindow(environmentDialog(w)); });
  QObject::connect(w.materialEditor(), &MaterialEditor::slateRequested, &w, [&w](uint32_t id) {
    SlateEditor* s = slateEditor(w);
    s->showMaterial(id);
    showWindow(s);
  });

  // Creation tools and gizmos.
  for (const char* type : {"VeraaLight", "VeraaIES", "VeraaSun", "SunPositioner", "VeraaPhysicalCamera",
                           "VeraaInfinitePlane", "VeraaProxy", "VeraaFur"}) {
    QString t = QString::fromLatin1(type);
    registerCreationTool(t, [t] { return std::make_shared<RenderCreateTool>(t); });
  }
  QObject::connect(state, &EditorState::createTypeChanged, &w, [state, doc](const QString& type) {
    // A creation tool belongs to one type; the next click makes a fresh one.
    if (dynamic_cast<RenderCreateTool*>(state->viewportTool())) state->setViewportTool(nullptr);
    // Fur grows on the selected objects right away.
    if (type == "VeraaFur") {
      std::vector<const Node*> sources;
      for (Node* n : doc->selectedNodes())
        if (n->geometry() && n->object->type() != "VeraaFur") sources.push_back(n);
      if (sources.empty()) {
        state->setPrompt(QObject::tr("Click a geometry object to grow fur on it"));
        return;
      }
      doc->beginEdit(QObject::tr("Create VeraaFur"));
      QVector<uint32_t> created;
      for (const Node* s : sources)
        if (Node* f = createFurOn(doc, s)) created.push_back(f->id);
      doc->endEdit();
      doc->setSelection(created);
      QTimer::singleShot(0, state, [state] {
        if (state->createType() == "VeraaFur") state->setCreateType({});
      });
      state->setPrompt(QObject::tr("Created fur on %1 object(s)").arg(created.size()));
    }
  });
  state->addDecorator(std::make_shared<RenderGizmos>());
  registerModifyRollouts("VeraaDisplacementMod", displacementRollout);
}

}  // namespace

VERAA_APP_EXTENSION(rendering, 300, installRendering);

}  // namespace veraa
