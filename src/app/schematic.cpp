// Schematic View: the scene as a graph of boxes with hierarchy links and animation relations
// (constraints, IK chains, wires, skin bones, bound space warps).
#include <QAction>
#include <QActionGroup>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <cmath>
#include <map>
#include <set>

#include "animcontroller.h"
#include "animicons.h"
#include "animui.h"
#include "extension.h"
#include "mainwindow.h"
#include "rigging.h"
#include "theme.h"

namespace veraa {
namespace {

const double kBoxW = 130, kBoxH = 34, kGapX = 24, kGapY = 46;

QColor kindColor(ObjectKind k) {
  switch (k) {
    case ObjectKind::Geometry: return QColor(0x3c, 0x6e, 0xa8);
    case ObjectKind::Shape: return QColor(0x8a, 0x5a, 0xa8);
    case ObjectKind::Light: return QColor(0xb0, 0x90, 0x30);
    case ObjectKind::Camera: return QColor(0x40, 0x90, 0x90);
    case ObjectKind::Helper: return QColor(0x3c, 0x8a, 0x50);
    case ObjectKind::SpaceWarp: return QColor(0x70, 0x70, 0xb0);
  }
  return QColor(0x60, 0x60, 0x60);
}

struct Relation {
  uint32_t from, to;
  QColor color;
  QString label;
};

class SchematicCanvas : public QWidget {
 public:
  explicit SchematicCanvas(AnimController* c) : c_(c) {
    setObjectName("schematicCanvas");
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setMinimumSize(400, 300);
  }

  bool connectMode = false;
  bool showRelations = true;
  bool showModifiers = true;

  void relayout() {
    pos_.clear();
    const Scene& s = c_->scene();
    double x = 0;
    std::function<double(const Node*, int)> place = [&](const Node* n, int depth) -> double {
      std::vector<Node*> kids = s.children(n->id);
      double startX = x;
      if (kids.empty()) {
        pos_[n->id] = QPointF(x, depth * (kBoxH + kGapY));
        x += kBoxW + kGapX;
        return pos_[n->id].x();
      }
      double first = 0, last = 0;
      for (size_t i = 0; i < kids.size(); ++i) {
        double cx = place(kids[i], depth + 1);
        if (i == 0) first = cx;
        last = cx;
      }
      double cx = (first + last) * 0.5;
      pos_[n->id] = QPointF(std::max(startX, cx), depth * (kBoxH + kGapY));
      return pos_[n->id].x();
    };
    for (const auto& up : s.nodes)
      if (!up->parentId) place(up.get(), 0);
    zoomExtents();
  }

  void zoomExtents() {
    if (pos_.empty()) {
      update();
      return;
    }
    QRectF b;
    for (const auto& kv : pos_) b |= QRectF(kv.second, QSizeF(kBoxW, kBoxH + modExtra(kv.first)));
    b.adjust(-20, -20, 20, 20);
    zoom_ = std::min(width() / b.width(), height() / b.height());
    zoom_ = std::clamp(zoom_, 0.15, 2.0);
    pan_ = QPointF(width() / 2.0, height() / 2.0) - b.center() * zoom_;
    update();
  }

  void sync() {
    const Scene& s = c_->scene();
    bool missing = false;
    for (const auto& up : s.nodes)
      if (!pos_.count(up->id)) missing = true;
    for (auto it = pos_.begin(); it != pos_.end();)
      it = s.findNode(it->first) ? std::next(it) : pos_.erase(it);
    if (missing) relayout();
    update();
  }

 protected:
  double modExtra(uint32_t id) const {
    if (!showModifiers) return 0;
    const Node* n = c_->scene().findNode(id);
    return n ? n->modifiers.size() * 14.0 : 0;
  }

  QRectF boxRect(uint32_t id) const {
    auto it = pos_.find(id);
    if (it == pos_.end()) return QRectF();
    return QRectF(it->second * zoom_ + pan_, QSizeF(kBoxW * zoom_, (kBoxH + modExtra(id)) * zoom_));
  }

  uint32_t boxAt(const QPointF& p) const {
    for (const auto& kv : pos_)
      if (boxRect(kv.first).contains(p)) return kv.first;
    return 0;
  }

