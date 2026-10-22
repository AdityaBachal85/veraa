// Rendered Frame Window: progressive render display with live color corrections (like a VFB).
#pragma once

#include <QImage>
#include <QMainWindow>
#include <QTimer>

#include "renderer.h"

class QAction;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QSlider;

namespace veraa {

class ImageView : public QWidget {
  Q_OBJECT
 public:
  explicit ImageView(QWidget* parent = nullptr);
  void setImage(const QImage& img);
  const QImage& image() const { return image_; }
  void fit();
  void zoom100();

 protected:
  void paintEvent(QPaintEvent* e) override;
  void wheelEvent(QWheelEvent* e) override;
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void mouseDoubleClickEvent(QMouseEvent* e) override;
  void resizeEvent(QResizeEvent* e) override;

 private:
  QImage image_;
  double zoom_ = 1.0;
  QPointF offset_;
  QPoint last_;
  bool fitMode_ = true;
};

class FrameBufferWindow : public QMainWindow {
  Q_OBJECT
 public:
  explicit FrameBufferWindow(QWidget* parent = nullptr);
  ~FrameBufferWindow() override;

  // Starts a render of the scene from the given view. viewName is shown in the title.
  void startRender(const Scene& scene, const RenderView& view, const QString& viewName);
  void stopRender();
  bool isRendering() const;
  const QImage& image() const { return view_->image(); }
  // Blocks until the render finishes (self test / screenshots). Returns false on timeout.
  bool waitForFinish(int timeoutMs);

 Q_SIGNALS:
  void renderAgainRequested();

 private:
  void tick();
  void resolveImage(bool final);
  void saveImage();
  DisplaySettings display() const;

  Renderer renderer_;
  ImageView* view_;
  QTimer timer_;
  QAction *renderAct_, *stopAct_, *saveAct_, *denoiseAct_;
  QDoubleSpinBox* exposure_;
  QComboBox* tonemap_;
  QDoubleSpinBox* contrast_;
  QDoubleSpinBox* saturation_;
  QLabel* stats_;
  QListWidget* log_;
  QString viewName_;
  bool finalized_ = true;
};

}  // namespace veraa
