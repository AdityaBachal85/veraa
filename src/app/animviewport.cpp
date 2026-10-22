// Viewport display for animation: red border while Auto Key is on, trajectories, ghosting, and
// icons for particle emitters, space warps and IK chains.
#include <QAction>
#include <QMenu>
#include <QPainter>

#include <cmath>
#include <unordered_map>

#include "animcontroller.h"
#include "extension.h"
#include "glscene.h"
#include "mainwindow.h"
#include "particles.h"
#include "theme.h"
#include "viewport.h"
#include "viewportpanel.h"

namespace veraa {
namespace {

QColor qcol(const Vec3& c) {
  return QColor::fromRgbF(clampf(c.x, 0.f, 1.f), clampf(c.y, 0.f, 1.f), clampf(c.z, 0.f, 1.f));
}

class AnimDecorator : public ViewportDecorator {
 public:
  explicit AnimDecorator(AnimController* c) : c_(c) {
    QObject::connect(c, &AnimController::animationChanged, c, [this] { cache_.clear(); });
    QObject::connect(c, &AnimController::rangeChanged, c, [this] { cache_.clear(); });
    QObject::connect(c->document(), &SceneDocument::nodesChanged, c, [this](const QVector<uint32_t>&, int) {
      if (!c_->playing()) cache_.clear();
    });
    QObject::connect(c->document(), &SceneDocument::structureChanged, c, [this] { cache_.clear(); });
  }

  void drawOverlay(Viewport& vp, QPainter& p) override {
    if (!c_->autoKey()) return;
    p.save();
    p.setPen(QPen(QColor(0xd8, 0x20, 0x20), 4));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(2, 2, vp.width() - 4, vp.height() - 4));
    p.restore();
  }

  void drawLines(Viewport& vp, LineBatch& b) override {
    const Scene& s = c_->scene();
    Animation* anim = c_->animation();
    float px = 1.0f;
    for (const auto& up : s.nodes) {
      const Node& n = *up;
      if (!s.isNodeVisible(n) || !n.object) continue;
      bool sel = c_->document()->isSelected(n.id);
      QColor col = sel ? theme::selection() : qcol(n.wireColor);
      Mat4 w = n.worldMatrix();
      Vec3 o = w.translation();
      px = vp.camera().worldPerPixel(o);
      drawIcon(b, n, w, col, px);
      // Trajectories.
      bool traj = false;
      if (anim) {
        auto it = anim->nodes.find(n.id);
        traj = it != anim->nodes.end() && it->second.trajectory;
        if (sel && showTrajectoriesForSelection && anim->nodeAnimated(n.id)) traj = true;
      }
      if (traj) drawTrajectory(b, n, px);
      if (sel && c_->ghosting && anim && anim->nodeAnimated(n.id)) drawGhosts(b, n);
    }
  }

  static bool showTrajectoriesForSelection;

 private:
  struct Traj {
    std::vector<Vec3> points;  // one per frame from start
    std::vector<Vec3> keys;
  };

  const Traj& trajectory(const Node& n) {
    auto it = cache_.find(n.id);
    if (it != cache_.end()) return it->second;
    Traj t;
    Animation* anim = c_->animation();
    int start = c_->startFrame(), end = c_->endFrame();
    int step = std::max(1, (end - start) / 1000);
    for (int f = start; f <= end; f += step) t.points.push_back(anim->worldMatrixAt(c_->scene(), n.id, f).translation());
    std::vector<double> times;
    anim->keyTimesForNodes({n.id}, times);
    for (double k : times)
      if (k >= start && k <= end) t.keys.push_back(anim->worldMatrixAt(c_->scene(), n.id, k).translation());
    return cache_[n.id] = std::move(t);
  }

  void drawTrajectory(LineBatch& b, const Node& n, float px) {
    const Traj& t = trajectory(n);
    QColor red(0xe0, 0x30, 0x30), tick(0xf0, 0xf0, 0xf0);
    for (size_t i = 1; i < t.points.size(); ++i) b.line(t.points[i - 1], t.points[i], red);
    float s = px * 2.0f;
    for (const auto& p : t.points) b.line(p - Vec3(0, 0, s * 0.5f), p + Vec3(0, 0, s * 0.5f), tick);
    float k = px * 4.0f;
    for (const auto& p : t.keys) {
      b.line(p + Vec3(-k, -k, 0), p + Vec3(k, -k, 0), tick);
      b.line(p + Vec3(k, -k, 0), p + Vec3(k, k, 0), tick);
      b.line(p + Vec3(k, k, 0), p + Vec3(-k, k, 0), tick);
      b.line(p + Vec3(-k, k, 0), p + Vec3(-k, -k, 0), tick);
    }
  }