  std::vector<Relation> relations() const {
    std::vector<Relation> out;
    const Scene& s = c_->scene();
    Animation* anim = c_->animation();
    if (anim) {
      for (const auto& [id, na] : anim->nodes)
        for (const auto& con : na.constraints)
          for (const auto& t : con.targets)
            if (t.nodeId) out.push_back({t.nodeId, id, QColor(0xf0, 0xa0, 0x40), constraintTitle(con.type)});
      for (const auto& w : anim->wires) {
        ValuePath a = ValuePath::parse(w.from), b = ValuePath::parse(w.to);
        if (a.id && b.id && a.kind != ValuePath::Kind::MaterialParam && b.kind != ValuePath::Kind::MaterialParam)
          out.push_back({a.id, b.id, QColor(0xc0, 0x70, 0xe0), QObject::tr("Wire")});
      }
      for (const auto& r : anim->reactions) {
        ValuePath a = ValuePath::parse(r.driver), b = ValuePath::parse(r.driven);
        if (a.id && b.id) out.push_back({a.id, b.id, QColor(0x50, 0xc0, 0xc0), QObject::tr("Reaction")});
      }
    }
    for (const auto& up : s.nodes) {
      if (up->object && up->object->type() == "IKChain")
        out.push_back({up->id, static_cast<uint32_t>(up->object->params.getInt("endBone")), QColor(0x60, 0xd0, 0x60), QObject::tr("IK")});
      for (const auto& m : up->modifiers)
        if (m)
          for (uint32_t ref : m->referencedNodes()) out.push_back({ref, up->id, QColor(0x9a, 0x9a, 0x9a), QString::fromStdString(m->type())});
    }
    return out;
  }

  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor(0x2e, 0x2e, 0x2e));
    const Scene& s = c_->scene();
    // Hierarchy links.
    p.setPen(QPen(QColor(0x70, 0x90, 0xc0), 1.5));
    for (const auto& up : s.nodes) {
      if (!up->parentId) continue;
      QRectF a = boxRect(up->parentId), b = boxRect(up->id);
      if (a.isNull() || b.isNull()) continue;
      QPointF from(a.center().x(), a.bottom()), to(b.center().x(), b.top());
      QPainterPath path(from);
      path.cubicTo(from + QPointF(0, 20 * zoom_), to - QPointF(0, 20 * zoom_), to);
      p.drawPath(path);
    }
    if (showRelations)
      for (const auto& r : relations()) {
        QRectF a = boxRect(r.from), b = boxRect(r.to);
        if (a.isNull() || b.isNull()) continue;
        p.setPen(QPen(r.color, 1.3, Qt::DashLine));
        QPointF from = a.center(), to = b.center();
        p.drawLine(from, to);
        QPointF d = to - from;
        double len = std::hypot(d.x(), d.y());
        if (len > 1) {
          QPointF u = d / len, n(-u.y(), u.x());
          QPointF tip = to - u * (b.height() * 0.5);
          p.setBrush(r.color);
          p.setPen(Qt::NoPen);
          QPolygonF tri;
          tri << tip << tip - u * 9 + n * 4 << tip - u * 9 - n * 4;
          p.drawPolygon(tri);
        }
      }
    // Boxes.
    QFont f = font();
    f.setPointSizeF(std::max(5.0, 8.5 * zoom_));
    p.setFont(f);
    Animation* anim = c_->animation();
    for (const auto& up : s.nodes) {
      QRectF r = boxRect(up->id);
      if (r.isNull()) continue;
      bool sel = c_->document()->isSelected(up->id);
      QRectF head(r.topLeft(), QSizeF(r.width(), kBoxH * zoom_));
      p.setPen(sel ? QPen(Qt::white, 2) : QPen(QColor(0x18, 0x18, 0x18), 1));
      p.setBrush(kindColor(up->kind()));
      p.drawRoundedRect(head, 4 * zoom_, 4 * zoom_);
      p.setPen(Qt::white);
      p.drawText(head.adjusted(6 * zoom_, 2 * zoom_, -6 * zoom_, -head.height() / 2), Qt::AlignLeft | Qt::AlignVCenter,
                 QString::fromStdString(up->name));
      p.setPen(QColor(0xd8, 0xd8, 0xd8));
      QString sub = up->object ? QString::fromStdString(objectDisplayName(up->object->type())) : QString();
      if (anim && anim->nodeAnimated(up->id)) sub += QStringLiteral("  ◆");
      p.drawText(head.adjusted(6 * zoom_, head.height() / 2, -6 * zoom_, -2 * zoom_), Qt::AlignLeft | Qt::AlignVCenter, sub);
      if (showModifiers)
        for (size_t i = 0; i < up->modifiers.size(); ++i) {
          if (!up->modifiers[i]) continue;
          QRectF mr(r.left() + 8 * zoom_, head.bottom() + i * 14 * zoom_, r.width() - 8 * zoom_, 13 * zoom_);
          p.setPen(QColor(0x18, 0x18, 0x18));
          p.setBrush(QColor(0x58, 0x58, 0x5e));
          p.drawRect(mr);
          p.setPen(QColor(0xe0, 0xe0, 0xe0));
          p.drawText(mr.adjusted(4, 0, -2, 0), Qt::AlignLeft | Qt::AlignVCenter,
                     QString::fromStdString(modifierDisplayName(up->modifiers[i]->type())));
        }
    }
    if (drag_ == Drag::Connect) {
      p.setPen(QPen(QColor(0xf0, 0xf0, 0x60), 2));
      p.drawLine(boxRect(dragNode_).center(), last_);
    }
    if (drag_ == Drag::Band) {
      p.setPen(QPen(Qt::white, 1, Qt::DashLine));
      p.setBrush(QColor(255, 255, 255, 25));
      p.drawRect(QRectF(press_, last_).normalized());
    }
  }

