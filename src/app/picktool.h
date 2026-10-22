// Viewport tool that picks a node with a click (constraint targets, IK end joints, space warps,
// paths, ...). Right-click or Esc cancels.
#pragma once

#include <QMouseEvent>
#include <QPainter>
#include <QString>

#include <functional>

#include "document.h"
#include "editorstate.h"
#include "extension.h"
#include "viewport.h"

namespace veraa {

class PickNodeTool : public ViewportTool {
 public:
  using Filter = std::function<bool(const Node&)>;
  using Done = std::function<void(uint32_t nodeId)>;

  PickNodeTool(QString prompt, Filter filter, Done done, bool keepPicking = false)
      : prompt_(std::move(prompt)), filter_(std::move(filter)), done_(std::move(done)), keep_(keepPicking) {}

  bool mousePress(Viewport& vp, QMouseEvent* e) override {
    if (e->button() == Qt::RightButton) {
      vp.editorState()->setViewportTool(nullptr);
      return true;
    }
    if (e->button() != Qt::LeftButton) return false;
    uint32_t id = vp.pickNode(static_cast<float>(e->position().x()), static_cast<float>(e->position().y()));
    const Node* n = id ? vp.document()->scene().findNode(id) : nullptr;
    if (!n || (filter_ && !filter_(*n))) return true;
    // Keep this tool alive while the callback runs (it may replace the tool).
    auto self = vp.editorState()->viewportToolShared();
    Done done = done_;
    if (!keep_) vp.editorState()->setViewportTool(nullptr);
    done(id);
    return true;
  }

  bool mouseMove(Viewport& vp, QMouseEvent* e) override {
    uint32_t id = vp.pickNode(static_cast<float>(e->position().x()), static_cast<float>(e->position().y()));
    const Node* n = id ? vp.document()->scene().findNode(id) : nullptr;
    bool ok = n && (!filter_ || filter_(*n));
    vp.setCursor(ok ? Qt::CrossCursor : Qt::ArrowCursor);
    hover_ = ok ? id : 0;
    return false;
  }

  void drawOverlay(Viewport& vp, QPainter& p) override {
    p.setPen(QColor(0xf0, 0xd0, 0x40));
    p.drawText(QRectF(8, vp.height() - 26, vp.width() - 16, 20), Qt::AlignLeft | Qt::AlignVCenter, prompt_);
  }

  bool cancel() override { return true; }
  bool allowsGizmo() const override { return false; }
  QString prompt() const override { return prompt_; }

 private:
  QString prompt_;
  Filter filter_;
  Done done_;
  bool keep_ = false;
  uint32_t hover_ = 0;
};

}  // namespace veraa
