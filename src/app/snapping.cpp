// Snaps: 2D / 2.5D / 3D point snapping (grid points and lines, vertices, endpoints, edges,
// midpoints, faces, face centers, pivots, bounding boxes, perpendicular and tangent), angle
// snap, percent snap and spinner snap, with toolbar buttons, hotkeys, the Grid and Snap
// Settings dialog and an on-screen snap marker.
#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QGridLayout>
#include <QGuiApplication>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QPointer>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>
#include <functional>

#include "document.h"
#include "editorstate.h"
#include "mainwindow.h"
#include "polyedit.h"
#include "selftest.h"
#include "viewport.h"
#include "viewportpanel.h"

namespace veraa {

namespace {

enum SnapTarget : int {
  SnapGridPoints = 1 << 0,
  SnapGridLines = 1 << 1,
  SnapPivot = 1 << 2,
  SnapBBox = 1 << 3,
  SnapPerpendicular = 1 << 4,
  SnapTangent = 1 << 5,
  SnapVertex = 1 << 6,
  SnapEndpoint = 1 << 7,
  SnapEdge = 1 << 8,
  SnapMidpoint = 1 << 9,
  SnapFace = 1 << 10,
  SnapCenterFace = 1 << 11,
};

struct TargetInfo {
  int bit;
  const char* name;
};
const TargetInfo kTargets[] = {
    {SnapGridPoints, "Grid Points"}, {SnapGridLines, "Grid Lines"}, {SnapPivot, "Pivot"},
    {SnapBBox, "Bounding Box"},      {SnapPerpendicular, "Perpendicular"}, {SnapTangent, "Tangent"},
    {SnapVertex, "Vertex"},          {SnapEndpoint, "Endpoint"},  {SnapEdge, "Edge/Segment"},
    {SnapMidpoint, "Midpoint"},      {SnapFace, "Face"},          {SnapCenterFace, "Center Face"},
};

struct SnapSettings {
  bool enabled = false;
  int mode = 2;  // 0 = 2D, 1 = 2.5D, 2 = 3D
  int targets = SnapGridPoints | SnapVertex;
  float radius = 12.0f;  // pixels
  float gridSpacing = 10.0f;
  bool snapToFrozen = false;
  bool showMarker = true;
  bool spinnerSnap = false;
  float spinnerStep = 1.0f;

