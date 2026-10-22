#include "slateeditor.h"

#include <cmath>
#include <functional>

#include <QDockWidget>
#include <QScrollBar>
#include <QTimer>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsItem>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "renderer.h"
#include "theme.h"

namespace veraa {

namespace {

constexpr double kNodeWidth = 210.0;
constexpr double kHeader = 24.0;
constexpr double kThumb = 56.0;
constexpr double kRow = 19.0;
constexpr const char* kMime = "application/x-veraa-slate";

QString pathKeyString(const MtlPath& path) {
  QString s = "root";
  for (const auto& st : path)
    s += st.kind == MtlPathStep::Kind::Sub ? QString("/s%1").arg(st.sub) : QString("/m%1").arg(QString::fromStdString(st.key));
  return s;
}

bool isPrefix(const MtlPath& a, const MtlPath& b) {
  if (a.size() > b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (!(a[i] == b[i])) return false;
  return true;
}

class BrowserTree : public QTreeWidget {
 public:
  using QTreeWidget::QTreeWidget;

 protected:
  QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override {
    if (items.isEmpty() || items.first()->data(0, Qt::UserRole).toString().isEmpty()) return nullptr;
    auto* m = new QMimeData;
    m->setData(kMime, items.first()->data(0, Qt::UserRole).toString().toUtf8());
    return m;
  }
  QStringList mimeTypes() const override { return {kMime}; }
};

}  // namespace

// ---------------------------------------------------------------------------------------------
// Node item

class SlateNodeItem : public QGraphicsItem {
 public:
  enum class Kind { Material, Map };
  struct Socket {
    QString label;
    MtlPathStep step;
    bool isMaterial = false;  // sub-material socket
    bool connected = false;
  };

  Kind kind = Kind::Map;
  bool floating = false;
  int floatingIndex = -1;
  bool isRoot = false;
  MtlPath path;
  QString title, subtitle;
  QImage thumb;
  QColor swatch;
  std::vector<Socket> sockets;
  std::function<void()> moved;

  SlateNodeItem() {
    setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
    setZValue(1);
  }
  double height() const { return kHeader + kThumb + 8 + sockets.size() * kRow + 6; }
  QRectF boundingRect() const override { return QRectF(-8, -2, kNodeWidth + 16, height() + 4); }
  QPointF outputPort() const { return mapToScene(QPointF(kNodeWidth, kHeader * 0.5)); }
  QPointF socketPos(int i) const { return mapToScene(QPointF(0, kHeader + kThumb + 8 + i * kRow + kRow * 0.5)); }
  bool outputAt(const QPointF& scenePos) const {
    if (isRoot) return false;
    return QLineF(scenePos, outputPort()).length() < 10.0;
  }
  int socketAt(const QPointF& scenePos) const {
    QPointF local = mapFromScene(scenePos);
    if (local.x() < -10 || local.x() > kNodeWidth * 0.6) return -1;
    double y = local.y() - (kHeader + kThumb + 8);
    if (y < 0) return -1;
    int i = static_cast<int>(y / kRow);
    return i < static_cast<int>(sockets.size()) ? i : -1;
  }
  void paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) override {
    p->setRenderHint(QPainter::Antialiasing);
    QRectF r(0, 0, kNodeWidth, height());
    QColor body(0x3c, 0x3c, 0x3c), head = kind == Kind::Material ? QColor(0x4a, 0x5f, 0x86) : QColor(0x6b, 0x55, 0x2e);
    if (floating) head = head.darker(140);
    p->setPen(QPen(isSelected() ? QColor(0xff, 0xd0, 0x60) : QColor(0x18, 0x18, 0x18), isSelected() ? 2.0 : 1.0));
    p->setBrush(body);
    p->drawRoundedRect(r, 5, 5);
    QPainterPath hp;
    hp.addRoundedRect(QRectF(0, 0, kNodeWidth, kHeader), 5, 5);
    p->fillPath(hp, head);
    p->setPen(Qt::white);
    QFont f = p->font();
    f.setBold(true);
    p->setFont(f);
    p->drawText(QRectF(8, 0, kNodeWidth - 24, kHeader), Qt::AlignVCenter | Qt::AlignLeft,
                p->fontMetrics().elidedText(title, Qt::ElideRight, static_cast<int>(kNodeWidth - 26)));
    f.setBold(false);
    p->setFont(f);
    QRectF thumbRect(8, kHeader + 4, kThumb, kThumb - 4);
    if (!thumb.isNull()) p->drawImage(thumbRect, thumb);
    else p->fillRect(thumbRect, swatch.isValid() ? swatch : QColor(0x55, 0x55, 0x55));
    p->setPen(QColor(0xc8, 0xc8, 0xc8));
    p->drawText(QRectF(kThumb + 16, kHeader + 4, kNodeWidth - kThumb - 22, kThumb - 4), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap,
                subtitle + (floating ? QObject::tr("\n(not connected)") : QString()));
    for (size_t i = 0; i < sockets.size(); ++i) {
      double y = kHeader + kThumb + 8 + i * kRow;
      p->setPen(QColor(0xb8, 0xb8, 0xb8));
      p->drawText(QRectF(12, y, kNodeWidth - 16, kRow), Qt::AlignVCenter | Qt::AlignLeft, sockets[i].label);
      p->setPen(QColor(0x18, 0x18, 0x18));
      p->setBrush(sockets[i].connected ? QColor(0x7f, 0xc8, 0x5a) : (sockets[i].isMaterial ? QColor(0x6d, 0x8c, 0xc8) : QColor(0x9a, 0x9a, 0x9a)));
      p->drawEllipse(QPointF(0, y + kRow * 0.5), 4.5, 4.5);
    }
    if (!isRoot) {
      p->setBrush(QColor(0xe0, 0xb0, 0x50));
      p->drawEllipse(QPointF(kNodeWidth, kHeader * 0.5), 5, 5);
    }
  }

