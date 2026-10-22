// Track View: Curve Editor (function curves with tangent handles) and Dope Sheet (key grid),
// sharing a controller tree on the left.
#include <QAction>
#include <QActionGroup>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QScrollBar>
#include <QSplitter>
#include <QToolBar>
#include <QTreeWidget>
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
#include "theme.h"

namespace veraa {
namespace {

const QColor kBg(0x30, 0x30, 0x30);
const QColor kGridC(0x3c, 0x3c, 0x3c);
const QColor kGridMajor(0x48, 0x48, 0x48);
const QColor kLabel(0xa8, 0xa8, 0xa8);
const int kRuler = 20;
const int kValueGutter = 54;

using KeyRef = std::pair<std::string, double>;  // track path, key time

QColor channelColor(const std::string& path) {
  ValuePath p = ValuePath::parse(path);
  int comp = -1;
  if (p.kind == ValuePath::Kind::Transform) comp = p.channel % 3;
  else if (p.component >= 0) comp = p.component;
  switch (comp) {
    case 0: return QColor(0xe8, 0x50, 0x50);
    case 1: return QColor(0x5c, 0xd0, 0x5c);
    case 2: return QColor(0x5a, 0x8c, 0xf8);
    default: return QColor(0xe8, 0xd0, 0x60);
  }
}

struct Row {
  enum Kind { NodeSummary, Group, TrackRow, RotQ } kind = TrackRow;
  QTreeWidgetItem* item = nullptr;
  uint32_t node = 0;
  std::string path;                // TrackRow
  std::vector<std::string> paths;  // summaries
};

class TrackViewWindow;

class GraphView : public QWidget {
 public:
  GraphView(TrackViewWindow* w, AnimController* c);
  void frameAll();
  double t0 = -5, t1 = 105, v0 = -10, v1 = 110;
  bool addMode = false;
  std::set<KeyRef> sel;

 protected:
  void paintEvent(QPaintEvent*) override;
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;
  void wheelEvent(QWheelEvent* e) override;
  void keyPressEvent(QKeyEvent* e) override;
  void contextMenuEvent(QContextMenuEvent* e) override;

 public:
  // Mapping.
  double left() const;
  double xOf(double t) const { return left() + (t - t0) / std::max(1e-9, t1 - t0) * (width() - left()); }
  double tOf(double x) const { return t0 + (x - left()) / std::max(1.0, static_cast<double>(width()) - left()) * (t1 - t0); }
  double yOf(double v) const { return kRuler + (v1 - v) / std::max(1e-12, v1 - v0) * (height() - kRuler); }
  double vOf(double y) const { return v1 - (y - kRuler) / std::max(1.0, static_cast<double>(height() - kRuler)) * (v1 - v0); }

  void deleteSelected();
  void setTangents(TangentType t);
  void setOutOfRange(OutOfRange o);
  void copyKeys();
  void pasteKeys();

 private:
  bool curveMode() const;
  bool hitKey(const QPointF& p, KeyRef& out) const;
  int hitHandle(const QPointF& p, KeyRef& out) const;  // 0 none, 1 in, 2 out
  QPointF handlePos(const Track& t, size_t i, bool out) const;
  void selectInRect(const QRectF& r, bool add);
  std::vector<KeyRef> rowKeys(const Row& r) const;
  void beginDrag(const QPointF& p);

  TrackViewWindow* w_;
  AnimController* c_;
  enum class Drag { None, Keys, Handle, Band, Pan } drag_ = Drag::None;
  QPointF press_, last_;
  bool copyDrag_ = false, moved_ = false, breakHandle_ = false;
  KeyRef handleKey_;
  int handleSide_ = 0;
  std::map<std::string, Track> snapshot_;
  std::map<uint32_t, std::vector<RotationKey>> rotSnapshot_;
  std::map<std::string, std::vector<Key>> clipboard_;
};

class TrackViewWindow : public QWidget {
 public:
  TrackViewWindow(MainWindow& w, AnimController* c, int mode);
  int mode() const { return mode_; }
  void setMode(int m);
  AnimController* controller() const { return c_; }
  const std::vector<Row>& rows() const { return rows_; }
  // Tracks shown as curves (Curve Editor).
  std::vector<std::string> curvePaths() const;
  Track* trackFor(const std::string& path, bool create = false) const;
  QTreeWidget* tree() const { return tree_; }
  void rebuild();
  void updateKeyStats();
  GraphView* view() const { return view_; }