  void load() {
    QSettings s;
    s.beginGroup("snaps");
    mode = s.value("mode", mode).toInt();
    targets = s.value("targets", targets).toInt();
    radius = s.value("radius", radius).toFloat();
    gridSpacing = s.value("gridSpacing", gridSpacing).toFloat();
    snapToFrozen = s.value("snapToFrozen", snapToFrozen).toBool();
    showMarker = s.value("showMarker", showMarker).toBool();
    spinnerStep = s.value("spinnerStep", spinnerStep).toFloat();
  }
  void save() const {
    QSettings s;
    s.beginGroup("snaps");
    s.setValue("mode", mode);
    s.setValue("targets", targets);
    s.setValue("radius", radius);
    s.setValue("gridSpacing", gridSpacing);
    s.setValue("snapToFrozen", snapToFrozen);
    s.setValue("showMarker", showMarker);
    s.setValue("spinnerStep", spinnerStep);
  }
  static SnapSettings& get() {
    static SnapSettings s;
    return s;
  }
};

// The last successful snap (for the marker).
struct SnapHit {
  bool valid = false;
  Vec3 point;
  int target = 0;
  QElapsedTimer time;
  const Viewport* viewport = nullptr;
};
SnapHit& lastHit() {
  static SnapHit h;
  return h;
}

// Reference point for perpendicular / tangent snaps: where the current drag started.
struct SnapReference {
  bool valid = false;
  Vec3 point;
  bool buttonsDown = false;
};
SnapReference& reference() {
  static SnapReference r;
  return r;
}

const char* targetName(int bit) {
  for (const auto& t : kTargets)
    if (t.bit == bit) return t.name;
  return "";
}

struct Candidate {
  Vec3 world;
  float screenDist;
  int target;
  int priority;  // lower wins on ties
};

// Finds the best snap near pixel (px, py). `candidate` is the unsnapped world point.
bool findSnap(const Viewport& vpc, float px, float py, const Vec3& candidate, Vec3& out) {
  SnapSettings& ss = SnapSettings::get();
  if (!ss.enabled || ss.targets == 0) return false;
  auto& vp = const_cast<Viewport&>(vpc);
  const ViewCamera& cam = vp.camera();
  SceneDocument* doc = vp.document();
  EditorState* st = vp.editorState();
  const Scene& scene = doc->scene();
  // Reference point for perpendicular/tangent: the first candidate of a drag.
  bool down = QGuiApplication::mouseButtons() != Qt::NoButton;
  SnapReference& ref = reference();
  if (down && !ref.buttonsDown) {
    ref.point = candidate;
    ref.valid = true;
  }
  ref.buttonsDown = down;

  Vec3 planeP, planeN;
  cam.constructionPlane(planeP, planeN);
  Ray ray = cam.rayAt(px, py);
  float r = ss.radius;
  std::vector<Candidate> cands;
  auto consider = [&](const Vec3& w, int target, int priority) {
    float sx, sy;
    if (!cam.project(w, sx, sy)) return;
    float d = std::hypot(sx - px, sy - py);
    if (d <= r) cands.push_back({w, d, target, priority});
  };
  // In sub-object editing the edited object is a valid target; otherwise the moving or
  // just-created (selected) objects are skipped.
  bool subObject = activePolyTool(st) != nullptr;
  if (ss.mode != 0) {
    for (const auto& up : scene.nodes) {
      const Node& n = *up;
      if (!scene.isNodeVisible(n)) continue;
      if (!ss.snapToFrozen && scene.isNodeFrozen(n)) continue;
      if (!subObject && doc->isSelected(n.id)) continue;
      if (ss.targets & SnapPivot) consider(n.worldPosition(), SnapPivot, 2);
      ObjectKind kind = n.kind();
      if (kind != ObjectKind::Geometry && kind != ObjectKind::Shape) continue;
      Aabb wb = n.worldBounds();
      if (!wb.valid()) continue;
      if (ss.targets & SnapBBox)
        for (int i = 0; i < 8; ++i)
          consider(Vec3((i & 1) ? wb.hi.x : wb.lo.x, (i & 2) ? wb.hi.y : wb.lo.y, (i & 4) ? wb.hi.z : wb.lo.z), SnapBBox, 3);
      // Skip objects whose screen bounds are far from the cursor.
      float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
      bool anyVisible = false;
      for (int i = 0; i < 8; ++i) {
        float sx, sy;
        Vec3 c((i & 1) ? wb.hi.x : wb.lo.x, (i & 2) ? wb.hi.y : wb.lo.y, (i & 4) ? wb.hi.z : wb.lo.z);
        if (!cam.project(c, sx, sy)) continue;
        anyVisible = true;
        minX = std::min(minX, sx);
        maxX = std::max(maxX, sx);
        minY = std::min(minY, sy);
        maxY = std::max(maxY, sy);
      }
      if (!anyVisible || px < minX - r || px > maxX + r || py < minY - r || py > maxY + r) continue;
      Mat4 m = n.objectWorldMatrix();
      const TriMesh& tm = n.triMesh();
      if (ss.targets & (SnapVertex | SnapEndpoint))
        for (const auto& p : tm.wirePositions) consider(m.transformPoint(p), SnapVertex, 0);
      // Shapes: knots are endpoints/vertices too.
      for (const auto& line : n.shapeLines()) {
        if (line.empty()) continue;
        if (ss.targets & SnapEndpoint) {
          consider(m.transformPoint(line.front()), SnapEndpoint, 0);
          consider(m.transformPoint(line.back()), SnapEndpoint, 0);
        }
        if (ss.targets & SnapVertex)
          for (const auto& p : line) consider(m.transformPoint(p), SnapVertex, 0);
      }
      auto segments = [&](const std::function<void(const Vec3&, const Vec3&)>& fn) {
        for (size_t i = 0; i + 1 < tm.wireIndices.size(); i += 2)
          fn(m.transformPoint(tm.wirePositions[tm.wireIndices[i]]), m.transformPoint(tm.wirePositions[tm.wireIndices[i + 1]]));
        for (const auto& line : n.shapeLines())
          for (size_t i = 1; i < line.size(); ++i) fn(m.transformPoint(line[i - 1]), m.transformPoint(line[i]));
      };
      if (ss.targets & SnapMidpoint) segments([&](const Vec3& a, const Vec3& b) { consider((a + b) * 0.5f, SnapMidpoint, 1); });
      if (ss.targets & SnapEdge)
        segments([&](const Vec3& a, const Vec3& b) {
          float ax, ay, bx, by;
          if (!cam.project(a, ax, ay) || !cam.project(b, bx, by)) return;
          float dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
          float t = len2 > 1e-9f ? clampf(((px - ax) * dx + (py - ay) * dy) / len2, 0.0f, 1.0f) : 0.0f;
          consider(lerp(a, b, t), SnapEdge, 4);
        });
      if ((ss.targets & SnapPerpendicular) && ref.valid)
        segments([&](const Vec3& a, const Vec3& b) {
          Vec3 d = b - a;
          float len2 = length2(d);
          if (len2 < 1e-12f) return;
          float t = dot(ref.point - a, d) / len2;
          if (t < 0.0f || t > 1.0f) return;
          consider(a + d * t, SnapPerpendicular, 1);
        });
      if ((ss.targets & SnapTangent) && ref.valid)
        for (const auto& line : n.shapeLines())
          for (size_t i = 1; i + 1 < line.size(); ++i) {
            Vec3 p = m.transformPoint(line[i]);
            Vec3 tdir = normalize(m.transformPoint(line[i + 1]) - m.transformPoint(line[i - 1]));
            Vec3 toP = p - ref.point;
            if (length2(toP) < 1e-9f) continue;
            if (std::fabs(dot(normalize(toP), tdir)) > 0.995f) consider(p, SnapTangent, 1);
          }
      if (ss.targets & (SnapFace | SnapCenterFace)) {
        Mat4 inv = m.inverse();
        Ray local{inv.transformPoint(ray.o), inv.transformDir(ray.d)};
        if (ss.targets & SnapFace) {
          float t = intersectTriMesh(tm, local);
          if (t > 0) cands.push_back({m.transformPoint(local.o + local.d * t), r, SnapFace, 6});
        }
        if (ss.targets & SnapCenterFace) {
          const PolyMesh& pm = n.mesh();
          for (const auto& f : pm.faces) {
            Vec3 c;
            for (int v : f.v) c += pm.positions[v];
            consider(m.transformPoint(c / static_cast<float>(f.v.size())), SnapCenterFace, 2);
          }
        }
      }
    }
  }
  // Grid snaps on the construction plane.
  float denom = dot(ray.d, planeN);
  Vec3 onPlane = candidate;
  bool planeHit = false;
  if (std::fabs(denom) > 1e-6f) {
    float t = dot(planeP - ray.o, planeN) / denom;
    if (cam.orthographic() || t > 0) {
      onPlane = ray.o + ray.d * t;
      planeHit = true;
    }
  }
  if (planeHit && (ss.targets & (SnapGridPoints | SnapGridLines))) {
    float g = std::max(ss.gridSpacing, 1e-4f);
    Vec3 q = onPlane;
    Vec3 snappedPt;
    for (int k = 0; k < 3; ++k) snappedPt[k] = std::fabs(planeN[k]) > 0.5f ? q[k] : std::round(q[k] / g) * g;
    if (ss.targets & SnapGridPoints) consider(snappedPt, SnapGridPoints, 5);
    if (ss.targets & SnapGridLines)
      for (int k = 0; k < 3; ++k) {
        if (std::fabs(planeN[k]) > 0.5f) continue;
        Vec3 l = q;
        l[k] = snappedPt[k];
        consider(l, SnapGridLines, 6);
      }
  }
  if (cands.empty()) return false;
  const Candidate* best = &cands[0];
  for (const auto& c : cands)
    if (c.priority < best->priority || (c.priority == best->priority && c.screenDist < best->screenDist)) best = &c;
  Vec3 result = best->world;
  // 2.5D: project the 3D snap onto the construction plane.
  if (ss.mode == 1 && best->target != SnapGridPoints && best->target != SnapGridLines)
    result = result - planeN * dot(result - planeP, planeN);
  out = result;
  SnapHit& h = lastHit();
  h.valid = true;
  h.point = result;
  h.target = best->target;
  h.time.start();
  h.viewport = &vpc;
  return true;
}

class SnapMarker : public ViewportDecorator {
 public:
  void drawOverlay(Viewport& vp, QPainter& p) override {
    SnapHit& h = lastHit();
    if (!h.valid || !SnapSettings::get().showMarker || !SnapSettings::get().enabled) return;
    if (h.viewport != &vp || h.time.elapsed() > 1500) return;
    float x, y;
    if (!vp.camera().project(h.point, x, y)) return;
    QPointF c(x, y);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(60, 220, 255), 2));
    p.setBrush(Qt::NoBrush);
    switch (h.target) {
      case SnapVertex:
      case SnapEndpoint: p.drawRect(QRectF(c.x() - 6, c.y() - 6, 12, 12)); break;
      case SnapMidpoint: p.drawPolygon(QPolygonF({c + QPointF(0, -7), c + QPointF(7, 5), c + QPointF(-7, 5)})); break;
      case SnapEdge: p.drawLine(c + QPointF(-7, 7), c + QPointF(7, -7)); break;
      case SnapFace:
      case SnapCenterFace: p.drawPolygon(QPolygonF({c + QPointF(0, -7), c + QPointF(7, 0), c + QPointF(0, 7), c + QPointF(-7, 0)})); break;
      default: p.drawEllipse(c, 6, 6); break;
    }
    p.drawLine(c + QPointF(-2, 0), c + QPointF(2, 0));
    p.drawLine(c + QPointF(0, -2), c + QPointF(0, 2));
    p.setPen(QColor(60, 220, 255));
    p.drawText(c + QPointF(10, -8), QObject::tr(targetName(h.target)));
    p.restore();
  }
};

