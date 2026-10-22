#include "viewportpanel.h"

#include <QSplitter>
#include <QVBoxLayout>

#include <functional>

namespace veraa {

namespace {

// Layout specs: h(...) splits left to right, v(...) top to bottom, digits are slots.
const char* const kSpecs[14] = {
    "0",                 // 1 viewport
    "h(0,1)",            // 2 side by side
    "v(0,1)",            // 2 stacked
    "h(v(0,1),2)",       // 2 left, 1 right
    "h(0,v(1,2))",       // 1 left, 2 right
    "v(h(0,1),2)",       // 2 top, 1 bottom
    "v(0,h(1,2))",       // 1 top, 2 bottom
    "h(0,1,2)",          // 3 columns
    "v(0,1,2)",          // 3 rows
    "v(h(0,1),h(2,3))",  // 2 x 2
    "h(v(0,1,2),3)",     // 3 left, 1 big right
    "h(0,v(1,2,3))",     // 1 big left, 3 right
    "v(h(0,1,2),3)",     // 3 top, 1 big bottom
    "v(0,h(1,2,3))",     // 1 big top, 3 bottom
};

const char* const kNames[14] = {"Single",           "Two Vertical",      "Two Horizontal",   "Three: 2 Left",
                                "Three: 2 Right",   "Three: 2 Top",      "Three: 2 Bottom",  "Three Columns",
                                "Three Rows",       "Four",              "Four: 3 Left",     "Four: 3 Right",
                                "Four: 3 Top",      "Four: 3 Bottom"};

struct SpecNode {
  char dir = 0;  // 'h', 'v' or 0 for a slot
  int slot = -1;
  std::vector<SpecNode> kids;
};

SpecNode parse(const char*& s) {
  SpecNode n;
  if (*s == 'h' || *s == 'v') {
    n.dir = *s;
    s += 2;  // "h("
    while (true) {
      n.kids.push_back(parse(s));
      if (*s == ',') {
        ++s;
        continue;
      }
      ++s;  // ')'
      break;
    }
  } else {
    n.slot = *s - '0';
    ++s;
  }
  return n;
}

SpecNode specFor(int layout) {
  const char* s = kSpecs[std::clamp(layout, 0, 13)];
  return parse(s);
}

int slotCount(const SpecNode& n) {
  if (!n.dir) return 1;
  int c = 0;
  for (const auto& k : n.kids) c += slotCount(k);
  return c;
}

// The biggest slot of the 3+1 layouts shows the Perspective view.
bool bigLastLayout(int layout) { return layout == 10 || layout == 12; }

}  // namespace

ViewportPanel::ViewportPanel(SceneDocument* doc, EditorState* state, QWidget* parent) : QWidget(parent) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  const ViewType types[4] = {ViewType::Top, ViewType::Front, ViewType::Left, ViewType::Perspective};
  for (int i = 0; i < 4; ++i) {
    auto* vp = new Viewport(doc, state, types[i], this);
    viewports_[i] = vp;
    connect(vp, &Viewport::activated, this, &ViewportPanel::setActive);
    connect(vp, &Viewport::maximizeToggleRequested, this, &ViewportPanel::toggleMaximize);
    connect(vp, &Viewport::contextMenuRequested, this, &ViewportPanel::contextMenuRequested);
    connect(vp, &Viewport::configureRequested, this, [this, vp](int tab) {
      setActive(vp);
      Q_EMIT configureRequested(tab);
    });
  }
  rebuild();
  viewports_[activeIndex_]->setActive(true);
}

int ViewportPanel::layoutCount() { return 14; }

QString ViewportPanel::layoutName(int layout) { return tr(kNames[std::clamp(layout, 0, 13)]); }

QVector<int> ViewportPanel::layoutViews(int layout) {
  int n = slotCount(specFor(layout));
  switch (n) {
    case 1: return {3};
    case 2: return {0, 3};
    case 3: return {0, 1, 3};
    default: return bigLastLayout(layout) || layout == 9 ? QVector<int>{0, 1, 2, 3} : QVector<int>{3, 0, 1, 2};
  }
}

QVector<QRectF> ViewportPanel::layoutCells(int layout) {
  SpecNode root = specFor(layout);
  QVector<QRectF> cells(slotCount(root));
  std::function<void(const SpecNode&, QRectF)> walk = [&](const SpecNode& n, QRectF r) {
    if (!n.dir) {
      cells[n.slot] = r;
      return;
    }
    int k = static_cast<int>(n.kids.size());
    for (int i = 0; i < k; ++i) {
      QRectF c = n.dir == 'h' ? QRectF(r.x() + r.width() * i / k, r.y(), r.width() / k, r.height())
                              : QRectF(r.x(), r.y() + r.height() * i / k, r.width(), r.height() / k);
      walk(n.kids[i], c);
    }
  };
  walk(root, QRectF(0, 0, 1, 1));
  return cells;
}