 private:
  AnimController* c_;
  int mode_;
  QTreeWidget* tree_;
  GraphView* view_;
  QCheckBox *allObjects_, *animatedOnly_;
  QDoubleSpinBox *keyTime_, *keyValue_;
  QLabel* status_;
  std::vector<Row> rows_;
  QAction *curveAct_, *dopeAct_, *addAct_;
  bool rebuilding_ = false;
};

// ---------------------------------------------------------------------------------------------

TrackViewWindow::TrackViewWindow(MainWindow& w, AnimController* c, int mode) : QWidget(&w, Qt::Window), c_(c), mode_(mode) {
  setObjectName("trackView");
  setAttribute(Qt::WA_DeleteOnClose);
  resize(1100, 520);
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(2, 2, 2, 2);
  v->setSpacing(2);
  auto* tb = new QToolBar(this);
  tb->setIconSize(QSize(20, 20));
  v->addWidget(tb);
  auto* modes = new QActionGroup(this);
  curveAct_ = tb->addAction(animicons::curveEditor(), tr("Curve Editor"));
  dopeAct_ = tb->addAction(animicons::dopeSheet(), tr("Dope Sheet"));
  for (QAction* a : {curveAct_, dopeAct_}) {
    a->setCheckable(true);
    modes->addAction(a);
  }
  connect(curveAct_, &QAction::triggered, this, [this] { setMode(0); });
  connect(dopeAct_, &QAction::triggered, this, [this] { setMode(1); });
  tb->addSeparator();
  QAction* moveAct = tb->addAction(animicons::moveKeys(), tr("Move Keys"));
  addAct_ = tb->addAction(animicons::addKey(), tr("Add/Remove Key"));
  auto* tools = new QActionGroup(this);
  for (QAction* a : {moveAct, addAct_}) {
    a->setCheckable(true);
    tools->addAction(a);
  }
  moveAct->setChecked(true);
  connect(moveAct, &QAction::triggered, this, [this] { view_->addMode = false; });
  connect(addAct_, &QAction::triggered, this, [this] { view_->addMode = true; });
  tb->addAction(animicons::frameAll(), tr("Frame All (Zoom Extents)"), this, [this] { view_->frameAll(); });
  tb->addSeparator();
  const char* tnames[] = {"Auto Tangents", "Smooth Tangents", "Linear Tangents", "Step Tangents", "Fast Tangents",
                          "Slow Tangents", "Custom Tangents"};
  for (int i = 0; i < 7; ++i)
    tb->addAction(animicons::tangent(i), tr(tnames[i]), this, [this, i] { view_->setTangents(static_cast<TangentType>(i)); });
  tb->addSeparator();
  const char* onames[] = {"Constant", "Cycle", "Loop", "Ping Pong", "Linear", "Relative Repeat"};
  for (int i = 0; i < 6; ++i)
    tb->addAction(animicons::outOfRange(i), tr("Out-of-Range: %1").arg(tr(onames[i])), this,
                  [this, i] { view_->setOutOfRange(static_cast<OutOfRange>(i)); });
  tb->addSeparator();
  tb->addAction(tr("Scale Keys..."), this, [this] {
    bool ok = false;
    double f = QInputDialog::getDouble(this, tr("Scale Keys"), tr("Scale selected key times around the current frame by:"), 1.0,
                                       0.01, 100.0, 3, &ok);
    if (!ok || view_->sel.empty()) return;
    double pivot = c_->frame();
    std::set<KeyRef> sel = view_->sel, next;
    c_->editAnimation(tr("Scale Keys"), [&] {
      for (const auto& [path, t] : sel) {
        Track* tr2 = trackFor(path);
        if (!tr2) continue;
        int i = tr2->keyIndexAt(t);
        if (i < 0) continue;
        double nt = std::round(pivot + (t - pivot) * f);
        tr2->keys[static_cast<size_t>(i)].time = nt;
        next.insert({path, nt});
      }
      for (const auto& [path, t] : sel)
        if (Track* tr2 = trackFor(path)) tr2->sortKeys();
    });
    view_->sel = next;
  });

  auto* split = new QSplitter(this);
  v->addWidget(split, 1);
  auto* left = new QWidget(split);
  auto* lv = new QVBoxLayout(left);
  lv->setContentsMargins(0, 0, 0, 0);
  lv->setSpacing(0);
  auto* opts = new QHBoxLayout;
  allObjects_ = new QCheckBox(tr("All objects"), left);
  animatedOnly_ = new QCheckBox(tr("Animated only"), left);
  opts->addWidget(allObjects_);
  opts->addWidget(animatedOnly_);
  lv->addLayout(opts);
  auto* spacer = new QLabel(tr("  Controllers"), left);
  spacer->setFixedHeight(kRuler);
  spacer->setStyleSheet("color: #9a9a9a;");
  lv->addWidget(spacer);
  tree_ = new QTreeWidget(left);
  tree_->setObjectName("trackViewTree");
  tree_->setHeaderHidden(true);
  tree_->setFrameShape(QFrame::NoFrame);
  tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  tree_->setUniformRowHeights(true);
  lv->addWidget(tree_, 1);
  view_ = new GraphView(this, c);
  split->addWidget(left);
  split->addWidget(view_);
  split->setStretchFactor(1, 1);
  split->setSizes({260, 840});

  auto* bottom = new QHBoxLayout;
  status_ = new QLabel(this);
  keyTime_ = new QDoubleSpinBox(this);
  keyTime_->setRange(-1e6, 1e6);
  keyTime_->setDecimals(1);
  keyTime_->setKeyboardTracking(false);
  keyValue_ = new QDoubleSpinBox(this);
  keyValue_->setRange(-1e9, 1e9);
  keyValue_->setDecimals(3);
  keyValue_->setKeyboardTracking(false);
  bottom->addWidget(status_, 1);
  bottom->addWidget(new QLabel(tr("Key time:"), this));
  bottom->addWidget(keyTime_);
  bottom->addWidget(new QLabel(tr("Value:"), this));
  bottom->addWidget(keyValue_);
  v->addLayout(bottom);

  connect(keyTime_, &QDoubleSpinBox::valueChanged, this, [this](double t) {
    if (view_->sel.size() != 1) return;
    KeyRef k = *view_->sel.begin();
    if (std::fabs(k.second - t) < 1e-9) return;
    c_->editAnimation(tr("Key Time"), [&] {
      Track* tr2 = trackFor(k.first);
      int i = tr2 ? tr2->keyIndexAt(k.second) : -1;
      if (i < 0) return;
      tr2->deleteKeyAt(t);
      i = tr2->keyIndexAt(k.second);
      tr2->keys[static_cast<size_t>(i)].time = t;
      tr2->sortKeys();
    });
    view_->sel = {{k.first, t}};
    view_->update();
  });
  connect(keyValue_, &QDoubleSpinBox::valueChanged, this, [this](double val) {
    if (view_->sel.empty()) return;
    std::set<KeyRef> sel = view_->sel;
    c_->editAnimation(tr("Key Value"), [&] {
      for (const auto& [path, t] : sel)
        if (Track* tr2 = trackFor(path)) {
          int i = tr2->keyIndexAt(t);
          if (i >= 0) tr2->keys[static_cast<size_t>(i)].value = val;
        }
    });
  });
  connect(tree_, &QTreeWidget::itemSelectionChanged, this, [this] {
    if (!rebuilding_) view_->update();
  });
  connect(tree_, &QTreeWidget::itemExpanded, view_, [this] { view_->update(); });
  connect(tree_, &QTreeWidget::itemCollapsed, view_, [this] { view_->update(); });
  connect(tree_->verticalScrollBar(), &QScrollBar::valueChanged, view_, [this] { view_->update(); });
  connect(allObjects_, &QCheckBox::toggled, this, [this] { rebuild(); });
  connect(animatedOnly_, &QCheckBox::toggled, this, [this] { rebuild(); });
  connect(c->document(), &SceneDocument::selectionChanged, this, [this] {
    if (!allObjects_->isChecked()) rebuild();
  });
  connect(c->document(), &SceneDocument::structureChanged, this, [this] { rebuild(); });
  connect(c, &AnimController::animationChanged, this, [this] {
    view_->update();
    updateKeyStats();
  });
  connect(c, &AnimController::frameChanged, view_, [this] { view_->update(); });
  setMode(mode);
  rebuild();
  view_->frameAll();
}

void TrackViewWindow::setMode(int m) {
  mode_ = m;
  curveAct_->setChecked(m == 0);
  dopeAct_->setChecked(m == 1);
  setWindowTitle(m == 0 ? tr("Track View - Curve Editor") : tr("Track View - Dope Sheet"));
  view_->update();
}

Track* TrackViewWindow::trackFor(const std::string& path, bool create) const {
  Animation* anim = c_->animation();
  if (!anim) return nullptr;
  if (anim->activeLayer > 0 && anim->activeLayer <= static_cast<int>(anim->layers.size())) {
    auto& L = anim->layers[static_cast<size_t>(anim->activeLayer - 1)].tracks;
    auto it = L.find(path);
    if (it != L.end()) return &it->second;
  }
  if (Track* t = anim->findTrack(path)) return t;
  return create ? &anim->activeTrack(path) : nullptr;
}

void TrackViewWindow::rebuild() {
  rebuilding_ = true;
  Animation* anim = c_->animation();
  // Remember expansion by label path.
  std::set<QString> expanded, selected;
  std::function<void(QTreeWidgetItem*, const QString&)> remember = [&](QTreeWidgetItem* it, const QString& prefix) {
    QString key = prefix + "/" + it->text(0);
    if (it->isExpanded()) expanded.insert(key);
    if (it->isSelected()) selected.insert(key);
    for (int i = 0; i < it->childCount(); ++i) remember(it->child(i), key);
  };
  for (int i = 0; i < tree_->topLevelItemCount(); ++i) remember(tree_->topLevelItem(i), QString());
  bool firstBuild = tree_->topLevelItemCount() == 0;
  tree_->clear();
  rows_.clear();
  if (!anim) {
    rebuilding_ = false;
    return;
  }
  const Scene& s = c_->scene();
  std::vector<const Node*> nodes;
  if (allObjects_->isChecked()) {
    for (const auto& up : s.nodes) nodes.push_back(up.get());
  } else {
    for (Node* n : c_->document()->selectedNodes()) nodes.push_back(n);
  }
  bool onlyAnimated = animatedOnly_->isChecked();
  for (const Node* n : nodes) {
    std::vector<std::string> all = anim->animatablePaths(s, n->id);
    std::vector<std::string> paths;
    for (const auto& p : all) {
      ValuePath vp = ValuePath::parse(p);
      bool animated = trackFor(p) != nullptr;
      if (onlyAnimated && !animated) continue;
      if (!animated && vp.kind != ValuePath::Kind::Transform) continue;  // keep the tree readable
      paths.push_back(p);
    }
    auto na = anim->nodes.find(n->id);
    bool tcb = na != anim->nodes.end() && na->second.rotationController == 1;
    if (onlyAnimated && paths.empty() && !tcb) continue;
    auto* ni = new QTreeWidgetItem(tree_, {QString::fromStdString(n->name)});
    ni->setForeground(0, QColor(0xf0, 0xf0, 0xf0));
    Row nr;
    nr.kind = Row::NodeSummary;
    nr.item = ni;
    nr.node = n->id;
    nr.paths = paths;
    rows_.push_back(nr);
    std::map<QString, QTreeWidgetItem*> groups;
    for (const auto& p : paths) {
      ValuePath vp = ValuePath::parse(p);
      QString group;
      switch (vp.kind) {
        case ValuePath::Kind::Transform: group = vp.channel < 3 ? tr("Position") : vp.channel < 6 ? tr("Rotation") : tr("Scale"); break;
        case ValuePath::Kind::ObjectParam: group = tr("Object"); break;
        case ValuePath::Kind::ModifierParam:
          group = QString::fromStdString(n->modifiers.size() > static_cast<size_t>(vp.index) && n->modifiers[vp.index]
                                             ? modifierDisplayName(n->modifiers[vp.index]->type())
                                             : std::string("Modifier"));
          break;
        default: group = tr("Other"); break;
      }
      if (tcb && vp.kind == ValuePath::Kind::Transform && vp.channel >= 3 && vp.channel < 6) continue;
      QTreeWidgetItem*& g = groups[group];
      if (!g) {
        g = new QTreeWidgetItem(ni, {group});
        Row gr;
        gr.kind = Row::Group;
        gr.item = g;
        gr.node = n->id;
        rows_.push_back(gr);
      }
      for (auto& r : rows_)
        if (r.item == g) r.paths.push_back(p);
      std::string label = Animation::displayName(s, p);
      size_t cut = label.rfind(" > ");
      QString shortLabel = QString::fromStdString(cut == std::string::npos ? label : label.substr(cut + 3));
      const Track* t = trackFor(p);
      auto* ti = new QTreeWidgetItem(g, {QStringLiteral("%1 : %2").arg(shortLabel, controllerName(t))});
      ti->setForeground(0, channelColor(p));
      Row tr2;
      tr2.kind = Row::TrackRow;
      tr2.item = ti;
      tr2.node = n->id;
      tr2.path = p;
      rows_.push_back(tr2);
    }
    if (tcb) {
      auto* ri = new QTreeWidgetItem(ni, {tr("Rotation : TCB Rotation")});
      Row rr;
      rr.kind = Row::RotQ;
      rr.item = ri;
      rr.node = n->id;
      rr.path = "rotq:" + std::to_string(n->id);
      rows_.push_back(rr);
    }
  }
  // Restore expansion (default: expanded).
  std::function<void(QTreeWidgetItem*, const QString&)> restore = [&](QTreeWidgetItem* it, const QString& prefix) {
    QString key = prefix + "/" + it->text(0);
    it->setExpanded(firstBuild || expanded.count(key) || it->childCount() < 12);
    if (selected.count(key)) it->setSelected(true);
    for (int i = 0; i < it->childCount(); ++i) restore(it->child(i), key);
  };
  for (int i = 0; i < tree_->topLevelItemCount(); ++i) restore(tree_->topLevelItem(i), QString());
  // Drop selected keys that no longer exist.
  std::set<KeyRef> keep;
  for (const auto& k : view_->sel)
    if (Track* t = trackFor(k.first); t && t->keyIndexAt(k.second) >= 0) keep.insert(k);
  view_->sel = keep;
  rebuilding_ = false;
  view_->update();
  updateKeyStats();
}

std::vector<std::string> TrackViewWindow::curvePaths() const {
  std::vector<std::string> out;
  std::set<std::string> seen;
  bool any = !tree_->selectedItems().isEmpty();
  for (const Row& r : rows_) {
    if (any && !r.item->isSelected()) continue;
    std::vector<std::string> ps = r.kind == Row::TrackRow ? std::vector<std::string>{r.path} : r.paths;
    for (const auto& p : ps)
      if (seen.insert(p).second && trackFor(p)) out.push_back(p);
  }
  return out;
}

void TrackViewWindow::updateKeyStats() {
  const auto& sel = view_->sel;
  QSignalBlocker b1(keyTime_), b2(keyValue_);
  if (sel.empty()) {
    status_->setText(tr("No keys selected"));
    keyTime_->setEnabled(false);
    keyValue_->setEnabled(false);
    return;
  }
  const KeyRef& k = *sel.begin();
  Track* t = trackFor(k.first);
  int i = t ? t->keyIndexAt(k.second) : -1;
  keyTime_->setEnabled(sel.size() == 1 && i >= 0);
  keyValue_->setEnabled(i >= 0);
  keyTime_->setValue(k.second);
  if (i >= 0) keyValue_->setValue(t->keys[static_cast<size_t>(i)].value);
  status_->setText(tr("%n key(s) selected", "", static_cast<int>(sel.size())));
}

// ---------------------------------------------------------------------------------------------

GraphView::GraphView(TrackViewWindow* w, AnimController* c) : QWidget(w), w_(w), c_(c) {
  setObjectName("trackViewGraph");
  setMouseTracking(true);
  setFocusPolicy(Qt::ClickFocus);
  setMinimumSize(300, 200);
}

bool GraphView::curveMode() const { return w_->mode() == 0; }
double GraphView::left() const { return curveMode() ? kValueGutter : 0.0; }

void GraphView::frameAll() {
  double a = 1e30, b = -1e30, lo = 1e30, hi = -1e30;
  TrackEnv env = c_->animation() ? c_->animation()->env(c_->scene()) : TrackEnv();
  for (const auto& p : w_->curvePaths()) {
    Track* t = w_->trackFor(p);
    double k0, k1;
    if (t->keyRange(k0, k1)) {
      a = std::min(a, k0);
      b = std::max(b, k1);
    }
  }
  if (a > b) {
    a = c_->startFrame();
    b = c_->endFrame();
  }
  if (b - a < 1) b = a + 10;
  for (const auto& p : w_->curvePaths()) {
    Track* t = w_->trackFor(p);
    for (int i = 0; i <= 200; ++i) {
      double v = t->evaluate(a + (b - a) * i / 200.0, &env);
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
  }
  if (lo > hi) {
    lo = 0;
    hi = 100;
  }
  if (hi - lo < 1e-6) {
    lo -= 10;
    hi += 10;
  }
  double pt = (b - a) * 0.05, pv = (hi - lo) * 0.08;
  t0 = a - pt;
  t1 = b + pt;
  v0 = lo - pv;
  v1 = hi + pv;
  update();
}

std::vector<KeyRef> GraphView::rowKeys(const Row& r) const {
  std::vector<KeyRef> out;
  if (r.kind == Row::TrackRow) {
    if (Track* t = w_->trackFor(r.path))
      for (const auto& k : t->keys) out.push_back({r.path, k.time});
  } else if (r.kind == Row::RotQ) {
    auto na = c_->animation()->nodes.find(r.node);
    if (na != c_->animation()->nodes.end())
      for (const auto& k : na->second.rotationKeys) out.push_back({r.path, k.time});
  } else {
    for (const auto& p : r.paths)
      if (Track* t = w_->trackFor(p))
        for (const auto& k : t->keys) out.push_back({p, k.time});
  }
  return out;
}

QPointF GraphView::handlePos(const Track& t, size_t i, bool out) const {
  const Key& k = t.keys[i];
  double in, outS;
  t.slopes(i, in, outS);
  double dt;
  if (out) dt = i + 1 < t.keys.size() ? (t.keys[i + 1].time - k.time) * k.outLength : 10.0 / 3.0;
  else dt = i > 0 ? (k.time - t.keys[i - 1].time) * k.inLength : 10.0 / 3.0;
  double slope = out ? outS : in;
  double sign = out ? 1.0 : -1.0;
  return QPointF(xOf(k.time + sign * dt), yOf(k.value + sign * slope * dt));
}

void GraphView::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  p.fillRect(rect(), kBg);
  Animation* anim = c_->animation();
  if (!anim) return;
  TrackEnv env = anim->env(c_->scene());
  // Active range shading.
  p.fillRect(QRectF(xOf(c_->startFrame()), kRuler, xOf(c_->endFrame()) - xOf(c_->startFrame()), height()), QColor(0x36, 0x36, 0x36));
  // Time grid.
  double pxPerFrame = (width() - left()) / std::max(1e-9, t1 - t0);
  double step = 1;
  for (double s : {1.0, 2.0, 5.0, 10.0, 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 5000.0}) {
    step = s;
    if (s * pxPerFrame >= 40) break;
  }
  p.setFont(QFont(font().family(), 8));
  for (double t = std::ceil(t0 / step) * step; t <= t1; t += step) {
    double x = xOf(t);
    p.setPen(kGridMajor);
    p.drawLine(QPointF(x, kRuler), QPointF(x, height()));
    p.setPen(kLabel);
    p.drawText(QRectF(x - 30, 2, 60, kRuler - 4), Qt::AlignCenter, QString::number(t));
  }
  if (curveMode()) {
    double range = v1 - v0, vs = 1;
    double target = range / std::max(1.0, (height() - kRuler) / 40.0);
    double mag = std::pow(10.0, std::floor(std::log10(std::max(target, 1e-9))));
    for (double m : {1.0, 2.0, 5.0, 10.0}) {
      vs = m * mag;
      if (vs >= target) break;
    }
    for (double v = std::ceil(v0 / vs) * vs; v <= v1; v += vs) {
      double y = yOf(v);
      p.setPen(std::fabs(v) < vs * 1e-6 ? QColor(0x70, 0x70, 0x70) : kGridC);
      p.drawLine(QPointF(left(), y), QPointF(width(), y));
      p.setPen(kLabel);
      p.drawText(QRectF(0, y - 8, kValueGutter - 4, 16), Qt::AlignRight | Qt::AlignVCenter, QString::number(v, 'g', 5));
    }
    // Curves.
    for (const auto& path : w_->curvePaths()) {
      Track* t = w_->trackFor(path);
      QColor col = channelColor(path);
      double k0 = 0, k1 = 0;
      bool hasRange = t->keyRange(k0, k1);
      QPainterPath inRange, outRange;
      bool startedIn = false, startedOut = false;
      for (int x = static_cast<int>(left()); x <= width(); x += 2) {
        double tt = tOf(x);
        double v = t->evaluate(tt, &env);
        QPointF pt(x, yOf(v));
        bool inside = !hasRange || (tt >= k0 && tt <= k1);
        QPainterPath& path2 = inside ? inRange : outRange;
        bool& started = inside ? startedIn : startedOut;
        if (!started) {
          path2.moveTo(pt);
          started = true;
        } else {
          path2.lineTo(pt);
        }
        // Break the other path so it does not bridge across.
        if (inside) startedOut = false;
        else startedIn = false;
      }
      p.setBrush(Qt::NoBrush);
      p.setPen(QPen(col, 1.6));
      p.drawPath(inRange);
      QColor dim = col;
      dim.setAlphaF(0.55f);
      p.setPen(QPen(dim, 1.2, Qt::DashLine));
      p.drawPath(outRange);
      // Keys and handles.
      for (size_t i = 0; i < t->keys.size(); ++i) {
        const Key& k = t->keys[i];
        QPointF kp(xOf(k.time), yOf(k.value));
        bool selected = sel.count({path, k.time}) > 0;
        if (selected && t->type == ControllerType::Bezier) {
          for (bool out : {false, true}) {
            QPointF h = handlePos(*t, i, out);
            p.setPen(QPen(QColor(0xc0, 0xc0, 0xc0), 1));
            p.drawLine(kp, h);
            p.setBrush(QColor(0x30, 0x30, 0x30));
            p.drawEllipse(h, 3.5, 3.5);
          }
        }
        p.setPen(QPen(Qt::black, 1));
        p.setBrush(selected ? QColor(Qt::white) : QColor(0x20, 0x20, 0x20));
        p.drawRect(QRectF(kp.x() - 3.5, kp.y() - 3.5, 7, 7));
      }
    }
  } else {
    // Dope sheet rows aligned with the tree.
    QTreeWidget* tree = w_->tree();
    for (const Row& r : w_->rows()) {
      QRect ir = tree->visualItemRect(r.item);
      if (!ir.isValid() || ir.height() == 0) continue;
      double y = kRuler + ir.top(), h = ir.height();
      if (y + h < kRuler || y > height()) continue;
      QColor rowBg = r.kind == Row::NodeSummary ? QColor(0x3a, 0x3a, 0x40) : r.kind == Row::Group ? QColor(0x35, 0x35, 0x38) : kBg;
      p.fillRect(QRectF(0, y, width(), h), rowBg);
      p.setPen(kGridC);
      p.drawLine(QPointF(0, y + h), QPointF(width(), y + h));
      // Range bar.
      std::vector<KeyRef> keys = rowKeys(r);
      if (keys.empty()) continue;
      double a = 1e30, b = -1e30;
      for (const auto& k : keys) {
        a = std::min(a, k.second);
        b = std::max(b, k.second);
      }
      p.fillRect(QRectF(xOf(a), y + h * 0.42, xOf(b) - xOf(a), h * 0.16), QColor(0x50, 0x50, 0x58));
      std::set<double> drawn;
      for (const auto& k : keys) {
        bool selected = r.kind == Row::TrackRow || r.kind == Row::RotQ ? sel.count(k) > 0 : false;
        if (r.kind != Row::TrackRow && r.kind != Row::RotQ) {
          if (!drawn.insert(k.second).second) continue;
          for (const auto& k2 : keys)
            if (k2.second == k.second && sel.count(k2)) selected = true;
        }
        QRectF box(xOf(k.second) - 3, y + 3, 6, h - 6);
        QColor col = r.kind == Row::TrackRow ? channelColor(r.path) : QColor(0xb8, 0xb8, 0xb8);
        p.setPen(selected ? QPen(Qt::white, 1.5) : QPen(Qt::black, 1));
        p.setBrush(selected ? QColor(Qt::white) : col);
        p.drawRect(box);
      }
    }
  }
  // Current frame.
  double cx = xOf(c_->frame());
  p.setPen(QPen(c_->autoKey() ? QColor(0xd0, 0x30, 0x30) : theme::accent(), 1.5));
  p.drawLine(QPointF(cx, 0), QPointF(cx, height()));
  // Band selection.
  if (drag_ == Drag::Band) {
    p.setPen(QPen(Qt::white, 1, Qt::DashLine));
    p.setBrush(QColor(255, 255, 255, 25));
    p.drawRect(QRectF(press_, last_).normalized());
  }
}

bool GraphView::hitKey(const QPointF& pos, KeyRef& out) const {
  double best = 7.0;
  bool found = false;
  if (curveMode()) {
    for (const auto& path : w_->curvePaths()) {
      Track* t = w_->trackFor(path);
      for (const auto& k : t->keys) {
        double d = std::hypot(xOf(k.time) - pos.x(), yOf(k.value) - pos.y());
        if (d < best) {
          best = d;
          out = {path, k.time};
          found = true;
        }
      }
    }
    return found;
  }
  QTreeWidget* tree = w_->tree();
  for (const Row& r : w_->rows()) {
    QRect ir = tree->visualItemRect(r.item);
    if (!ir.isValid() || ir.height() == 0) continue;
    double y = kRuler + ir.top();
    if (pos.y() < y || pos.y() > y + ir.height()) continue;
    for (const auto& k : rowKeys(r)) {
      double d = std::fabs(xOf(k.second) - pos.x());
      if (d < best) {
        best = d;
        out = k;
        found = true;
      }
    }
    if (found && r.kind != Row::TrackRow && r.kind != Row::RotQ) out.first = "summary:" + std::to_string(reinterpret_cast<uintptr_t>(r.item));
    return found;
  }
  return false;
}

int GraphView::hitHandle(const QPointF& pos, KeyRef& out) const {
  if (!curveMode()) return 0;
  for (const auto& k : sel) {
    Track* t = w_->trackFor(k.first);
    if (!t || t->type != ControllerType::Bezier) continue;
    int i = t->keyIndexAt(k.second);
    if (i < 0) continue;
    for (int side = 1; side <= 2; ++side) {
      QPointF h = handlePos(*t, static_cast<size_t>(i), side == 2);
      if (std::hypot(h.x() - pos.x(), h.y() - pos.y()) < 6) {
        out = k;
        return side;
      }
    }
  }
  return 0;
}

void GraphView::selectInRect(const QRectF& r, bool add) {
  if (!add) sel.clear();
  if (curveMode()) {
    for (const auto& path : w_->curvePaths())
      for (const auto& k : w_->trackFor(path)->keys)
        if (r.contains(xOf(k.time), yOf(k.value))) sel.insert({path, k.time});
  } else {
    QTreeWidget* tree = w_->tree();
    for (const Row& row : w_->rows()) {
      if (row.kind != Row::TrackRow && row.kind != Row::RotQ) continue;
      QRect ir = tree->visualItemRect(row.item);
      if (!ir.isValid() || ir.height() == 0) continue;
      double y = kRuler + ir.center().y();
      for (const auto& k : rowKeys(row))
        if (r.contains(xOf(k.second), y)) sel.insert(k);
    }
  }
  w_->updateKeyStats();
}

void GraphView::beginDrag(const QPointF& p) {
  Animation* anim = c_->animation();
  snapshot_.clear();
  rotSnapshot_.clear();
  for (const auto& k : sel) {
    if (k.first.rfind("rotq:", 0) == 0) {
      uint32_t id = static_cast<uint32_t>(std::stoul(k.first.substr(5)));
      rotSnapshot_[id] = anim->nodes[id].rotationKeys;
    } else if (Track* t = w_->trackFor(k.first)) {
      snapshot_[k.first] = *t;
    }
  }
  press_ = last_ = p;
  moved_ = false;
  c_->beginAnimationEdit(copyDrag_ ? tr("Copy Keys") : tr("Move Keys"));
}

void GraphView::mousePressEvent(QMouseEvent* e) {
  setFocus();
  QPointF pos = e->position();
  if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && e->modifiers() & Qt::AltModifier)) {
    drag_ = Drag::Pan;
    press_ = last_ = pos;
    return;
  }
  if (e->button() != Qt::LeftButton) return;
  Animation* anim = c_->animation();
  if (!anim) return;
  if (pos.y() < kRuler) {  // ruler: scrub time
    c_->setFrame(std::round(tOf(pos.x())));
    drag_ = Drag::None;
    return;
  }
  KeyRef k;
  int side = hitHandle(pos, k);
  if (side) {
    drag_ = Drag::Handle;
    handleKey_ = k;
    handleSide_ = side;
    breakHandle_ = e->modifiers() & Qt::ShiftModifier;
    snapshot_.clear();
    snapshot_[k.first] = *w_->trackFor(k.first);
    c_->beginAnimationEdit(tr("Edit Tangent"));
    return;
  }
  bool ctrl = e->modifiers() & Qt::ControlModifier;
  if (addMode) {
    // Add a key on the curve / row under the cursor.
    double t = std::round(tOf(pos.x()));
    std::string path;
    if (curveMode()) {
      TrackEnv env = anim->env(c_->scene());
      double best = 8.0;
      for (const auto& p2 : w_->curvePaths()) {
        double d = std::fabs(yOf(w_->trackFor(p2)->evaluate(t, &env)) - pos.y());
        if (d < best) {
          best = d;
          path = p2;
        }
      }
    } else {
      for (const Row& r : w_->rows()) {
        QRect ir = w_->tree()->visualItemRect(r.item);
        if (r.kind == Row::TrackRow && ir.isValid() && pos.y() >= kRuler + ir.top() && pos.y() <= kRuler + ir.bottom()) path = r.path;
      }
    }
    if (path.empty()) return;
    double value = anim->valueAt(c_->scene(), path, t);
    c_->editAnimation(tr("Add Key"), [&] { w_->trackFor(path, true)->setKey(t, value); });
    sel = {{path, t}};
    w_->updateKeyStats();
    return;
  }
  if (hitKey(pos, k)) {
    std::vector<KeyRef> group;
    if (k.first.rfind("summary:", 0) == 0) {
      for (const Row& r : w_->rows())
        if ("summary:" + std::to_string(reinterpret_cast<uintptr_t>(r.item)) == k.first)
          for (const auto& rk : rowKeys(r))
            if (rk.second == k.second) group.push_back(rk);
    } else {
      group.push_back(k);
    }
    bool already = !group.empty() && sel.count(group.front());
    if (ctrl) {
      for (const auto& g : group) {
        if (already) sel.erase(g);
        else sel.insert(g);
      }
    } else if (!already) {
      sel = std::set<KeyRef>(group.begin(), group.end());
    }
    w_->updateKeyStats();
    copyDrag_ = e->modifiers() & Qt::ShiftModifier && !curveMode();
    drag_ = Drag::Keys;
    beginDrag(pos);
    update();
    return;
  }
  drag_ = Drag::Band;
  press_ = last_ = pos;
  if (!ctrl) sel.clear();
  update();
}