  void mousePressEvent(QMouseEvent* e) override {
    setFocus();
    press_ = last_ = e->position();
    if (e->button() == Qt::MiddleButton) {
      drag_ = Drag::Pan;
      return;
    }
    if (e->button() != Qt::LeftButton) return;
    uint32_t id = boxAt(e->position());
    SceneDocument* doc = c_->document();
    if (!id) {
      drag_ = Drag::Band;
      if (!(e->modifiers() & Qt::ControlModifier)) doc->clearSelection();
      return;
    }
    if (connectMode) {
      drag_ = Drag::Connect;
      dragNode_ = id;
      return;
    }
    if (e->modifiers() & Qt::ControlModifier) {
      if (doc->isSelected(id)) doc->deselect(id);
      else doc->select(id, true);
    } else if (!doc->isSelected(id)) {
      doc->select(id, false);
    }
    drag_ = Drag::Move;
    dragNode_ = id;
  }

  void mouseMoveEvent(QMouseEvent* e) override {
    QPointF d = e->position() - last_;
    switch (drag_) {
      case Drag::Pan: pan_ += d; break;
      case Drag::Move:
        for (uint32_t id : c_->document()->selection())
          if (pos_.count(id)) pos_[id] += d / zoom_;
        break;
      default: break;
    }
    last_ = e->position();
    if (drag_ != Drag::None) update();
  }

  void mouseReleaseEvent(QMouseEvent* e) override {
    if (drag_ == Drag::Connect) {
      uint32_t parent = boxAt(e->position());
      if (parent && parent != dragNode_) {
        SceneDocument* doc = c_->document();
        doc->beginEdit(tr("Link"));
        if (doc->scene().setParent(dragNode_, parent)) doc->notifyStructureChanged();
        doc->endEdit();
        relayout();
      }
    } else if (drag_ == Drag::Band) {
      QRectF band = QRectF(press_, last_).normalized();
      QVector<uint32_t> ids = c_->document()->selection();
      for (const auto& kv : pos_)
        if (band.intersects(boxRect(kv.first)) && !ids.contains(kv.first)) ids.push_back(kv.first);
      c_->document()->setSelection(ids);
    }
    drag_ = Drag::None;
    update();
  }

  void mouseDoubleClickEvent(QMouseEvent* e) override {
    uint32_t id = boxAt(e->position());
    if (!id) return;
    // Select the node and its children.
    QVector<uint32_t> ids{id};
    std::function<void(uint32_t)> add = [&](uint32_t p) {
      for (Node* ch : c_->scene().children(p)) {
        ids.push_back(ch->id);
        add(ch->id);
      }
    };
    add(id);
    c_->document()->setSelection(ids);
  }

