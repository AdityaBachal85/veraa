#include "animbar.h"

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QRadioButton>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <cmath>
#include <map>

#include "animcontroller.h"
#include "animicons.h"
#include "extension.h"
#include "mainwindow.h"
#include "theme.h"

namespace veraa {
namespace {

const QColor kBarBg(0x2b, 0x2b, 0x2b);
const QColor kGroove(0x38, 0x38, 0x38);
const QColor kTick(0x7a, 0x7a, 0x7a);
const QColor kText(0xc8, 0xc8, 0xc8);
const QColor kAutoRed(0xc8, 0x2a, 0x2a);
const QColor kKeyPos(0xe0, 0x46, 0x46);
const QColor kKeyRot(0x52, 0xc0, 0x52);
const QColor kKeyScale(0x50, 0x80, 0xf0);
const QColor kKeyOther(0xb0, 0xb0, 0xb0);

int channelBits(const std::string& path) {
  ValuePath p = ValuePath::parse(path);
  if (p.kind != ValuePath::Kind::Transform) return 8;
  return p.channel < 3 ? 1 : p.channel < 6 ? 2 : 4;
}

}  // namespace

double TimeAxis::x(double f) const {
  double span = std::max(1e-9, end - start);
  return left + (f - start) / span * (right - left);
}

double TimeAxis::frame(double px) const {
  double w = std::max(1e-9, right - left);
  return start + (px - left) / w * (end - start);
}

TimeAxis timeAxisFor(const AnimController& c, int width) {
  TimeAxis a;
  a.start = c.startFrame();
  a.end = c.endFrame();
  a.left = TimeSlider::kArrow + TimeSlider::kThumb / 2.0;
  a.right = std::max(a.left + 10.0, width - TimeSlider::kArrow - TimeSlider::kThumb / 2.0);
  return a;
}

// ---------------------------------------------------------------------------------------------
// Time slider

TimeSlider::TimeSlider(AnimController* c, QWidget* parent) : QWidget(parent), c_(c) {
  setObjectName("timeSlider");
  setMinimumHeight(22);
  setMaximumHeight(22);
  setToolTip(tr("Time slider: drag to change the current frame. Use , and . to step."));
  connect(c, &AnimController::frameChanged, this, [this] { update(); });
  connect(c, &AnimController::rangeChanged, this, [this] { update(); });
  connect(c, &AnimController::autoKeyChanged, this, [this] { update(); });
  connect(c, &AnimController::timeDisplayChanged, this, [this] { update(); });
}

void TimeSlider::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  QRect r = rect();
  bool autoKey = c_->autoKey();
  p.fillRect(r, autoKey ? kAutoRed.darker(170) : kBarBg);
  QRectF groove(kArrow, 3, width() - 2 * kArrow, height() - 6);
  p.fillRect(groove, autoKey ? kAutoRed.darker(130) : kGroove);
  // Arrows.
  p.setPen(Qt::NoPen);
  p.setBrush(kText);
  double cy = height() / 2.0;
  QPolygonF la, ra;
  la << QPointF(5, cy) << QPointF(12, cy - 5) << QPointF(12, cy + 5);
  ra << QPointF(width() - 5, cy) << QPointF(width() - 12, cy - 5) << QPointF(width() - 12, cy + 5);
  p.drawPolygon(la);
  p.drawPolygon(ra);
  // Thumb.
  TimeAxis a = timeAxisFor(*c_, width());
  double cx = a.x(c_->frame());
  QRectF thumb(cx - kThumb / 2.0, 2, kThumb, height() - 4);
  p.setBrush(autoKey ? kAutoRed : QColor(0x5a, 0x5a, 0x5a));
  p.setPen(QColor(0x90, 0x90, 0x90));
  p.drawRoundedRect(thumb, 2, 2);
  p.setPen(Qt::white);
  QFont f = font();
  f.setPointSizeF(std::max(7.0, f.pointSizeF() - 0.5));
  p.setFont(f);
  p.drawText(thumb, Qt::AlignCenter, QStringLiteral("%1 / %2").arg(c_->formatTime(c_->frame()), c_->formatTime(c_->endFrame())));
}