void GraphView::mouseMoveEvent(QMouseEvent* e) {
  QPointF pos = e->position();
  Animation* anim = c_->animation();
  switch (drag_) {
    case Drag::Pan: {
      double dt = (tOf(pos.x()) - tOf(last_.x()));
      double dv = (vOf(pos.y()) - vOf(last_.y()));
      t0 -= dt;
      t1 -= dt;
      if (curveMode()) {
        v0 -= dv;
        v1 -= dv;
      }
      last_ = pos;
      update();
      break;
    }
    case Drag::Band:
      last_ = pos;
      update();
      break;
    case Drag::Handle: {
      Track* t = w_->trackFor(handleKey_.first);
      if (!t) break;
      *t = snapshot_[handleKey_.first];
      int i = t->keyIndexAt(handleKey_.second);
      if (i < 0) break;
      Key& k = t->keys[static_cast<size_t>(i)];
      double dt = tOf(pos.x()) - k.time, dv = vOf(pos.y()) - k.value;
      if (handleSide_ == 1) dt = std::min(dt, -1e-3);
      else dt = std::max(dt, 1e-3);
      double slope = dv / dt;
      double in, out;
      t->slopes(static_cast<size_t>(i), in, out);
      if (!breakHandle_) {
        k.inType = k.outType = TangentType::Custom;
        k.inSlope = k.outSlope = slope;
      } else if (handleSide_ == 1) {
        k.inType = k.outType = TangentType::Custom;
        k.inSlope = slope;
        k.outSlope = out;
      } else {
        k.inType = k.outType = TangentType::Custom;
        k.outSlope = slope;
        k.inSlope = in;
      }
      c_->previewAnimation();
      update();
      break;
    }
    case Drag::Keys: {
      if (!anim) break;
      double dt = std::round(tOf(pos.x()) - tOf(press_.x()));
      double dv = curveMode() ? vOf(pos.y()) - vOf(press_.y()) : 0.0;
      if (curveMode() && (e->modifiers() & Qt::ShiftModifier)) {
        // Constrain to the dominant direction.
        if (std::fabs(pos.x() - press_.x()) > std::fabs(pos.y() - press_.y())) dv = 0;
        else dt = 0;
      }
      moved_ = dt != 0 || std::fabs(dv) > 0;
      for (const auto& [path, orig] : snapshot_) {
        Track* t = w_->trackFor(path);
        if (!t) continue;
        *t = orig;
        std::vector<Key> movedKeys;
        for (auto it = t->keys.begin(); it != t->keys.end();) {
          if (sel.count({path, it->time})) {
            Key k = *it;
            k.time += dt;
            k.value += dv;
            movedKeys.push_back(k);
            if (!copyDrag_) {
              it = t->keys.erase(it);
              continue;
            }
          }
          ++it;
        }
        for (const auto& k : movedKeys) {
          t->deleteKeyAt(k.time);
          t->keys.push_back(k);
        }
        t->sortKeys();
      }
      for (const auto& [id, orig] : rotSnapshot_) {
        auto& rk = anim->nodes[id].rotationKeys;
        rk = orig;
        std::string path = "rotq:" + std::to_string(id);
        std::vector<RotationKey> movedKeys;
        for (auto it = rk.begin(); it != rk.end();) {
          if (sel.count({path, it->time})) {
            RotationKey k = *it;
            k.time += dt;
            movedKeys.push_back(k);
            if (!copyDrag_) {
              it = rk.erase(it);
              continue;
            }
          }
          ++it;
        }
        for (const auto& k : movedKeys) {
          rk.erase(std::remove_if(rk.begin(), rk.end(), [&](const RotationKey& o) { return std::fabs(o.time - k.time) < 1e-3; }), rk.end());
          rk.push_back(k);
        }
        std::sort(rk.begin(), rk.end(), [](const RotationKey& a, const RotationKey& b) { return a.time < b.time; });
      }
      last_ = QPointF(dt, dv);
      c_->previewAnimation();
      update();
      break;
    }
    case Drag::None: {
      KeyRef k;
      setCursor(hitHandle(pos, k) || hitKey(pos, k) ? Qt::PointingHandCursor : Qt::ArrowCursor);
      break;
    }
  }
}