  void wheelEvent(QWheelEvent* e) override {
    double f = e->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
    QPointF c = e->position();
    pan_ = c + (pan_ - c) * f;
    zoom_ *= f;
    update();
  }

  void contextMenuEvent(QContextMenuEvent* e) override {
    QMenu m(this);
    m.addAction(tr("Unlink Selected"), this, [this] {
      SceneDocument* doc = c_->document();
      doc->beginEdit(tr("Unlink"));
      for (uint32_t id : doc->selection()) doc->scene().setParent(id, 0);
      doc->notifyStructureChanged();
      doc->endEdit();
      relayout();
    });
    m.addAction(tr("Arrange Children"), this, [this] { relayout(); });
    m.addAction(tr("Zoom Extents"), this, [this] { zoomExtents(); });
    m.exec(e->globalPos());
  }

 private:
  enum class Drag { None, Move, Pan, Band, Connect };
  AnimController* c_;
  std::map<uint32_t, QPointF> pos_;
  QPointF pan_{20, 20};
  double zoom_ = 1.0;
  Drag drag_ = Drag::None;
  QPointF press_, last_;
  uint32_t dragNode_ = 0;
};

class SchematicWindow : public QWidget {
 public:
  SchematicWindow(MainWindow& w, AnimController* c) : QWidget(&w, Qt::Window) {
    setObjectName("schematicView");
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("Schematic View"));
    resize(900, 560);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(2, 2, 2, 2);
    auto* tb = new QToolBar(this);
    v->addWidget(tb);
    canvas_ = new SchematicCanvas(c);
    v->addWidget(canvas_, 1);
    auto* modes = new QActionGroup(this);
    QAction* sel = tb->addAction(tr("Select"));
    QAction* link = tb->addAction(tr("Connect (Link)"));
    for (QAction* a : {sel, link}) {
      a->setCheckable(true);
      modes->addAction(a);
    }
    sel->setChecked(true);
    connect(sel, &QAction::triggered, this, [this] { canvas_->connectMode = false; });
    connect(link, &QAction::triggered, this, [this] { canvas_->connectMode = true; });
    tb->addSeparator();
    QAction* rel = tb->addAction(tr("Show Relationships"));
    rel->setCheckable(true);
    rel->setChecked(true);
    connect(rel, &QAction::toggled, this, [this](bool on) {
      canvas_->showRelations = on;
      canvas_->update();
    });
    QAction* mods = tb->addAction(tr("Show Modifiers"));
    mods->setCheckable(true);
    mods->setChecked(true);
    connect(mods, &QAction::toggled, this, [this](bool on) {
      canvas_->showModifiers = on;
      canvas_->update();
    });
    tb->addSeparator();
    tb->addAction(tr("Arrange"), this, [this] { canvas_->relayout(); });
    tb->addAction(animicons::frameAll(), tr("Zoom Extents"), this, [this] { canvas_->zoomExtents(); });
    connect(c->document(), &SceneDocument::structureChanged, this, [this] { canvas_->sync(); });
    connect(c->document(), &SceneDocument::selectionChanged, canvas_, [this] { canvas_->update(); });
    connect(c->document(), &SceneDocument::nodesChanged, canvas_, [this] { canvas_->update(); });
    connect(c, &AnimController::animationChanged, canvas_, [this] { canvas_->update(); });
  }
  void showEvent(QShowEvent* e) override {
    QWidget::showEvent(e);
    canvas_->relayout();
  }

 private:
  SchematicCanvas* canvas_;
};

QPointer<SchematicWindow> g_schematic;

void installSchematic(MainWindow& w) {
  QMenu* graph = w.menuAt("Graph Editors");
  graph->addSeparator();
  QAction* a = graph->addAction(animicons::schematic(), QObject::tr("Schematic View..."));
  w.registerCommand("graph.schematic", a);
  QObject::connect(a, &QAction::triggered, &w, [&w] { openSchematicView(w); });
}

}  // namespace

void openSchematicView(MainWindow& w) {
  AnimController* c = AnimController::instance();
  if (!c) return;
  if (!g_schematic) g_schematic = new SchematicWindow(w, c);
  g_schematic->show();
  g_schematic->raise();
  g_schematic->activateWindow();
}

VERAA_APP_EXTENSION(schematicView, 304, installSchematic);

}  // namespace veraa