 protected:
  QVariant itemChange(GraphicsItemChange change, const QVariant& value) override {
    if (change == ItemPositionHasChanged && moved) moved();
    return QGraphicsItem::itemChange(change, value);
  }
};

namespace {

QPainterPath wirePath(const QPointF& a, const QPointF& b) {
  QPainterPath path(a);
  double dx = std::max(60.0, std::abs(b.x() - a.x()) * 0.5);
  path.cubicTo(a + QPointF(dx, 0), b - QPointF(dx, 0), b);
  return path;
}

struct Wire {
  SlateNodeItem* from;
  SlateNodeItem* to;
  int socket;
  QGraphicsPathItem* item;
};

}  // namespace

// ---------------------------------------------------------------------------------------------
// View

SlateGraphView::SlateGraphView(QWidget* parent) : QGraphicsView(parent) {
  setRenderHint(QPainter::Antialiasing);
  setDragMode(QGraphicsView::RubberBandDrag);
  setAcceptDrops(true);
  setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
  setBackgroundBrush(QColor(0x2a, 0x2a, 0x2a));
}

static SlateNodeItem* nodeAt(QGraphicsScene* scene, const QPointF& p, int* socket, bool* output) {
  for (QGraphicsItem* it : scene->items(QRectF(p - QPointF(12, 12), QSizeF(24, 24)))) {
    auto* n = dynamic_cast<SlateNodeItem*>(it);
    if (!n) continue;
    if (output && n->outputAt(p)) {
      *output = true;
      return n;
    }
    if (socket) {
      int s = n->socketAt(p);
      if (s >= 0) {
        *socket = s;
        return n;
      }
    }
  }
  return nullptr;
}

void SlateGraphView::mousePressEvent(QMouseEvent* e) {
  if (e->button() == Qt::MiddleButton) {
    panning_ = true;
    panLast_ = e->pos();
    return;
  }
  if (e->button() == Qt::LeftButton) {
    QPointF sp = mapToScene(e->pos());
    bool output = false;
    SlateNodeItem* n = nodeAt(scene(), sp, nullptr, &output);
    if (n && output) {
      dragFrom_ = n;
      tempWire_ = scene()->addPath(wirePath(n->outputPort(), sp), QPen(QColor(0xff, 0xd0, 0x60), 2.0));
      tempWire_->setZValue(5);
      return;
    }
  }
  QGraphicsView::mousePressEvent(e);
}

void SlateGraphView::mouseMoveEvent(QMouseEvent* e) {
  if (panning_) {
    QPoint d = e->pos() - panLast_;
    panLast_ = e->pos();
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() - d.x());
    verticalScrollBar()->setValue(verticalScrollBar()->value() - d.y());
    return;
  }
  if (tempWire_ && dragFrom_) {
    tempWire_->setPath(wirePath(dragFrom_->outputPort(), mapToScene(e->pos())));
    return;
  }
  QGraphicsView::mouseMoveEvent(e);
}