void TimeSlider::mousePressEvent(QMouseEvent* e) {
  if (e->button() != Qt::LeftButton) return;
  double x = e->position().x();
  if (x < kArrow) {
    c_->stepFrame(-1);
    return;
  }
  if (x > width() - kArrow) {
    c_->stepFrame(1);
    return;
  }
  TimeAxis a = timeAxisFor(*c_, width());
  double cx = a.x(c_->frame());
  dragging_ = true;
  grabOffset_ = std::fabs(x - cx) <= kThumb / 2.0 ? x - cx : 0.0;
  c_->stop();
  c_->setFrame(std::round(a.frame(x - grabOffset_)));
}

void TimeSlider::mouseMoveEvent(QMouseEvent* e) {
  if (!dragging_) return;
  TimeAxis a = timeAxisFor(*c_, width());
  c_->setFrame(std::round(a.frame(e->position().x() - grabOffset_)));
}

void TimeSlider::mouseReleaseEvent(QMouseEvent*) { dragging_ = false; }

void TimeSlider::wheelEvent(QWheelEvent* e) {
  int steps = e->angleDelta().y() > 0 ? 1 : e->angleDelta().y() < 0 ? -1 : 0;
  if (steps) c_->stepFrame(steps);
}

// ---------------------------------------------------------------------------------------------
// Track bar

TrackBar::TrackBar(AnimController* c, QWidget* parent) : QWidget(parent), c_(c) {
  setObjectName("trackBar");
  setMinimumHeight(30);
  setMaximumHeight(30);
  setFocusPolicy(Qt::ClickFocus);
  setToolTip(tr("Track bar: keys of the selection. Drag keys to move them (Shift+drag copies), "
                "Delete removes them, right-click for more."));
  auto up = [this] { update(); };
  connect(c, &AnimController::frameChanged, this, up);
  connect(c, &AnimController::rangeChanged, this, up);
  connect(c, &AnimController::animationChanged, this, up);
  connect(c, &AnimController::trackBarSourceChanged, this, up);
  connect(c->document(), &SceneDocument::selectionChanged, this, [this] {
    selected_.clear();
    update();
  });
}

bool TrackBar::pathPassesFilter(const std::string& path) const {
  int bits = channelBits(path);
  switch (filter) {
    case 1: return bits != 8;
    case 2: return bits == 1;
    case 3: return bits == 2;
    case 4: return bits == 4;
    default: return true;
  }
}

std::vector<std::pair<double, int>> TrackBar::keyMarks() const {
  std::map<double, int> marks;
  Animation* anim = c_->animation();
  if (!anim) return {};
  auto addTrack = [&](const std::string& path, const Track& t) {
    if (!pathPassesFilter(path)) return;
    std::vector<double> times;
    t.keyTimes(times);
    for (double k : times) marks[std::round(k * 1000.0) / 1000.0] |= channelBits(path);
  };
  for (uint32_t id : c_->trackBarNodes()) {
    for (const auto& path : anim->nodeTrackPaths(id)) {
      if (const Track* t = anim->findTrack(path)) addTrack(path, *t);
      for (const auto& L : anim->layers) {
        auto it = L.tracks.find(path);
        if (it != L.tracks.end()) addTrack(path, it->second);
      }
    }
    auto na = anim->nodes.find(id);
    if (na != anim->nodes.end() && na->second.rotationController == 1 && filter != 2 && filter != 4)
      for (const auto& k : na->second.rotationKeys) marks[std::round(k.time * 1000.0) / 1000.0] |= 2;
  }
  return std::vector<std::pair<double, int>>(marks.begin(), marks.end());
}

