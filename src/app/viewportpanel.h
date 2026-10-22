// The 2x2 viewport layout (Top, Front, Left, Perspective) with maximize toggle.
#pragma once

#include <QWidget>

#include <array>

#include "viewport.h"

class QSplitter;

namespace veraa {

class ViewportPanel : public QWidget {
  Q_OBJECT
 public:
  ViewportPanel(SceneDocument* doc, EditorState* state, QWidget* parent = nullptr);

  Viewport* active() const { return viewports_[activeIndex_]; }
  Viewport* viewport(int i) const { return viewports_[i]; }
  int count() const { return static_cast<int>(viewports_.size()); }
  void setActive(Viewport* vp);
  void toggleMaximize();
  bool isMaximized() const { return maximized_; }
  void updateAll();
  void zoomExtentsAll(bool selectedOnly);

 Q_SIGNALS:
  void activeChanged(Viewport* vp);
  void contextMenuRequested(Viewport* vp, const QPoint& globalPos);

 private:
  std::array<Viewport*, 4> viewports_{};
  QSplitter *vsplit_ = nullptr, *top_ = nullptr, *bottom_ = nullptr;
  int activeIndex_ = 3;
  bool maximized_ = false;
};

}  // namespace veraa