void GraphView::mouseReleaseEvent(QMouseEvent*) {
  switch (drag_) {
    case Drag::Band: selectInRect(QRectF(press_, last_).normalized(), false); break;
    case Drag::Handle: c_->endAnimationEdit(); break;
    case Drag::Keys: {
      double dt = moved_ ? last_.x() : 0.0;
      if (moved_) {
        std::set<KeyRef> next;
        for (const auto& k : sel) next.insert({k.first, k.second + dt});
        sel = next;
      }
      c_->endAnimationEdit();
      w_->updateKeyStats();
      break;
    }
    default: break;
  }
  drag_ = Drag::None;
  snapshot_.clear();
  rotSnapshot_.clear();
  update();
}

void GraphView::wheelEvent(QWheelEvent* e) {
  double f = e->angleDelta().y() > 0 ? 0.85 : 1.0 / 0.85;
  QPointF pos = e->position();
  bool timeOnly = e->modifiers() & Qt::ControlModifier || !curveMode();
  bool valueOnly = e->modifiers() & Qt::ShiftModifier;
  if (!valueOnly) {
    double t = tOf(pos.x());
    t0 = t + (t0 - t) * f;
    t1 = t + (t1 - t) * f;
  }
  if (!timeOnly && curveMode()) {
    double v = vOf(pos.y());
    v0 = v + (v0 - v) * f;
    v1 = v + (v1 - v) * f;
  }
  update();
}