void TrackBar::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.fillRect(rect(), kBarBg);
  TimeAxis a = timeAxisFor(*c_, width());
  // Ruler.
  double span = std::max(1.0, a.end - a.start);
  double pxPerFrame = (a.right - a.left) / span;
  int minorStep = 1;
  for (int s : {1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000}) {
    minorStep = s;
    if (s * pxPerFrame >= 6) break;
  }
  int labelStep = minorStep;
  for (int s : {5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000}) {
    labelStep = s;
    if (s >= minorStep && s * pxPerFrame >= 45) break;
  }
  p.setPen(kTick);
  QFont f = font();
  f.setPointSizeF(std::max(6.5, f.pointSizeF() - 1.5));
  p.setFont(f);
  int first = static_cast<int>(std::ceil(a.start / minorStep)) * minorStep;
  for (int fr = first; fr <= a.end; fr += minorStep) {
    double x = a.x(fr);
    bool major = fr % labelStep == 0;
    p.drawLine(QPointF(x, 0), QPointF(x, major ? 9 : 5));
    if (major) p.drawText(QRectF(x - 25, 9, 50, 12), Qt::AlignHCenter | Qt::AlignTop, QString::number(fr));
  }
  // Keys.
  for (const auto& [t, bits] : keyMarks()) {
    double shown = t;
    if (dragging_ && moved_ && selected_.count(t)) shown = t + dragDelta_;
    double x = a.x(shown);
    if (x < a.left - 6 || x > a.right + 6) continue;
    QRectF box(x - 3, 13, 6, 15);
    std::vector<QColor> cols;
    if (bits & 1) cols.push_back(kKeyPos);
    if (bits & 2) cols.push_back(kKeyRot);
    if (bits & 4) cols.push_back(kKeyScale);
    if (bits & 8) cols.push_back(kKeyOther);
    double h = box.height() / std::max<size_t>(1, cols.size());
    for (size_t i = 0; i < cols.size(); ++i) p.fillRect(QRectF(box.left(), box.top() + h * i, box.width(), h), cols[i]);
    if (selected_.count(t)) {
      p.setPen(QPen(Qt::white, 1.5));
      p.drawRect(box.adjusted(-1, -1, 1, 1));
    } else {
      p.setPen(QColor(0x10, 0x10, 0x10));
      p.drawRect(box);
    }
    if (dragging_ && moved_ && copyDrag_ && selected_.count(t)) {
      // Ghost of the original when copying.
      p.setPen(QPen(QColor(255, 255, 255, 90), 1, Qt::DashLine));
      p.drawRect(QRectF(a.x(t) - 3, 13, 6, 15));
    }
  }
  // Current frame.
  double cx = a.x(c_->frame());
  p.setPen(QPen(c_->autoKey() ? kAutoRed : theme::accent(), 2));
  p.drawLine(QPointF(cx, 0), QPointF(cx, height()));
}

double TrackBar::keyAt(double x) const {
  TimeAxis a = timeAxisFor(*c_, width());
  double best = std::nan(""), bestD = 5.0;
  for (const auto& [t, bits] : keyMarks()) {
    double d = std::fabs(a.x(t) - x);
    if (d <= bestD) {
      bestD = d;
      best = t;
    }
  }
  return best;
}

void TrackBar::mousePressEvent(QMouseEvent* e) {
  setFocus();
  if (e->button() != Qt::LeftButton) return;
  TimeAxis a = timeAxisFor(*c_, width());
  double k = keyAt(e->position().x());
  bool ctrl = e->modifiers() & Qt::ControlModifier;
  if (std::isnan(k)) {
    if (!ctrl) selected_.clear();
    c_->setFrame(std::round(a.frame(e->position().x())));
    update();
    return;
  }
  if (ctrl) {
    if (selected_.count(k)) selected_.erase(k);
    else selected_.insert(k);
  } else if (!selected_.count(k)) {
    selected_ = {k};
  }
  dragging_ = true;
  moved_ = false;
  copyDrag_ = e->modifiers() & Qt::ShiftModifier;
  dragStartFrame_ = a.frame(e->position().x());
  dragDelta_ = 0;
  update();
}

void TrackBar::mouseMoveEvent(QMouseEvent* e) {
  if (!dragging_) return;
  TimeAxis a = timeAxisFor(*c_, width());
  double d = std::round(a.frame(e->position().x()) - dragStartFrame_);
  if (d != dragDelta_) {
    dragDelta_ = d;
    moved_ = d != 0;
    update();
  }
}