// ---- Toolbar icons ----

QIcon snapIcon(int mode) {
  QPixmap pm(24, 24);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(QPen(QColor(220, 220, 220), 2.2));
  QPainterPath magnet;
  magnet.moveTo(5, 4);
  magnet.lineTo(5, 13);
  magnet.arcTo(QRectF(5, 7, 14, 13), 180, 180);
  magnet.lineTo(19, 4);
  p.drawPath(magnet);
  p.setPen(QPen(QColor(230, 80, 70), 2.2));
  p.drawLine(QPointF(5, 4), QPointF(5, 7));
  p.drawLine(QPointF(19, 4), QPointF(19, 7));
  QFont f = p.font();
  f.setPixelSize(9);
  f.setBold(true);
  p.setFont(f);
  p.setPen(QColor(255, 210, 90));
  p.drawText(QRectF(0, 12, 24, 12), Qt::AlignRight | Qt::AlignBottom, mode == 0 ? "2" : mode == 1 ? "2.5" : "3");
  return QIcon(pm);
}

QIcon textIcon(const QString& text, bool arc = false) {
  QPixmap pm(24, 24);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(QPen(QColor(220, 220, 220), 1.8));
  if (arc) {
    p.drawLine(QPointF(4, 20), QPointF(20, 20));
    p.drawLine(QPointF(4, 20), QPointF(17, 6));
    p.drawArc(QRectF(-4, 12, 16, 16), 0, 45 * 16);
  } else {
    QFont f = p.font();
    f.setPixelSize(15);
    f.setBold(true);
    p.setFont(f);
    p.drawText(QRectF(0, 0, 24, 24), Qt::AlignCenter, text);
  }
  return QIcon(pm);
}