  void drawGhosts(LineBatch& b, const Node& n) {
    const TriMesh& tm = n.triMesh();
    if (tm.wireIndices.size() > 40000) return;
    Animation* anim = c_->animation();
    for (int g = 1; g <= c_->ghostFrames; ++g)
      for (int dir : {-1, 1}) {
        double f = c_->frame() + dir * g * c_->ghostStep;
        if (f < c_->startFrame() || f > c_->endFrame()) continue;
        Mat4 m = anim->worldMatrixAt(c_->scene(), n.id, f) * n.objectOffset.matrix();
        QColor col = dir < 0 ? QColor(0xe8, 0xd0, 0x40) : QColor(0x40, 0xd0, 0xe8);
        col.setAlphaF(std::max(0.2, 0.8 - 0.2 * g));
        for (size_t i = 0; i + 1 < tm.wireIndices.size(); i += 2)
          b.line(m.transformPoint(tm.wirePositions[tm.wireIndices[i]]), m.transformPoint(tm.wirePositions[tm.wireIndices[i + 1]]),
                 col);
      }
  }

  static void circle(LineBatch& b, const Vec3& c, const Vec3& u, const Vec3& v, float r, const QColor& col, int segs = 24) {
    Vec3 prev = c + u * r;
    for (int i = 1; i <= segs; ++i) {
      float a = kTwoPi * i / segs;
      Vec3 p = c + (u * std::cos(a) + v * std::sin(a)) * r;
      b.line(prev, p, col);
      prev = p;
    }
  }

  static void arrow(LineBatch& b, const Vec3& from, const Vec3& to, const Vec3& side, const QColor& col) {
    b.line(from, to, col);
    Vec3 d = to - from;
    b.line(to, to - d * 0.25f + side, col);
    b.line(to, to - d * 0.25f - side, col);
  }

  void drawIcon(LineBatch& b, const Node& n, const Mat4& w, const QColor& col, float px) {
    const std::string type = n.object->type();
    const ParamBlock& p = n.object->params;
    Vec3 o = w.translation();
    Mat4 m = w * n.objectOffset.matrix();
    Vec3 ax = normalize(m.column(0)), ay = normalize(m.column(1)), az = normalize(m.column(2));
    float sz = px * 40.0f;
    float icon = p.has("iconSize") ? std::max(p.getFloat("iconSize"), px * 10.0f) : sz;
    if (type == "Spray" || type == "Snow") {
      float hw = p.getFloat("width") * 0.5f, hl = p.getFloat("length") * 0.5f;
      Vec3 c[4] = {m.transformPoint({-hw, -hl, 0}), m.transformPoint({hw, -hl, 0}), m.transformPoint({hw, hl, 0}),
                   m.transformPoint({-hw, hl, 0})};
      for (int i = 0; i < 4; ++i) b.line(c[i], c[(i + 1) % 4], col);
      Vec3 center = m.transformPoint({0, 0, 0});
      arrow(b, center, center - az * std::max(hw, hl) * 0.6f, ax * std::max(hw, hl) * 0.08f, col);
    } else if (type == "SuperSpray") {
      circle(b, o, ax, ay, icon * 0.5f, col, 16);
      arrow(b, o, o + az * icon * 1.5f, ax * icon * 0.2f, col);
    } else if (type == "Gravity" || type == "Wind") {
      if (p.getInt("mode") == 1) {
        circle(b, o, ax, ay, icon, col);
        circle(b, o, ay, az, icon, col);
        circle(b, o, az, ax, icon, col);
      } else {
        Vec3 c[4] = {o + (ax + ay) * icon, o + (ay - ax) * icon, o - (ax + ay) * icon, o + (ax - ay) * icon};
        for (int i = 0; i < 4; ++i) b.line(c[i], c[(i + 1) % 4], col);
        arrow(b, o, o - az * icon * 1.6f, ax * icon * 0.25f, col);
        if (type == "Wind") arrow(b, o + ax * icon * 0.5f, o + ax * icon * 0.5f - az * icon * 1.2f, ax * icon * 0.18f, col);
      }
    } else if (type == "Drag" || type == "Motor" || type == "Push") {
      circle(b, o, ax, ay, icon, col);
      arrow(b, o, o + az * icon * 1.5f, ax * icon * 0.25f, col);
      if (type == "Motor") arrow(b, o + ax * icon, o + ax * icon + ay * icon * 0.8f, az * icon * 0.2f, col);
      if (type == "Drag") circle(b, o, ax, ay, icon * 0.6f, col);
    } else if (type == "Vortex") {
      Vec3 prev = o;
      for (int i = 1; i <= 64; ++i) {
        float t = i / 64.0f, a = t * kTwoPi * 3.0f, r = icon * (0.2f + t);
        Vec3 q = o + (ax * std::cos(a) + ay * std::sin(a)) * r + az * (t * icon * 2.0f);
        b.line(prev, q, col);
        prev = q;
      }
    } else if (type == "PBomb") {
      for (const Vec3& d : {ax, ay, az, -ax, -ay, -az}) b.line(o, o + d * icon, col);
      circle(b, o, ax, ay, icon * 0.4f, col, 12);
    } else if (type == "Deflector") {
      float hw = p.getFloat("width") * 0.5f, hl = p.getFloat("length") * 0.5f;
      Vec3 c[4] = {m.transformPoint({-hw, -hl, 0}), m.transformPoint({hw, -hl, 0}), m.transformPoint({hw, hl, 0}),
                   m.transformPoint({-hw, hl, 0})};
      for (int i = 0; i < 4; ++i) b.line(c[i], c[(i + 1) % 4], col);
      b.line(c[0], c[2], col);
      b.line(c[1], c[3], col);
    } else if (type == "SDeflector") {
      float r = p.getFloat("diameter") * 0.5f * length(m.column(0));
      circle(b, o, ax, ay, r, col);
      circle(b, o, ay, az, r, col);
      circle(b, o, az, ax, r, col);
    } else if (type == "UDeflector") {
      circle(b, o, ax, ay, icon, col);
      b.line(o - ax * icon, o + ax * icon, col);
      b.line(o - ay * icon, o + ay * icon, col);
    } else if (type == "Ripple") {
      int circles = std::min(p.getInt("circles"), 30), segs = std::max(8, p.getInt("segments"));
      float wl = p.getFloat("waveLength");
      Mat4 inv = m.inverse();
      auto* warp = dynamic_cast<const ForceWarpObject*>(n.object.get());
      for (int ci = 1; ci <= circles; ++ci) {
        float r = wl * 0.25f * ci;
        Vec3 prev;
        for (int i = 0; i <= segs; ++i) {
          float a = kTwoPi * i / segs;
          Vec3 q = m.transformPoint(Vec3(std::cos(a) * r, std::sin(a) * r, 0));
          if (warp) q = warp->deform(m, inv, q, 1.0f);
          if (i) b.line(prev, q, col);
          prev = q;
        }
      }
    } else if (type == "Wave") {
      int sides = std::min(p.getInt("sides"), 30), segs = std::min(p.getInt("segments"), 40) * 8;
      float half = p.getFloat("width") * 0.5f, wl = p.getFloat("waveLength");
      Mat4 inv = m.inverse();
      auto* warp = dynamic_cast<const ForceWarpObject*>(n.object.get());
      for (int si = 0; si <= sides; ++si) {
        float x = -half + 2.0f * half * si / std::max(1, sides);
        Vec3 prev;
        for (int i = 0; i <= segs; ++i) {
          float y = -wl + 2.0f * wl * i / segs;
          Vec3 q = m.transformPoint(Vec3(x, y, 0));
          if (warp) q = warp->deform(m, inv, q, 1.0f);
          if (i) b.line(prev, q, col);
          prev = q;
        }
      }
    } else if (type == "IKChain") {
      float g = std::max(p.getFloat("size"), px * 8.0f);
      b.line(o - ax * g, o + ax * g, col);
      b.line(o - ay * g, o + ay * g, col);
      b.line(o - az * g, o + az * g, col);
      if (const Node* start = c_->scene().findNode(static_cast<uint32_t>(p.getInt("startBone")))) {
        Vec3 s = start->worldPosition();
        QColor line = col;
        line.setAlphaF(0.7f);
        b.line(s, o, line);
        // Swivel plane handle.
        Vec3 mid = (s + o) * 0.5f;
        b.line(mid, mid + az * g * 2.0f, QColor(0xe0, 0xc0, 0x40));
      }
    }
  }