void GraphView::deleteSelected() {
  if (sel.empty()) return;
  std::set<KeyRef> s = sel;
  Animation* anim = c_->animation();
  c_->editAnimation(tr("Delete Keys"), [&] {
    for (const auto& [path, t] : s) {
      if (path.rfind("rotq:", 0) == 0) {
        auto& rk = anim->nodes[static_cast<uint32_t>(std::stoul(path.substr(5)))].rotationKeys;
        rk.erase(std::remove_if(rk.begin(), rk.end(), [t = t](const RotationKey& k) { return std::fabs(k.time - t) < 1e-3; }), rk.end());
      } else if (Track* tr2 = w_->trackFor(path)) {
        tr2->deleteKeyAt(t);
      }
    }
  });
  sel.clear();
  w_->rebuild();
}

void GraphView::setTangents(TangentType type) {
  if (sel.empty()) return;
  std::set<KeyRef> s = sel;
  c_->editAnimation(tr("Set Tangents"), [&] {
    for (const auto& [path, t] : s)
      if (Track* tr2 = w_->trackFor(path)) {
        int i = tr2->keyIndexAt(t);
        if (i < 0) continue;
        double in, out;
        tr2->slopes(static_cast<size_t>(i), in, out);
        Key& k = tr2->keys[static_cast<size_t>(i)];
        k.inType = k.outType = type;
        k.inSlope = in;
        k.outSlope = out;
      }
  });
}