// ---- Spinner snap ----

class SpinnerSnapFilter : public QObject {
 public:
  using QObject::QObject;
  bool eventFilter(QObject* o, QEvent* e) override {
    if (e->type() == QEvent::Show)
      if (auto* s = qobject_cast<QDoubleSpinBox*>(o)) apply(s);
    return false;
  }
  static void apply(QDoubleSpinBox* s) {
    SnapSettings& ss = SnapSettings::get();
    if (!s->property("veraaBaseStep").isValid()) s->setProperty("veraaBaseStep", s->singleStep());
    s->setSingleStep(ss.spinnerSnap ? ss.spinnerStep : s->property("veraaBaseStep").toDouble());
  }
  static void applyAll() {
    for (QWidget* w : QApplication::allWidgets())
      if (auto* s = qobject_cast<QDoubleSpinBox*>(w)) apply(s);
  }
};

// ---- Settings dialog ----

class SnapSettingsDialog : public QDialog {
 public:
  SnapSettingsDialog(EditorState* st, QWidget* parent) : QDialog(parent), st_(st) {
    setWindowTitle(tr("Grid and Snap Settings"));
    setObjectName("SnapSettingsDialog");
    SnapSettings& ss = SnapSettings::get();
    auto* v = new QVBoxLayout(this);
    auto* tabs = new QTabWidget(this);
    v->addWidget(tabs);
    auto* snaps = new QWidget(tabs);
    auto* g = new QGridLayout(snaps);
    int i = 0;
    for (const auto& t : kTargets) {
      auto* c = new QCheckBox(tr(t.name), snaps);
      c->setObjectName(QString("snap_%1").arg(t.bit));
      c->setChecked(ss.targets & t.bit);
      int bit = t.bit;
      connect(c, &QCheckBox::toggled, this, [bit](bool on) {
        SnapSettings& s = SnapSettings::get();
        s.targets = on ? (s.targets | bit) : (s.targets & ~bit);
        s.save();
      });
      g->addWidget(c, i / 2, i % 2);
      ++i;
    }
    auto* clear = new QPushButton(tr("Clear All"), snaps);
    connect(clear, &QPushButton::clicked, this, [snaps] {
      for (QCheckBox* c : snaps->findChildren<QCheckBox*>()) c->setChecked(false);
    });
    g->addWidget(clear, i / 2 + 1, 0);
    tabs->addTab(snaps, tr("Snaps"));

    auto* opts = new QWidget(tabs);
    auto* f = new QFormLayout(opts);
    auto* mode = new QComboBox(opts);
    mode->addItems({tr("2D"), tr("2.5D"), tr("3D")});
    mode->setCurrentIndex(ss.mode);
    connect(mode, &QComboBox::currentIndexChanged, this, [](int m) {
      SnapSettings::get().mode = m;
      SnapSettings::get().save();
    });
    f->addRow(tr("Snap mode:"), mode);
    auto* marker = new QCheckBox(tr("Display marker"), opts);
    marker->setChecked(ss.showMarker);
    connect(marker, &QCheckBox::toggled, this, [](bool on) {
      SnapSettings::get().showMarker = on;
      SnapSettings::get().save();
    });
    f->addRow(marker);
    auto* radius = new QSpinBox(opts);
    radius->setRange(2, 100);
    radius->setValue(static_cast<int>(ss.radius));
    connect(radius, &QSpinBox::valueChanged, this, [](int r) {
      SnapSettings::get().radius = static_cast<float>(r);
      SnapSettings::get().save();
    });
    f->addRow(tr("Snap strength (pixels):"), radius);
    auto* angle = new QDoubleSpinBox(opts);
    angle->setRange(0.01, 360);
    angle->setValue(st->angleSnapStep());
    connect(angle, &QDoubleSpinBox::valueChanged, this, [this](double a) { st_->setAngleSnap(st_->angleSnap(), static_cast<float>(a)); });
    f->addRow(tr("Angle (deg):"), angle);
    auto* percent = new QDoubleSpinBox(opts);
    percent->setRange(0.01, 1000);
    percent->setValue(st->percentSnapStep());
    connect(percent, &QDoubleSpinBox::valueChanged, this,
            [this](double p) { st_->setPercentSnap(st_->percentSnap(), static_cast<float>(p)); });
    f->addRow(tr("Percent:"), percent);
    auto* spin = new QDoubleSpinBox(opts);
    spin->setRange(0.001, 1e6);
    spin->setDecimals(3);
    spin->setValue(ss.spinnerStep);
    connect(spin, &QDoubleSpinBox::valueChanged, this, [](double s) {
      SnapSettings::get().spinnerStep = static_cast<float>(s);
      SnapSettings::get().save();
      SpinnerSnapFilter::applyAll();
    });
    f->addRow(tr("Spinner snap:"), spin);
    auto* frozen = new QCheckBox(tr("Snap to frozen objects"), opts);
    frozen->setChecked(ss.snapToFrozen);
    connect(frozen, &QCheckBox::toggled, this, [](bool on) {
      SnapSettings::get().snapToFrozen = on;
      SnapSettings::get().save();
    });
    f->addRow(frozen);
    tabs->addTab(opts, tr("Options"));

    auto* grid = new QWidget(tabs);
    auto* gf = new QFormLayout(grid);
    auto* spacing = new QDoubleSpinBox(grid);
    spacing->setRange(0.001, 1e6);
    spacing->setDecimals(3);
    spacing->setValue(ss.gridSpacing);
    connect(spacing, &QDoubleSpinBox::valueChanged, this, [](double s) {
      SnapSettings::get().gridSpacing = static_cast<float>(s);
      SnapSettings::get().save();
    });
    gf->addRow(tr("Grid spacing:"), spacing);
    tabs->addTab(grid, tr("Home Grid"));
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::accept);
    v->addWidget(bb);
  }

 private:
  EditorState* st_;
};

