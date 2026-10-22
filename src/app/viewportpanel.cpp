#include "viewportpanel.h"

#include <QSplitter>
#include <QVBoxLayout>

namespace veraa {

ViewportPanel::ViewportPanel(SceneDocument* doc, EditorState* state, QWidget* parent) : QWidget(parent) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  vsplit_ = new QSplitter(Qt::Vertical, this);
  top_ = new QSplitter(Qt::Horizontal, vsplit_);
  bottom_ = new QSplitter(Qt::Horizontal, vsplit_);
  for (QSplitter* s : {vsplit_, top_, bottom_}) {
    s->setHandleWidth(3);
    s->setChildrenCollapsible(false);
  }
  const ViewType types[4] = {ViewType::Top, ViewType::Front, ViewType::Left, ViewType::Perspective};
  for (int i = 0; i < 4; ++i) {
    auto* vp = new Viewport(doc, state, types[i], i < 2 ? top_ : bottom_);
    viewports_[i] = vp;
    connect(vp, &Viewport::activated, this, &ViewportPanel::setActive);
    connect(vp, &Viewport::maximizeToggleRequested, this, &ViewportPanel::toggleMaximize);
    connect(vp, &Viewport::contextMenuRequested, this, &ViewportPanel::contextMenuRequested);
  }
  // Keep the two horizontal splitters aligned like a 2x2 grid.
  connect(top_, &QSplitter::splitterMoved, this, [this] { bottom_->setSizes(top_->sizes()); });
  connect(bottom_, &QSplitter::splitterMoved, this, [this] { top_->setSizes(bottom_->sizes()); });
  layout->addWidget(vsplit_);
  viewports_[activeIndex_]->setActive(true);
}

void ViewportPanel::setActive(Viewport* vp) {
  for (int i = 0; i < 4; ++i) {
    bool a = viewports_[i] == vp;
    viewports_[i]->setActive(a);
    if (a) activeIndex_ = i;
  }
  Q_EMIT activeChanged(vp);
}

void ViewportPanel::toggleMaximize() {
  maximized_ = !maximized_;
  for (int i = 0; i < 4; ++i) viewports_[i]->setVisible(!maximized_ || i == activeIndex_);
  bool topHas = activeIndex_ < 2;
  top_->setVisible(!maximized_ || topHas);
  bottom_->setVisible(!maximized_ || !topHas);
  active()->setFocus();
}

void ViewportPanel::updateAll() {
  for (auto* vp : viewports_) vp->update();
}

void ViewportPanel::zoomExtentsAll(bool selectedOnly) {
  for (auto* vp : viewports_) vp->zoomExtents(selectedOnly);
}

}  // namespace veraa