void TrackBar::mouseReleaseEvent(QMouseEvent*) {
  if (!dragging_) return;
  dragging_ = false;
  if (!moved_ || dragDelta_ == 0) {
    update();
    return;
  }
  double delta = dragDelta_;
  bool copy = copyDrag_;
  std::set<double> sel = selected_;
  QVector<uint32_t> ids = c_->trackBarNodes();
  std::vector<uint32_t> nodes(ids.begin(), ids.end());
  Animation* anim = c_->animation();
  int flt = filter;
  auto filterOk = [this](const std::string& path) { return pathPassesFilter(path); };
  c_->editAnimation(copy ? tr("Copy Keys") : tr("Move Keys"), [&] {
    // Process in an order that never moves a key onto one that still has to move.
    std::vector<double> times(sel.begin(), sel.end());
    if (delta > 0) std::reverse(times.begin(), times.end());
    for (double t : times) {
      if (copy || flt != 0) {
        for (uint32_t id : nodes)
          for (const auto& path : anim->nodeTrackPaths(id)) {
            if (!filterOk(path)) continue;
            auto apply = [&](Track& tr) {
              int i = tr.keyIndexAt(t);
              if (i < 0) return;
              Key k = tr.keys[static_cast<size_t>(i)];
              if (!copy) tr.keys.erase(tr.keys.begin() + i);
              tr.deleteKeyAt(t + delta);
              k.time = t + delta;
              tr.keys.push_back(k);
              tr.sortKeys();
            };
            if (Track* tr = anim->findTrack(path)) apply(*tr);
            for (auto& L : anim->layers) {
              auto it = L.tracks.find(path);
              if (it != L.tracks.end()) apply(it->second);
            }
          }
        if (flt == 0 || flt == 1 || flt == 3)
          for (uint32_t id : nodes) {
            auto na = anim->nodes.find(id);
            if (na == anim->nodes.end()) continue;
            auto& rk = na->second.rotationKeys;
            for (size_t i = 0; i < rk.size(); ++i)
              if (std::fabs(rk[i].time - t) < 1e-3) {
                RotationKey k = rk[i];
                k.time = t + delta;
                if (!copy) rk.erase(rk.begin() + static_cast<long>(i));
                rk.erase(std::remove_if(rk.begin(), rk.end(), [&](const RotationKey& o) { return std::fabs(o.time - k.time) < 1e-3; }),
                         rk.end());
                rk.push_back(k);
                std::sort(rk.begin(), rk.end(), [](const RotationKey& x, const RotationKey& y) { return x.time < y.time; });
                break;
              }
          }
      } else {
        anim->moveNodeKeys(nodes, t, t + delta);
      }
    }
  });
  std::set<double> moved;
  for (double t : sel) moved.insert(t + delta);
  selected_ = moved;
  update();
}

void TrackBar::deleteSelectedKeys() {
  if (selected_.empty()) return;
  QVector<uint32_t> ids = c_->trackBarNodes();
  Animation* anim = c_->animation();
  std::set<double> sel = selected_;
  c_->editAnimation(tr("Delete Keys"), [&] {
    for (double t : sel)
      for (uint32_t id : ids) {
        if (filter == 0) {
          anim->deleteNodeKeysAt(id, t);
          continue;
        }
        for (const auto& path : anim->nodeTrackPaths(id))
          if (pathPassesFilter(path)) anim->deleteKey(path, t);
      }
  });
  selected_.clear();
  update();
}

void TrackBar::keyPressEvent(QKeyEvent* e) {
  if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) {
    deleteSelectedKeys();
    return;
  }
  QWidget::keyPressEvent(e);
}