void SlateGraphView::mouseReleaseEvent(QMouseEvent* e) {
  if (panning_ && e->button() == Qt::MiddleButton) {
    panning_ = false;
    return;
  }
  if (tempWire_) {
    scene()->removeItem(tempWire_);
    delete tempWire_;
    tempWire_ = nullptr;
    int socket = -1;
    SlateNodeItem* to = nodeAt(scene(), mapToScene(e->pos()), &socket, nullptr);
    SlateNodeItem* from = dragFrom_;
    dragFrom_ = nullptr;
    if (to && to != from && socket >= 0) Q_EMIT connectRequested(from, to, socket, e->modifiers() & Qt::ShiftModifier);
    return;
  }
  QGraphicsView::mouseReleaseEvent(e);
}

void SlateGraphView::wheelEvent(QWheelEvent* e) {
  double f = std::pow(1.15, e->angleDelta().y() / 120.0);
  double cur = transform().m11();
  if (cur * f < 0.15 || cur * f > 3.0) return;
  scale(f, f);
}

void SlateGraphView::keyPressEvent(QKeyEvent* e) {
  if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) {
    Q_EMIT deleteRequested();
    return;
  }
  QGraphicsView::keyPressEvent(e);
}

void SlateGraphView::dragEnterEvent(QDragEnterEvent* e) {
  if (e->mimeData()->hasFormat(kMime)) e->acceptProposedAction();
}

void SlateGraphView::dragMoveEvent(QDragMoveEvent* e) {
  if (e->mimeData()->hasFormat(kMime)) e->acceptProposedAction();
}

void SlateGraphView::dropEvent(QDropEvent* e) {
  if (!e->mimeData()->hasFormat(kMime)) return;
  Q_EMIT dropRequested(QString::fromUtf8(e->mimeData()->data(kMime)), mapToScene(e->position().toPoint()));
  e->acceptProposedAction();
}

// ---------------------------------------------------------------------------------------------
// Editor

namespace {
std::vector<Wire>& wiresOf(QGraphicsScene* s) {
  static std::map<QGraphicsScene*, std::vector<Wire>> all;
  return all[s];
}
}  // namespace

