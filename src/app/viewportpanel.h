// The viewport area: four viewports (Top, Front, Left, Perspective) arranged in one of 14
// layouts (Views > Viewport Configuration > Layout), with a maximize toggle (Alt+W).
#pragma once

#include <QRectF>
#include <QVector>
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
  int activeIndex() const { return activeIndex_; }
  void setActive(Viewport* vp);
  void setActiveIndex(int i) { setActive(viewports_[i]); }
  void toggleMaximize();
  bool isMaximized() const { return maximized_; }
  void updateAll();
  void zoomExtentsAll(bool selectedOnly);

  // ---- Layouts ----
  static int layoutCount();  // 14
  static QString layoutName(int layout);
  // Viewport indices shown by a layout, in slot order.
  static QVector<int> layoutViews(int layout);
  // Slot rectangles in [0,1]^2 (for previews), in slot order.
  static QVector<QRectF> layoutCells(int layout);
  int viewLayout() const { return layout_; }
  void setViewLayout(int layout);
  // Viewports visible in the current layout.
  QVector<Viewport*> visibleViewports() const;

 Q_SIGNALS:
  void activeChanged(Viewport* vp);
  void contextMenuRequested(Viewport* vp, const QPoint& globalPos);
  void configureRequested(int tab);
  void layoutChanged(int layout);

 private:
  void rebuild();
  void applyMaximize();

  std::array<Viewport*, 4> viewports_{};
  QWidget* root_ = nullptr;
  int layout_ = 9;  // 2x2
  int activeIndex_ = 3;
  bool maximized_ = false;
};

}  // namespace veraa