void TrackBar::contextMenuEvent(QContextMenuEvent* e) {
  Animation* anim = c_->animation();
  if (!anim) return;
  TimeAxis a = timeAxisFor(*c_, width());
  double k = keyAt(e->pos().x());
  QMenu menu(this);
  QVector<uint32_t> ids = c_->trackBarNodes();
  if (!std::isnan(k)) {
    if (!selected_.count(k)) selected_ = {k};
    update();
    // Tracks with a key at this time.
    QMenu* props = menu.addMenu(tr("Key Properties"));
    QMenu* del = menu.addMenu(tr("Delete Key"));
    for (uint32_t id : ids)
      for (const auto& path : anim->nodeTrackPaths(id)) {
        const Track* t = anim->findTrack(path);
        if (!t || t->keyIndexAt(k) < 0 || !pathPassesFilter(path)) continue;
        const Node* n = c_->scene().findNode(id);
        QString label = QStringLiteral("%1: %2").arg(QString::fromStdString(n ? n->name : std::string()),
                                                     QString::fromStdString(Animation::displayName(c_->scene(), path)));
        int idx = t->keyIndexAt(k);
        double value = t->keys[static_cast<size_t>(idx)].value;
        props->addAction(QStringLiteral("%1 = %2").arg(label).arg(value, 0, 'g', 6), this, [this, path, k] {
          // Jump to the key; the Motion panel's Key Info shows it.
          c_->setFrame(k);
          if (QAction* m = c_->window().command("anim.motionPanel")) m->trigger();
          Q_UNUSED(path);
        });
        del->addAction(label, this, [this, path, k] {
          c_->editAnimation(tr("Delete Key"), [&] { c_->animation()->deleteKey(path, k); });
        });
      }
    del->addSeparator();
    del->addAction(tr("All"), this, [this, k, ids] {
      c_->editAnimation(tr("Delete Key"), [&] {
        for (uint32_t id : ids) c_->animation()->deleteNodeKeysAt(id, k);
      });
    });
    menu.addAction(tr("Delete Selected Keys"), this, [this] { deleteSelectedKeys(); });
    menu.addSeparator();
  }
  QMenu* flt = menu.addMenu(tr("Filter"));
  const char* names[] = {"All Keys", "All Transform Keys", "Current Transform: Position", "Current Transform: Rotation",
                         "Current Transform: Scale"};
  for (int i = 0; i < 5; ++i) {
    QAction* act = flt->addAction(tr(names[i]), this, [this, i] {
      filter = i;
      selected_.clear();
      update();
    });
    act->setCheckable(true);
    act->setChecked(filter == i);
  }
  QAction* src = menu.addAction(tr("Show Keys of All Objects"), this, [this] { c_->setTrackBarSource(c_->trackBarSource() == 1 ? 0 : 1); });
  src->setCheckable(true);
  src->setChecked(c_->trackBarSource() == 1);
  menu.addSeparator();
  double f = std::round(a.frame(e->pos().x()));
  menu.addAction(tr("Goto Time %1").arg(f), this, [this, f] { c_->setFrame(f); });
  menu.exec(e->globalPos());
}

// ---------------------------------------------------------------------------------------------
// Controls

namespace {

QToolButton* toolButton(QWidget* parent, const QIcon& icon, const QString& tip, bool checkable = false) {
  auto* b = new QToolButton(parent);
  b->setIcon(icon);
  b->setToolTip(tip);
  b->setCheckable(checkable);
  b->setAutoRaise(true);
  b->setIconSize(QSize(20, 20));
  return b;
}

}  // namespace