SlateEditor::SlateEditor(SceneDocument* doc, QWidget* parent) : QMainWindow(parent, Qt::Window), doc_(doc) {
  setWindowTitle(tr("Slate Material Editor - Veraa"));
  resize(1280, 780);

  // Browser (left).
  auto* left = new QDockWidget(tr("Material/Map Browser"), this);
  left->setFeatures(QDockWidget::NoDockWidgetFeatures);
  auto* lw = new QWidget(left);
  auto* lv = new QVBoxLayout(lw);
  lv->setContentsMargins(4, 4, 4, 4);
  browser_ = new BrowserTree(lw);
  browser_->setHeaderHidden(true);
  browser_->setDragEnabled(true);
  browser_->setDragDropMode(QAbstractItemView::DragOnly);
  auto addGroup = [&](const QString& title) {
    auto* g = new QTreeWidgetItem(browser_, {title});
    g->setFlags(Qt::ItemIsEnabled);
    g->setExpanded(true);
    return g;
  };
  QTreeWidgetItem* mtls = addGroup(tr("Materials"));
  for (const auto& t : materialTypes()) {
    auto* it = new QTreeWidgetItem(mtls, {QString::fromStdString(t.displayName)});
    it->setData(0, Qt::UserRole, QString("mtl:%1").arg(QString::fromStdString(t.type)));
    it->setToolTip(0, QString::fromStdString(t.description));
  }
  std::map<std::string, QTreeWidgetItem*> mapGroups;
  for (const auto& t : texMapTypes()) {
    QTreeWidgetItem*& g = mapGroups[t.category];
    if (!g) g = addGroup(tr("Maps: %1").arg(QString::fromStdString(t.category)));
    auto* it = new QTreeWidgetItem(g, {QString::fromStdString(t.displayName)});
    it->setData(0, Qt::UserRole, QString("map:%1").arg(QString::fromStdString(t.type)));
  }
  lv->addWidget(new QLabel(tr("Drag onto a socket (or the canvas):"), lw));
  lv->addWidget(browser_, 2);
  lv->addWidget(new QLabel(tr("Scene materials:"), lw));
  materials_ = new QTreeWidget(lw);
  materials_->setHeaderHidden(true);
  lv->addWidget(materials_, 1);
  left->setWidget(lw);
  left->setMinimumWidth(230);
  addDockWidget(Qt::LeftDockWidgetArea, left);

  // Parameters (right).
  auto* right = new QDockWidget(tr("Parameters"), this);
  right->setFeatures(QDockWidget::NoDockWidgetFeatures);
  auto* rw = new QWidget(right);
  auto* rv = new QVBoxLayout(rw);
  rv->setContentsMargins(4, 4, 4, 4);
  title_ = new QLabel(rw);
  title_->setWordWrap(true);
  title_->setStyleSheet("font-weight: bold;");
  rv->addWidget(title_);
  auto* scroll = new QScrollArea(rw);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  level_ = new LevelEditor(doc_);
  scroll->setWidget(level_);
  rv->addWidget(scroll, 1);
  right->setWidget(rw);
  right->setMinimumWidth(360);
  addDockWidget(Qt::RightDockWidgetArea, right);

  // Graph (center).
  scene_ = new QGraphicsScene(this);
  scene_->setSceneRect(-4000, -3000, 8000, 6000);
  view_ = new SlateGraphView(this);
  view_->setScene(scene_);
  setCentralWidget(view_);

  auto* tb = addToolBar(tr("Slate"));
  tb->setMovable(false);
  tb->addAction(tr("Lay Out All"), this, [this] {
    positions_.clear();
    layoutAll();
  });
  tb->addAction(tr("Fit"), this, [this] { view_->fitInView(scene_->itemsBoundingRect().adjusted(-40, -40, 40, 40), Qt::KeepAspectRatio); });
  tb->addAction(tr("Delete Selected"), this, &SlateEditor::deleteSelection);
  tb->addSeparator();
  auto* hint = new QLabel(tr("  Drag from an output (yellow) to a socket to connect; Shift-drag copies. Delete removes nodes or wires."), this);
  hint->setStyleSheet("color: #a8a8a8;");
  tb->addWidget(hint);

  connect(view_, &SlateGraphView::connectRequested, this, &SlateEditor::connectNodes);
  connect(view_, &SlateGraphView::dropRequested, this, &SlateEditor::dropItem);
  connect(view_, &SlateGraphView::deleteRequested, this, &SlateEditor::deleteSelection);
  connect(scene_, &QGraphicsScene::selectionChanged, this, [this] {
    QList<QGraphicsItem*> sel = scene_->selectedItems();
    SlateNodeItem* n = nullptr;
    for (auto* it : sel)
      if ((n = dynamic_cast<SlateNodeItem*>(it))) break;
    if (n) selectNode(n);
  });
  connect(browser_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* it) {
    QString what = it ? it->data(0, Qt::UserRole).toString() : QString();
    if (!what.isEmpty()) dropItem(what, view_->mapToScene(view_->viewport()->rect().center()));
  });
  connect(materials_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* it) {
    if (it) showMaterial(it->data(0, Qt::UserRole).toUInt());
  });
  connect(level_, &LevelEditor::navigateRequested, this, [this](const MtlPath& p) {
    level_->setTarget(materialId_, p);
    title_->setText(describeMtlPath(doc_->scene().findMaterial(materialId_), p));
    rebuildGraph();
  });
  connect(level_, &LevelEditor::edited, this, [this] {
    // Thumbnails and titles may change; structure changes come through materialsChanged.
    QTimer::singleShot(0, this, [this] { rebuildGraph(); });
  });
  connect(doc_, &SceneDocument::materialsChanged, this, [this] {
    rebuildMaterialList();
    if (!doc_->scene().findMaterial(materialId_)) materialId_ = 0;
    rebuildGraph();
  });
  connect(doc_, &SceneDocument::structureChanged, this, [this] {
    rebuildMaterialList();
    rebuildGraph();
  });
  rebuildMaterialList();
}

int SlateEditor::nodeCount() const {
  int n = 0;
  for (auto* it : scene_->items())
    if (dynamic_cast<SlateNodeItem*>(it)) ++n;
  return n;
}

void SlateEditor::rebuildMaterialList() {
  materials_->clear();
  for (const auto& m : doc_->scene().materials) {
    auto* it = new QTreeWidgetItem(materials_, {QString::fromStdString(m->name)});
    it->setData(0, Qt::UserRole, m->id);
    if (m->id == materialId_) materials_->setCurrentItem(it);
  }
}

void SlateEditor::showMaterial(uint32_t id) {
  if (id != materialId_) positions_.clear();
  materialId_ = id;
  rebuildMaterialList();
  level_->setTarget(id, {});
  rebuildGraph();
  view_->fitInView(scene_->itemsBoundingRect().adjusted(-60, -60, 60, 60), Qt::KeepAspectRatio);
  if (view_->transform().m11() > 1.0) view_->resetTransform();
}