void GraphView::setOutOfRange(OutOfRange o) {
  std::set<std::string> paths;
  for (const auto& k : sel) paths.insert(k.first);
  if (paths.empty())
    for (const auto& p : w_->curvePaths()) paths.insert(p);
  c_->editAnimation(tr("Out-of-Range Type"), [&] {
    for (const auto& p : paths)
      if (Track* t = w_->trackFor(p)) t->before = t->after = o;
  });
}

void GraphView::copyKeys() {
  clipboard_.clear();
  double first = 1e30;
  for (const auto& k : sel) first = std::min(first, k.second);
  for (const auto& [path, t] : sel)
    if (Track* tr2 = w_->trackFor(path)) {
      int i = tr2->keyIndexAt(t);
      if (i < 0) continue;
      Key k = tr2->keys[static_cast<size_t>(i)];
      k.time -= first;
      clipboard_[path].push_back(k);
    }
}

void GraphView::pasteKeys() {
  if (clipboard_.empty()) return;
  double at = c_->frame();
  auto clip = clipboard_;
  std::set<KeyRef> next;
  c_->editAnimation(tr("Paste Keys"), [&] {
    for (const auto& [path, keys] : clip) {
      Track* t = w_->trackFor(path, true);
      for (Key k : keys) {
        k.time += at;
        k.selected = false;
        t->deleteKeyAt(k.time);
        t->keys.push_back(k);
        next.insert({path, k.time});
      }
      t->sortKeys();
    }
  });
  sel = next;
  w_->rebuild();
}