void showSettings(MainWindow& w) {
  static QPointer<SnapSettingsDialog> dlg;
  if (!dlg) dlg = new SnapSettingsDialog(w.editorState(), &w);
  dlg->show();
  dlg->raise();
}

void installSnapping(MainWindow& w) {
  SnapSettings::get().load();
  EditorState* st = w.editorState();
  st->setPointSnapper([](const Viewport& vp, float px, float py, const Vec3& c, Vec3& out) { return findSnap(vp, px, py, c, out); });
  st->addDecorator(std::make_shared<SnapMarker>());
  qApp->installEventFilter(new SpinnerSnapFilter(&w));

  // The main window places these on the main toolbar; "contextCommand" makes a right-click on
  // their buttons open the settings and "toolMenu" gives the snap button its 2D / 2.5D / 3D flyout.
  auto make = [&](const QString& id, const QString& text, const QKeySequence& key, const QIcon& icon) {
    auto* a = new QAction(icon, text, &w);
    a->setCheckable(true);
    if (!key.isEmpty()) a->setShortcut(key);
    a->setToolTip(key.isEmpty() ? QString("%1 - right-click for settings").arg(text)
                                : QString("%1 (%2) - right-click for settings").arg(text, key.toString()));
    a->setProperty("contextCommand", "snaps.settings");
    w.addAction(a);
    w.registerCommand(id, a);
    return a;
  };
  SnapSettings& ss = SnapSettings::get();
  QAction* snap = make("snaps.toggle", QObject::tr("Snaps Toggle"), QKeySequence("S"), snapIcon(ss.mode));
  QAction* angle = make("snaps.angle", QObject::tr("Angle Snap Toggle"), QKeySequence("A"), textIcon("", true));
  QAction* percent = make("snaps.percent", QObject::tr("Percent Snap Toggle"), QKeySequence("Shift+Ctrl+P"), textIcon("%"));
  QAction* spinner = make("snaps.spinner", QObject::tr("Spinner Snap Toggle"), QKeySequence(), textIcon(QString::fromUtf8("⇕")));
  // The snap button offers 2D / 2.5D / 3D from a menu.
  {
    auto* menu = new QMenu(&w);
    for (int m = 0; m < 3; ++m) {
      QAction* ma = menu->addAction(snapIcon(m), m == 0 ? QObject::tr("2D Snap") : m == 1 ? QObject::tr("2.5D Snap") : QObject::tr("3D Snap"));
      QObject::connect(ma, &QAction::triggered, &w, [snap, m] {
        SnapSettings::get().mode = m;
        SnapSettings::get().save();
        snap->setIcon(snapIcon(m));
        if (!snap->isChecked()) snap->trigger();
      });
    }
    snap->setProperty("toolMenu", QVariant::fromValue<QObject*>(menu));
  }
  QObject::connect(snap, &QAction::toggled, &w, [&w](bool on) {
    SnapSettings::get().enabled = on;
    w.editorState()->setPrompt(on ? QObject::tr("Snaps on") : QObject::tr("Snaps off"));
    w.editorState()->requestRedraw();
  });
  QObject::connect(angle, &QAction::toggled, &w, [st](bool on) { st->setAngleSnap(on, st->angleSnapStep()); });
  QObject::connect(percent, &QAction::toggled, &w, [st](bool on) { st->setPercentSnap(on, st->percentSnapStep()); });
  QObject::connect(spinner, &QAction::toggled, &w, [](bool on) {
    SnapSettings::get().spinnerSnap = on;
    SpinnerSnapFilter::applyAll();
  });
  auto* settings = new QAction(QObject::tr("Grid and Snap Settings..."), &w);
  w.registerCommand("snaps.settings", settings);
  QObject::connect(settings, &QAction::triggered, &w, [&w] { showSettings(w); });
  QMenu* tools = w.menuAt("Tools");
  tools->addSeparator();
  tools->addAction(snap);
  tools->addAction(angle);
  tools->addAction(percent);
  tools->addAction(spinner);
  tools->addAction(settings);
}