void SlateEditor::rebuildGraph() {
  // Remember positions of existing nodes.
  for (auto* it : scene_->items())
    if (auto* n = dynamic_cast<SlateNodeItem*>(it)) {
      if (n->floating) {
        if (n->kind == SlateNodeItem::Kind::Map && n->floatingIndex < static_cast<int>(floatingMapPos_.size()))
          floatingMapPos_[n->floatingIndex] = n->pos();
        else if (n->kind == SlateNodeItem::Kind::Material && n->floatingIndex < static_cast<int>(floatingMtlPos_.size()))
          floatingMtlPos_[n->floatingIndex] = n->pos();
      } else {
        positions_[pathKeyString(n->path).toStdString()] = n->pos();
      }
    }
  QString selectedKey;
  for (auto* it : scene_->selectedItems())
    if (auto* n = dynamic_cast<SlateNodeItem*>(it)) selectedKey = n->floating ? QString() : pathKeyString(n->path);
  scene_->clearSelection();
  wiresOf(scene_).clear();
  scene_->clear();
  Material* root = doc_->scene().findMaterial(materialId_);
  setWindowTitle(root ? tr("Slate Material Editor - %1 - Veraa").arg(QString::fromStdString(root->name))
                      : tr("Slate Material Editor - Veraa"));
  std::map<int, double> columnY;
  std::vector<Wire>& wires = wiresOf(scene_);
  auto updateWires = [this] {
    for (auto& w : wiresOf(scene_)) w.item->setPath(wirePath(w.from->outputPort(), w.to->socketPos(w.socket)));
  };

  std::function<SlateNodeItem*(const MtlPath&, int)> addNode = [&](const MtlPath& path, int depth) -> SlateNodeItem* {
    MtlTarget t = resolveMtlPath(root, path);
    if (!t.valid()) return nullptr;
    auto* n = new SlateNodeItem;
    n->path = path;
    n->isRoot = path.empty();
    if (t.material) {
      n->kind = SlateNodeItem::Kind::Material;
      n->title = QString::fromStdString(t.material->name);
      n->subtitle = QString::fromStdString(materialDisplayName(t.material->materialType));
      Vec3 c = t.material->viewportColor();
      n->swatch = QColor::fromRgbF(std::min(1.0f, std::pow(c.x, 1 / 2.2f)), std::min(1.0f, std::pow(c.y, 1 / 2.2f)),
                                   std::min(1.0f, std::pow(c.z, 1 / 2.2f)));
      for (size_t i = 0; i < t.material->subs.size(); ++i) {
        SlateNodeItem::Socket s;
        s.label = QString::fromStdString(t.material->subs[i].label);
        s.step = {MtlPathStep::Kind::Sub, static_cast<int>(i), {}};
        s.isMaterial = true;
        s.connected = t.material->subs[i].material != nullptr;
        n->sockets.push_back(s);
      }
      for (const auto& k : t.material->mappableKeys()) {
        SlateNodeItem::Socket s;
        s.label = slotLabel(&t.material->params, k);
        s.step = {MtlPathStep::Kind::Map, 0, k};
        s.connected = t.material->mapFor(k) != nullptr || (t.material->maps.count(k) && t.material->maps.at(k).map);
        n->sockets.push_back(s);
      }
    } else {
      n->kind = SlateNodeItem::Kind::Map;
      n->title = QString::fromStdString(t.map->name.empty() ? t.map->type : t.map->name);
      n->subtitle = mapButtonText(t.map);
      std::vector<uint8_t> rgba = renderMapPreview(previewCopy(*t.map, doc_->scene()), nullptr, 48);
      n->thumb = QImage(rgba.data(), 48, 48, 48 * 4, QImage::Format_RGBA8888).copy();
      for (const auto& k : t.map->mappableKeys()) {
        SlateNodeItem::Socket s;
        s.label = slotLabel(&t.map->params, k);
        s.step = {MtlPathStep::Kind::Map, 0, k};
        s.connected = t.map->maps.count(k) && t.map->maps.at(k).map;
        n->sockets.push_back(s);
      }
    }
    scene_->addItem(n);
    auto key = pathKeyString(path).toStdString();
    auto pit = positions_.find(key);
    if (pit != positions_.end()) {
      n->setPos(pit->second);
    } else {
      double& y = columnY[depth];
      n->setPos(-depth * 290.0, y);
      y += n->height() + 24;
    }
    for (size_t i = 0; i < n->sockets.size(); ++i) {
      if (!n->sockets[i].connected) continue;
      MtlPath child = path;
      child.push_back(n->sockets[i].step);
      if (SlateNodeItem* c = addNode(child, depth + 1)) {
        auto* item = scene_->addPath(QPainterPath(), QPen(QColor(0xd8, 0xd8, 0xd8), 1.6));
        item->setZValue(0);
        item->setFlag(QGraphicsItem::ItemIsSelectable);
        item->setData(0, static_cast<int>(wires.size()));
        wires.push_back({c, n, static_cast<int>(i), item});
      }
    }
    n->moved = updateWires;
    if (!selectedKey.isEmpty() && pathKeyString(path) == selectedKey) n->setSelected(true);
    return n;
  };
  if (root) addNode({}, 0);

  // Floating nodes.
  for (size_t i = 0; i < floatingMaps_.size(); ++i) {
    auto* n = new SlateNodeItem;
    n->kind = SlateNodeItem::Kind::Map;
    n->floating = true;
    n->floatingIndex = static_cast<int>(i);
    n->title = QString::fromStdString(floatingMaps_[i]->name);
    n->subtitle = mapButtonText(floatingMaps_[i].get());
    std::vector<uint8_t> rgba = renderMapPreview(previewCopy(*floatingMaps_[i], doc_->scene()), nullptr, 48);
    n->thumb = QImage(rgba.data(), 48, 48, 48 * 4, QImage::Format_RGBA8888).copy();
    scene_->addItem(n);
    n->setPos(i < floatingMapPos_.size() ? floatingMapPos_[i] : QPointF(-900, 400 + 120.0 * i));
  }
  for (size_t i = 0; i < floatingMtls_.size(); ++i) {
    auto* n = new SlateNodeItem;
    n->kind = SlateNodeItem::Kind::Material;
    n->floating = true;
    n->floatingIndex = static_cast<int>(i);
    n->title = QString::fromStdString(floatingMtls_[i]->name);
    n->subtitle = QString::fromStdString(materialDisplayName(floatingMtls_[i]->materialType));
    scene_->addItem(n);
    n->setPos(i < floatingMtlPos_.size() ? floatingMtlPos_[i] : QPointF(-900, -300 - 120.0 * i));
  }
  updateWires();
}