QVector<Viewport*> ViewportPanel::visibleViewports() const {
  QVector<Viewport*> out;
  for (int i : layoutViews(layout_)) out.push_back(viewports_[i]);
  return out;
}

void ViewportPanel::setViewLayout(int layout) {
  layout = std::clamp(layout, 0, layoutCount() - 1);
  if (layout == layout_ && root_) return;
  layout_ = layout;
  maximized_ = false;
  rebuild();
  if (!layoutViews(layout_).contains(activeIndex_)) setActive(viewports_[layoutViews(layout_).back()]);
  Q_EMIT layoutChanged(layout_);
}

void ViewportPanel::rebuild() {
  // Detach the viewports, then rebuild the splitter tree for the layout.
  for (Viewport* vp : viewports_) {
    vp->hide();
    vp->setParent(this);
  }
  delete root_;
  root_ = nullptr;
  QVector<int> views = layoutViews(layout_);
  SpecNode spec = specFor(layout_);
  std::vector<QSplitter*> rows;  // splitters to keep aligned in the 2x2 layout
  std::function<QWidget*(const SpecNode&, QWidget*)> build = [&](const SpecNode& n, QWidget* parent) -> QWidget* {
    if (!n.dir) {
      Viewport* vp = viewports_[views[n.slot]];
      vp->setParent(parent);
      vp->show();
      return vp;
    }
    auto* s = new QSplitter(n.dir == 'h' ? Qt::Horizontal : Qt::Vertical, parent);
    s->setHandleWidth(3);
    s->setChildrenCollapsible(false);
    for (const auto& k : n.kids) s->addWidget(build(k, s));
    if (n.dir == 'h') rows.push_back(s);
    return s;
  };
  // A container owns the splitters (never a viewport itself, so deleting it is always safe).
  root_ = new QWidget(this);
  auto* rootLayout = new QVBoxLayout(root_);
  rootLayout->setContentsMargins(0, 0, 0, 0);
  QWidget* tree = build(spec, root_);
  rootLayout->addWidget(tree);
  if (layout_ == 9 && rows.size() == 2) {
    // Keep the two rows aligned like a grid.
    QSplitter *a = rows[0], *b = rows[1];
    connect(a, &QSplitter::splitterMoved, b, [a, b] { b->setSizes(a->sizes()); });
    connect(b, &QSplitter::splitterMoved, a, [a, b] { a->setSizes(b->sizes()); });
  }
  if (bigLastLayout(layout_) || layout_ == 11 || layout_ == 13)
    if (auto* s = qobject_cast<QSplitter*>(tree)) {
      // The single big viewport gets two thirds of the space.
      bool bigFirst = layout_ == 11 || layout_ == 13;
      s->setStretchFactor(bigFirst ? 0 : 1, 2);
      s->setStretchFactor(bigFirst ? 1 : 0, 1);
    }
  layout()->addWidget(root_);
  root_->show();
}

void ViewportPanel::setActive(Viewport* vp) {
  for (int i = 0; i < 4; ++i) {
    bool a = viewports_[i] == vp;
    viewports_[i]->setActive(a);
    if (a) activeIndex_ = i;
  }
  Q_EMIT activeChanged(vp);
}

void ViewportPanel::applyMaximize() {
  QVector<int> views = layoutViews(layout_);
  for (int i : views) viewports_[i]->setVisible(!maximized_ || i == activeIndex_);
  // Hide splitters whose children are all hidden.
  std::function<bool(QWidget*)> fix = [&](QWidget* w) -> bool {
    if (auto* s = qobject_cast<QSplitter*>(w)) {
      bool any = false;
      for (int i = 0; i < s->count(); ++i) any |= fix(s->widget(i));
      s->setVisible(any);
      return any;
    }
    return !w->isHidden();
  };
  if (root_ && root_->layout() && root_->layout()->count()) fix(root_->layout()->itemAt(0)->widget());
}

void ViewportPanel::toggleMaximize() {
  maximized_ = !maximized_;
  applyMaximize();
  active()->setFocus();
}

void ViewportPanel::updateAll() {
  for (auto* vp : viewports_) vp->update();
}

void ViewportPanel::zoomExtentsAll(bool selectedOnly) {
  for (auto* vp : viewports_) vp->zoomExtents(selectedOnly);
}

}  // namespace veraa