// ---- Self test: snapping a moved object to another object's vertex ----

void testSnapping(MainWindow& w) {
  using testing::check;
  using testing::pump;
  SceneDocument* doc = w.document();
  EditorState* st = w.editorState();
  Viewport* top = w.viewports()->viewport(0);
  w.viewports()->setActive(top);
  doc->beginEdit("Create");
  Node* a = doc->createNode("Box");
  Node* b = doc->createNode("Box");
  b->object->params.setNumber("width", 40);
  b->object->params.setNumber("length", 40);
  b->transform.position = Vec3(200, 0, 0);
  doc->endEdit();
  uint32_t ida = a->id, idb = b->id;
  top->zoomExtents(false);
  top->camera().zoom(1.3f, top->width() * 0.5f, top->height() * 0.5f);
  pump(30);
  SnapSettings& ss = SnapSettings::get();
  int savedTargets = ss.targets, savedMode = ss.mode;
  ss.targets = SnapVertex;
  ss.mode = 2;
  QAction* snap = w.command("snaps.toggle");
  check(snap != nullptr, "snap toggle command registered");
  if (!snap) return;
  if (!snap->isChecked()) snap->trigger();
  check(ss.enabled, "S turns snaps on");
  st->setTool(TransformTool::Move);
  doc->setSelection({idb});
  pump(30);
  // Drag box B by its body and release next to box A's corner (50, -50).
  QPointF from = testing::projectPoint(top, Vec3(200, 0, 50));
  QPointF to = testing::projectPoint(top, Vec3(50, -50, 0)) + QPointF(4, -3);
  top->simulateMouse(QEvent::MouseMove, from, Qt::NoButton, Qt::NoButton);
  top->simulateMouse(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
  for (int i = 1; i <= 10; ++i) top->simulateMouse(QEvent::MouseMove, from + (to - from) * (i / 10.0), Qt::NoButton, Qt::LeftButton);
  top->simulateMouse(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
  pump(30);
  Node* nb = doc->scene().findNode(idb);
  Vec3 pos = nb ? nb->worldPosition() : Vec3();
  check(std::fabs(pos.x - 50.0f) < 1e-3f && std::fabs(pos.y + 50.0f) < 1e-3f, "moved object snaps exactly onto the vertex");
  if (QString dir = qEnvironmentVariable("VERAA_SCREENSHOT_DIR"); !dir.isEmpty()) testing::saveGrab(&w, dir + "/snap_01_vertex.png");
  // Grid snap: create-drag a box with grid points only.
  ss.targets = SnapGridPoints;
  Vec3 out;
  bool ok = findSnap(*top, static_cast<float>(testing::projectPoint(top, Vec3(23.4f, 17.2f, 0)).x()),
                     static_cast<float>(testing::projectPoint(top, Vec3(23.4f, 17.2f, 0)).y()), Vec3(), out);
  check(ok && std::fabs(out.x - 20.0f) < 1e-3f && std::fabs(out.y - 20.0f) < 1e-3f, "grid point snap rounds to the grid");
  // Angle snap rounds rotations.
  QAction* angleAct = w.command("snaps.angle");
  if (angleAct && !angleAct->isChecked()) angleAct->trigger();
  check(st->angleSnap() && std::fabs(degrees(st->snapAngle(radians(12.6f))) - 15.0f) < 1e-3f, "angle snap uses 5 degree steps");
  if (angleAct) angleAct->trigger();
  QAction* pct = w.command("snaps.percent");
  if (pct && !pct->isChecked()) pct->trigger();
  check(st->percentSnap() && std::fabs(st->snapPercent(Vec3(1.13f)).x - 1.1f) < 1e-4f, "percent snap uses 10% steps");
  if (pct) pct->trigger();
  snap->trigger();
  ss.targets = savedTargets;
  ss.mode = savedMode;
  st->setTool(TransformTool::Select);
  (void)ida;
}

}  // namespace

VERAA_APP_EXTENSION(snapping, 150, installSnapping);
VERAA_SELF_TEST(snapping, testSnapping);

}  // namespace veraa