void SlateEditor::layoutAll() {
  rebuildGraph();
  view_->fitInView(scene_->itemsBoundingRect().adjusted(-60, -60, 60, 60), Qt::KeepAspectRatio);
}

void SlateEditor::selectNode(SlateNodeItem* n) {
  if (n->floating) {
    title_->setText(tr("%1 is not connected. Wire its output into a socket to use and edit it.").arg(n->title));
    level_->setTarget(0, {});
    return;
  }
  Material* root = doc_->scene().findMaterial(materialId_);
  title_->setText(describeMtlPath(root, n->path));
  if (level_->materialId() != materialId_ || level_->path() != n->path) level_->setTarget(materialId_, n->path);
}

void SlateEditor::connectNodes(SlateNodeItem* from, SlateNodeItem* to, int socket, bool copy) {
  if (!from || !to || to->floating || socket < 0 || socket >= static_cast<int>(to->sockets.size())) return;
  const SlateNodeItem::Socket& sock = to->sockets[socket];
  bool fromMaterial = from->kind == SlateNodeItem::Kind::Material;
  if (fromMaterial != sock.isMaterial) return;  // maps go into map sockets, materials into sub-material sockets
  if (!from->floating && isPrefix(from->path, to->path)) return;  // would create a cycle
  Material* root = doc_->scene().findMaterial(materialId_);
  if (!root) return;
  MtlPath fromPath = from->path, toPath = to->path;
  MtlPathStep step = sock.step;
  int floatingIndex = from->floatingIndex;
  bool fromFloating = from->floating;
  doc_->commitSoftEdit();
  doc_->beginEdit(tr("Connect Nodes"));
  root = doc_->scene().findMaterial(materialId_);
  std::unique_ptr<TexMap> map;
  std::unique_ptr<Material> mtl;
  if (fromFloating) {
    if (fromMaterial) {
      mtl = copy ? std::make_unique<Material>(*floatingMtls_[floatingIndex]) : std::move(floatingMtls_[floatingIndex]);
    } else {
      map = copy ? std::make_unique<TexMap>(*floatingMaps_[floatingIndex]) : std::move(floatingMaps_[floatingIndex]);
    }
  } else {
    MtlPath parentPath(fromPath.begin(), fromPath.end() - 1);
    MtlTarget parent = resolveMtlPath(root, parentPath);
    const MtlPathStep& last = fromPath.back();
    if (last.kind == MtlPathStep::Kind::Sub && parent.material) {
      auto& slot = parent.material->subs[last.sub].material;
      mtl = copy ? std::make_unique<Material>(*slot) : std::move(slot);
    } else if (last.kind == MtlPathStep::Kind::Map) {
      MapSlots* slots = parent.material ? &parent.material->maps : (parent.map ? &parent.map->maps : nullptr);
      if (slots && slots->count(last.key)) {
        auto& m = (*slots)[last.key].map;
        map = copy ? std::make_unique<TexMap>(*m) : std::move(m);
        if (!copy) slots->erase(last.key);
      }
    }
  }
  MtlTarget target = resolveMtlPath(root, toPath);
  if (target.valid()) {
    if (step.kind == MtlPathStep::Kind::Sub && target.material && mtl) {
      auto& slot = target.material->subs[step.sub].material;
      if (slot) floatingMtls_.push_back(std::move(slot));
      slot = std::move(mtl);
    } else if (step.kind == MtlPathStep::Kind::Map && map) {
      MapSlots* slots = target.material ? &target.material->maps : &target.map->maps;
      MapSlot& s = (*slots)[step.key];
      if (s.map) floatingMaps_.push_back(std::move(s.map));
      s.map = std::move(map);
      s.enabled = true;
    }
  }
  // Drop emptied floating entries.
  floatingMaps_.erase(std::remove(floatingMaps_.begin(), floatingMaps_.end(), nullptr), floatingMaps_.end());
  floatingMtls_.erase(std::remove(floatingMtls_.begin(), floatingMtls_.end(), nullptr), floatingMtls_.end());
  floatingMapPos_.resize(floatingMaps_.size());
  floatingMtlPos_.resize(floatingMtls_.size());
  doc_->notifyMaterialsChanged();
  doc_->endEdit();
  rebuildGraph();
}