AnimControlBar::AnimControlBar(AnimController* c, QWidget* parent) : QWidget(parent), c_(c) {
  setObjectName("animControls");
  auto* h = new QHBoxLayout(this);
  h->setContentsMargins(4, 2, 4, 2);
  h->setSpacing(3);
  MainWindow& w = c->window();

  autoKey_ = new QToolButton(this);
  autoKey_->setObjectName("autoKeyButton");
  autoKey_->setDefaultAction(w.command("anim.autoKey"));
  autoKey_->setToolButtonStyle(Qt::ToolButtonTextOnly);
  autoKey_->setMinimumWidth(72);
  setKey_ = new QToolButton(this);
  setKey_->setObjectName("setKeyModeButton");
  setKey_->setDefaultAction(w.command("anim.setKeyMode"));
  setKey_->setToolButtonStyle(Qt::ToolButtonTextOnly);
  setKey_->setMinimumWidth(64);
  source_ = new QComboBox(this);
  source_->addItems({tr("Selected"), tr("All Objects")});
  source_->setToolTip(tr("Which objects' keys the track bar shows and the key buttons step through"));
  filters_ = toolButton(this, animicons::keyFilters(), tr("Key Filters..."));
  filters_->setText(tr("Key Filters..."));
  filters_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  setKeys_ = new QToolButton(this);
  setKeys_->setObjectName("setKeysButton");
  setKeys_->setDefaultAction(w.command("anim.setKey"));
  setKeys_->setIconSize(QSize(26, 26));
  setKeys_->setAutoRaise(true);

  h->addWidget(autoKey_);
  h->addWidget(setKey_);
  h->addWidget(source_);
  h->addWidget(filters_);
  h->addWidget(setKeys_);
  h->addSpacing(12);
  for (const char* id : {"anim.goStart", "anim.prevFrame"}) {
    auto* b = new QToolButton(this);
    b->setDefaultAction(w.command(id));
    b->setAutoRaise(true);
    h->addWidget(b);
  }
  play_ = new QToolButton(this);
  play_->setObjectName("playButton");
  play_->setDefaultAction(w.command("anim.play"));
  play_->setAutoRaise(true);
  h->addWidget(play_);
  for (const char* id : {"anim.nextFrame", "anim.goEnd"}) {
    auto* b = new QToolButton(this);
    b->setDefaultAction(w.command(id));
    b->setAutoRaise(true);
    h->addWidget(b);
  }
  keyMode_ = new QToolButton(this);
  keyMode_->setDefaultAction(w.command("anim.keyMode"));
  keyMode_->setAutoRaise(true);
  h->addWidget(keyMode_);
  frame_ = new QDoubleSpinBox(this);
  frame_->setObjectName("currentFrameSpinner");
  frame_->setDecimals(0);
  frame_->setKeyboardTracking(false);
  frame_->setToolTip(tr("Current frame"));
  frame_->setMinimumWidth(70);
  h->addWidget(frame_);
  auto* tc = new QToolButton(this);
  tc->setDefaultAction(w.command("anim.timeConfig"));
  tc->setAutoRaise(true);
  h->addWidget(tc);
  timeLabel_ = new QLabel(this);
  timeLabel_->setMinimumWidth(80);
  h->addWidget(timeLabel_);
  h->addStretch(1);

  connect(source_, &QComboBox::currentIndexChanged, this, [this](int i) { c_->setTrackBarSource(i); });
  connect(filters_, &QToolButton::clicked, this, [this] {
    KeyFiltersDialog d(c_, this);
    d.exec();
  });
  connect(frame_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
    if (std::fabs(v - c_->frame()) > 1e-9) c_->setFrame(v);
  });
  connect(c, &AnimController::frameChanged, this, [this] { sync(); });
  connect(c, &AnimController::rangeChanged, this, [this] { sync(); });
  connect(c, &AnimController::autoKeyChanged, this, [this] { sync(); });
  connect(c, &AnimController::playingChanged, this, [this] { sync(); });
  connect(c, &AnimController::timeDisplayChanged, this, [this] { sync(); });
  connect(c, &AnimController::trackBarSourceChanged, this, [this] {
    QSignalBlocker b(source_);
    source_->setCurrentIndex(c_->trackBarSource());
  });
  sync();
}

void AnimControlBar::sync() {
  {
    QSignalBlocker b(frame_);
    frame_->setRange(c_->startFrame(), c_->endFrame());
    frame_->setValue(c_->frame());
  }
  autoKey_->setStyleSheet(c_->autoKey() ? "QToolButton { background: #c82a2a; color: white; font-weight: bold; }"
                                        : QString());
  timeLabel_->setText(c_->timeDisplay() == 0 ? QString() : c_->formatTime(c_->frame()));
}

// ---------------------------------------------------------------------------------------------
// Dialogs