void GraphView::keyPressEvent(QKeyEvent* e) {
  if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) deleteSelected();
  else if (e->matches(QKeySequence::Copy)) copyKeys();
  else if (e->matches(QKeySequence::Paste)) pasteKeys();
  else if (e->key() == Qt::Key_Z && !(e->modifiers() & Qt::ControlModifier)) frameAll();
  else QWidget::keyPressEvent(e);
}

void GraphView::contextMenuEvent(QContextMenuEvent* e) {
  QMenu m(this);
  m.addAction(tr("Delete Keys"), this, [this] { deleteSelected(); });
  m.addAction(tr("Copy Keys"), this, [this] { copyKeys(); });
  m.addAction(tr("Paste Keys at Current Frame"), this, [this] { pasteKeys(); });
  m.addSeparator();
  QMenu* tan = m.addMenu(tr("Tangents"));
  const char* names[] = {"Auto", "Smooth", "Linear", "Step", "Fast", "Slow", "Custom"};
  for (int i = 0; i < 7; ++i) tan->addAction(animicons::tangent(i), tr(names[i]), this, [this, i] { setTangents(static_cast<TangentType>(i)); });
  QMenu* oor = m.addMenu(tr("Out-of-Range Type"));
  const char* on[] = {"Constant", "Cycle", "Loop", "Ping Pong", "Linear", "Relative Repeat"};
  for (int i = 0; i < 6; ++i) oor->addAction(animicons::outOfRange(i), tr(on[i]), this, [this, i] { setOutOfRange(static_cast<OutOfRange>(i)); });
  m.addSeparator();
  m.addAction(tr("Frame All"), this, [this] { frameAll(); });
  m.exec(e->globalPos());
}

QPointer<TrackViewWindow> g_windows[2];

}  // namespace

void openTrackView(MainWindow& w, int mode) {
  AnimController* c = AnimController::instance();
  if (!c) return;
  mode = mode == 1 ? 1 : 0;
  if (!g_windows[mode]) g_windows[mode] = new TrackViewWindow(w, c, mode);
  g_windows[mode]->show();
  g_windows[mode]->raise();
  g_windows[mode]->activateWindow();
}

namespace {

void installTrackView(MainWindow& w) {
  QMenu* graph = w.menuAt("Graph Editors");
  QAction* curve = graph->addAction(animicons::curveEditor(), QObject::tr("Track View - Curve Editor..."));
  QAction* dope = graph->addAction(animicons::dopeSheet(), QObject::tr("Track View - Dope Sheet..."));
  w.registerCommand("graph.curveEditor", curve);
  w.registerCommand("graph.dopeSheet", dope);
  QObject::connect(curve, &QAction::triggered, &w, [&w] { openTrackView(w, 0); });
  QObject::connect(dope, &QAction::triggered, &w, [&w] { openTrackView(w, 1); });
}

}  // namespace

VERAA_APP_EXTENSION(trackView, 303, installTrackView);

}  // namespace veraa