void SlateEditor::dropItem(const QString& what, const QPointF& pos) {
  bool isMap = what.startsWith("map:"), isMtl = what.startsWith("mtl:");
  if (!isMap && !isMtl) return;
  std::string type = what.mid(4).toStdString();
  // Dropped on a matching socket: create it there.
  int socket = -1;
  SlateNodeItem* target = nullptr;
  for (QGraphicsItem* it : scene_->items(QRectF(pos - QPointF(12, 12), QSizeF(24, 24))))
    if (auto* n = dynamic_cast<SlateNodeItem*>(it)) {
      int s = n->socketAt(pos);
      if (s >= 0 && !n->floating && n->sockets[s].isMaterial == isMtl) {
        target = n;
        socket = s;
        break;
      }
    }
  if (isMap) {
    auto map = createTexMap(type);
    if (!map) return;
    if (type == "Bitmap") {
      QString start = doc_->filePath().isEmpty() ? QString() : QFileInfo(doc_->filePath()).absolutePath();
      QString f = QFileDialog::getOpenFileName(this, tr("Select Bitmap Image File"), start,
                                               tr("Images (*.png *.jpg *.jpeg *.bmp *.tga *.tif *.tiff *.hdr *.exr);;All files (*)"));
      if (!f.isEmpty()) {
        map->params.setString("file", f.toStdString());
        map->name = QFileInfo(f).fileName().toStdString();
      }
    }
    floatingMaps_.push_back(std::move(map));
    floatingMapPos_.push_back(pos);
  } else {
    auto m = std::make_unique<Material>(type);
    m->name = materialDisplayName(type);
    floatingMtls_.push_back(std::move(m));
    floatingMtlPos_.push_back(pos);
  }
  rebuildGraph();
  if (target) {
    // Find the floating node just added and connect it.
    for (auto* it : scene_->items())
      if (auto* n = dynamic_cast<SlateNodeItem*>(it)) {
        bool match = n->floating && ((isMap && n->kind == SlateNodeItem::Kind::Map && n->floatingIndex == static_cast<int>(floatingMaps_.size()) - 1) ||
                                     (isMtl && n->kind == SlateNodeItem::Kind::Material && n->floatingIndex == static_cast<int>(floatingMtls_.size()) - 1));
        if (!match) continue;
        // Re-find the target after the rebuild by path.
        for (auto* it2 : scene_->items())
          if (auto* t2 = dynamic_cast<SlateNodeItem*>(it2))
            if (!t2->floating && t2->path == target->path) {
              connectNodes(n, t2, socket, false);
              return;
            }
      }
  }
}