  AnimController* c_;
  std::unordered_map<uint32_t, Traj> cache_;
};

bool AnimDecorator::showTrajectoriesForSelection = false;

void installAnimViewport(MainWindow& w) {
  AnimController* c = AnimController::instance();
  if (!c) return;
  auto deco = std::make_shared<AnimDecorator>(c);
  w.editorState()->addDecorator(deco);
  auto redraw = [&w] { w.editorState()->requestRedraw(); };
  QObject::connect(c, &AnimController::autoKeyChanged, &w, redraw);
  QObject::connect(c, &AnimController::animationChanged, &w, redraw);

  QMenu* views = w.menuAt("Views");
  views->addSeparator();
  QAction* ghost = views->addAction(QObject::tr("Show Ghosting"));
  ghost->setCheckable(true);
  w.registerCommand("views.ghosting", ghost);
  QObject::connect(ghost, &QAction::toggled, &w, [c, &w](bool on) {
    c->ghosting = on;
    w.editorState()->requestRedraw();
  });
  QAction* traj = views->addAction(QObject::tr("Show Trajectories of Selection"));
  traj->setCheckable(true);
  w.registerCommand("views.trajectories", traj);
  QObject::connect(traj, &QAction::toggled, &w, [&w](bool on) {
    AnimDecorator::showTrajectoriesForSelection = on;
    w.editorState()->requestRedraw();
  });
}

}  // namespace

// Shared with the Motion panel's Trajectories button.
void setShowSelectionTrajectories(bool on) { AnimDecorator::showTrajectoriesForSelection = on; }
bool showSelectionTrajectories() { return AnimDecorator::showTrajectoriesForSelection; }

VERAA_APP_EXTENSION(animationViewport, 301, installAnimViewport);

}  // namespace veraa