KeyFiltersDialog::KeyFiltersDialog(AnimController* c, QWidget* parent) : QDialog(parent) {
  setWindowTitle(tr("Set Key Filters"));
  auto* v = new QVBoxLayout(this);
  struct Item {
    const char* label;
    int bit;
  };
  const Item items[] = {{"Position", KeyPosition},          {"Rotation", KeyRotation},     {"Scale", KeyScale},
                        {"Object Parameters", KeyObjectParams}, {"Custom Attributes", KeyCustom}, {"Modifiers", KeyModifiers},
                        {"Materials", KeyMaterials},         {"Other (Constraints)", KeyOther}};
  std::vector<std::pair<QCheckBox*, int>> boxes;
  auto* all = new QCheckBox(tr("All"), this);
  v->addWidget(all);
  for (const auto& it : items) {
    auto* cb = new QCheckBox(tr(it.label), this);
    cb->setChecked(c->keyFilters() & it.bit);
    v->addWidget(cb);
    boxes.emplace_back(cb, it.bit);
  }
  connect(all, &QCheckBox::toggled, this, [boxes](bool on) {
    for (auto& b : boxes) b.first->setChecked(on);
  });
  auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  v->addWidget(bb);
  connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(bb, &QDialogButtonBox::accepted, this, [this, c, boxes] {
    int f = 0;
    for (auto& b : boxes)
      if (b.first->isChecked()) f |= b.second;
    c->setKeyFilters(f);
    accept();
  });
}

TimeConfigDialog::TimeConfigDialog(AnimController* c, QWidget* parent) : QDialog(parent), c_(c) {
  setObjectName("timeConfigDialog");
  setWindowTitle(tr("Time Configuration"));
  auto* v = new QVBoxLayout(this);
  auto* top = new QHBoxLayout;
  v->addLayout(top);

  auto* rate = new QGroupBox(tr("Frame Rate"), this);
  auto* rl = new QVBoxLayout(rate);
  const char* rates[] = {"NTSC (30)", "PAL (25)", "Film (24)", "Custom"};
  const float values[] = {30.0f, 25.0f, 24.0f, 0.0f};
  for (int i = 0; i < 4; ++i) {
    rate_[i] = new QRadioButton(tr(rates[i]), rate);
    rl->addWidget(rate_[i]);
  }
  fps_ = new QDoubleSpinBox(rate);
  fps_->setRange(1, 1000);
  fps_->setDecimals(2);
  fps_->setValue(c->fps());
  auto* fpsRow = new QFormLayout;
  fpsRow->addRow(tr("FPS:"), fps_);
  rl->addLayout(fpsRow);
  int match = 3;
  for (int i = 0; i < 3; ++i)
    if (std::fabs(c->fps() - values[i]) < 1e-3) match = i;
  rate_[match]->setChecked(true);
  for (int i = 0; i < 3; ++i) {
    float val = values[i];
    connect(rate_[i], &QRadioButton::toggled, this, [this, val](bool on) {
      if (on) fps_->setValue(val);
    });
  }
  connect(fps_, &QDoubleSpinBox::valueChanged, this, [this, values](double f) {
    for (int i = 0; i < 3; ++i)
      if (std::fabs(f - values[i]) < 1e-3) {
        QSignalBlocker b(rate_[i]);
        rate_[i]->setChecked(true);
        return;
      }
    rate_[3]->setChecked(true);
  });
  top->addWidget(rate);

  auto* disp = new QGroupBox(tr("Time Display"), this);
  auto* dl = new QVBoxLayout(disp);
  const char* modes[] = {"Frames", "SMPTE", "FRAME:TICKS", "MM:SS:TICKS"};
  for (int i = 0; i < 4; ++i) {
    display_[i] = new QRadioButton(tr(modes[i]), disp);
    dl->addWidget(display_[i]);
  }
  display_[c->timeDisplay()]->setChecked(true);
  top->addWidget(disp);

  auto* play = new QGroupBox(tr("Playback"), this);
  auto* pl = new QFormLayout(play);
  realTime_ = new QCheckBox(tr("Real Time"), play);
  realTime_->setChecked(c->realTime);
  loop_ = new QCheckBox(tr("Loop"), play);
  loop_->setChecked(c->loop);
  speed_ = new QComboBox(play);
  speed_->addItems({"1/4x", "1/2x", "1x", "2x", "4x"});
  const double speeds[] = {0.25, 0.5, 1.0, 2.0, 4.0};
  for (int i = 0; i < 5; ++i)
    if (std::fabs(c->speed - speeds[i]) < 1e-6) speed_->setCurrentIndex(i);
  direction_ = new QComboBox(play);
  direction_->addItems({tr("Forward"), tr("Reverse"), tr("Ping-Pong")});
  direction_->setCurrentIndex(c->direction == 1 ? 0 : c->direction == -1 ? 1 : 2);
  pl->addRow(realTime_);
  pl->addRow(loop_);
  pl->addRow(tr("Speed:"), speed_);
  pl->addRow(tr("Direction:"), direction_);
  v->addWidget(play);

  auto* an = new QGroupBox(tr("Animation"), this);
  auto* al = new QFormLayout(an);
  auto spin = [an](int val) {
    auto* s = new QSpinBox(an);
    s->setRange(-100000, 100000);
    s->setValue(val);
    return s;
  };
  start_ = spin(c->startFrame());
  start_->setObjectName("timeConfigStart");
  end_ = spin(c->endFrame());
  end_->setObjectName("timeConfigEnd");
  length_ = spin(c->endFrame() - c->startFrame());
  length_->setMinimum(1);
  current_ = spin(static_cast<int>(std::lround(c->frame())));
  rescale_ = new QCheckBox(tr("Re-scale Time (move keys with the new range)"), an);
  al->addRow(tr("Start Time:"), start_);
  al->addRow(tr("End Time:"), end_);
  al->addRow(tr("Length:"), length_);
  al->addRow(tr("Current Time:"), current_);
  al->addRow(rescale_);
  connect(start_, &QSpinBox::valueChanged, this, [this](int s) {
    QSignalBlocker b(length_);
    length_->setValue(std::max(1, end_->value() - s));
  });
  connect(end_, &QSpinBox::valueChanged, this, [this](int e) {
    QSignalBlocker b(length_);
    length_->setValue(std::max(1, e - start_->value()));
  });
  connect(length_, &QSpinBox::valueChanged, this, [this](int l) {
    QSignalBlocker b(end_);
    end_->setValue(start_->value() + l);
  });
  v->addWidget(an);

  auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  v->addWidget(bb);
  connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(bb, &QDialogButtonBox::accepted, this, [this] {
    apply();
    accept();
  });
}