bool SlateEditor::addMapToRoot(const std::string& key, const std::string& type) {
  Material* root = doc_->scene().findMaterial(materialId_);
  if (!root) return false;
  auto map = createTexMap(type);
  if (!map) return false;
  floatingMaps_.push_back(std::move(map));
  floatingMapPos_.push_back(QPointF(-600, 0));
  rebuildGraph();
  SlateNodeItem *from = nullptr, *to = nullptr;
  for (auto* it : scene_->items())
    if (auto* n = dynamic_cast<SlateNodeItem*>(it)) {
      if (n->floating && n->kind == SlateNodeItem::Kind::Map) from = n;
      if (n->isRoot) to = n;
    }
  if (!from || !to) return false;
  for (size_t i = 0; i < to->sockets.size(); ++i)
    if (!to->sockets[i].isMaterial && to->sockets[i].step.key == key) {
      connectNodes(from, to, static_cast<int>(i), false);
      return root->mapFor(key) != nullptr || doc_->scene().findMaterial(materialId_)->mapFor(key) != nullptr;
    }
  return false;
}

void SlateEditor::deleteSelection() {
  Material* root = doc_->scene().findMaterial(materialId_);
  std::vector<MtlPath> remove;     // nodes to delete
  std::vector<MtlPath> detach;     // wires to cut (child path)
  std::vector<int> floatMaps, floatMtls;
  for (auto* it : scene_->selectedItems()) {
    if (auto* n = dynamic_cast<SlateNodeItem*>(it)) {
      if (n->floating) (n->kind == SlateNodeItem::Kind::Map ? floatMaps : floatMtls).push_back(n->floatingIndex);
      else if (!n->isRoot) remove.push_back(n->path);
    } else if (auto* w = dynamic_cast<QGraphicsPathItem*>(it)) {
      int idx = w->data(0).toInt();
      auto& wires = wiresOf(scene_);
      if (idx >= 0 && idx < static_cast<int>(wires.size()) && wires[idx].item == w) detach.push_back(wires[idx].from->path);
    }
  }
  std::sort(floatMaps.rbegin(), floatMaps.rend());
  for (int i : floatMaps) {
    floatingMaps_.erase(floatingMaps_.begin() + i);
    if (i < static_cast<int>(floatingMapPos_.size())) floatingMapPos_.erase(floatingMapPos_.begin() + i);
  }
  std::sort(floatMtls.rbegin(), floatMtls.rend());
  for (int i : floatMtls) {
    floatingMtls_.erase(floatingMtls_.begin() + i);
    if (i < static_cast<int>(floatingMtlPos_.size())) floatingMtlPos_.erase(floatingMtlPos_.begin() + i);
  }
  if (root && (!remove.empty() || !detach.empty())) {
    doc_->commitSoftEdit();
    doc_->beginEdit(tr("Delete Nodes"));
    root = doc_->scene().findMaterial(materialId_);
    auto take = [&](const MtlPath& p, bool keepFloating) {
      if (p.empty()) return;
      MtlPath parentPath(p.begin(), p.end() - 1);
      MtlTarget parent = resolveMtlPath(root, parentPath);
      const MtlPathStep& last = p.back();
      if (last.kind == MtlPathStep::Kind::Sub && parent.material && last.sub < static_cast<int>(parent.material->subs.size())) {
        auto& slot = parent.material->subs[last.sub].material;
        if (keepFloating && slot) {
          floatingMtls_.push_back(std::move(slot));
          floatingMtlPos_.push_back(QPointF(-900, -300));
        }
        slot.reset();
      } else if (last.kind == MtlPathStep::Kind::Map) {
        MapSlots* slots = parent.material ? &parent.material->maps : (parent.map ? &parent.map->maps : nullptr);
        if (slots && slots->count(last.key)) {
          if (keepFloating && (*slots)[last.key].map) {
            floatingMaps_.push_back(std::move((*slots)[last.key].map));
            floatingMapPos_.push_back(QPointF(-900, 400));
          }
          slots->erase(last.key);
        }
      }
    };
    // Deepest first so parent removals do not invalidate child paths.
    auto byDepth = [](const MtlPath& a, const MtlPath& b) { return a.size() > b.size(); };
    std::sort(detach.begin(), detach.end(), byDepth);
    std::sort(remove.begin(), remove.end(), byDepth);
    for (const auto& p : detach) take(p, true);
    for (const auto& p : remove) take(p, false);
    doc_->notifyMaterialsChanged();
    doc_->endEdit();
  }
  rebuildGraph();
}

}  // namespace veraa