void TimeConfigDialog::apply() {
  const double speeds[] = {0.25, 0.5, 1.0, 2.0, 4.0};
  c_->realTime = realTime_->isChecked();
  c_->loop = loop_->isChecked();
  c_->speed = speeds[std::clamp(speed_->currentIndex(), 0, 4)];
  c_->direction = direction_->currentIndex() == 0 ? 1 : direction_->currentIndex() == 1 ? -1 : 0;
  for (int i = 0; i < 4; ++i)
    if (display_[i]->isChecked()) c_->setTimeDisplay(i);
  int oldStart = c_->startFrame(), oldEnd = c_->endFrame();
  int ns = start_->value(), ne = std::max(start_->value() + 1, end_->value());
  if (rescale_->isChecked() && (ns != oldStart || ne != oldEnd)) {
    double scale = static_cast<double>(ne - ns) / std::max(1, oldEnd - oldStart);
    auto remap = [&](double t) { return ns + (t - oldStart) * scale; };
    c_->editAnimation(tr("Re-scale Time"), [&] {
      Animation* anim = c_->animation();
      auto fixTrack = [&](Track& t) {
        for (auto& k : t.keys) k.time = remap(k.time);
        for (auto& s : t.subs)
          for (auto& k : s.keys) k.time = remap(k.time);
      };
      for (auto& kv : anim->tracks) fixTrack(kv.second);
      for (auto& L : anim->layers)
        for (auto& kv : L.tracks) fixTrack(kv.second);
      for (auto& kv : anim->nodes)
        for (auto& k : kv.second.rotationKeys) k.time = remap(k.time);
    });
  }
  c_->setFps(static_cast<float>(fps_->value()));
  c_->setRange(ns, ne);
  c_->setFrame(current_->value());
}

}  // namespace veraa
