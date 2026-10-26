// Rendered Frame Window (a VFB): progressive or bucket render display with render elements,
// Light Mix, color corrections, curves, levels, LUTs, lens effects, region rendering, history
// with A/B comparison, pixel information and saving to PNG/JPG/TIFF/EXR/HDR.
#pragma once

#include <QImage>
#include <QMainWindow>
#include <QTimer>

#include <array>

#include "renderer.h"

class QAction;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QSlider;
class QSpinBox;
class QTabWidget;
class QVBoxLayout;

namespace veraa {

class ColorSwatch;

class ImageView : public QWidget {
  Q_OBJECT
 public:
  explicit ImageView(QWidget* parent = nullptr);
  void setImage(const QImage& img);
  const QImage& image() const { return image_; }
  void fit();
  void zoom100();
  // Region tool: dragging draws a region (fractions of the image).
  void setRegionMode(bool on) { regionMode_ = on; }
  void setRegion(bool on, const QRectF& r);
  bool hasRegion() const { return regionOn_; }
  QRectF region() const { return region_; }
  void setBuckets(const std::vector<std::array<int, 4>>& b);
  // A/B compare: shows `other` to the right of a draggable split line.
  void setCompare(const QImage& other);
  void clearCompare();
  bool comparing() const { return !compare_.isNull(); }

 Q_SIGNALS:
  void pixelHovered(int x, int y);
  void regionChanged();

 protected:
  void paintEvent(QPaintEvent* e) override;
  void wheelEvent(QWheelEvent* e) override;
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;
  void mouseDoubleClickEvent(QMouseEvent* e) override;
  void resizeEvent(QResizeEvent* e) override;

 private:
  QRectF imageRect() const;
  QPointF toImage(const QPointF& widgetPos) const;

  QImage image_, compare_;
  double zoom_ = 1.0;
  QPointF offset_;
  QPoint last_;
  bool fitMode_ = true;
  bool regionMode_ = false, regionOn_ = false, draggingRegion_ = false, draggingSplit_ = false;
  QRectF region_;
  QPointF regionStart_;
  double split_ = 0.5;
  std::vector<std::array<int, 4>> buckets_;
};

// Editable tone curve (control points in [0,1]^2).
class CurveWidget : public QWidget {
  Q_OBJECT
 public:
  explicit CurveWidget(QWidget* parent = nullptr);
  void setPoints(const std::vector<Vec2>& pts);
  const std::vector<Vec2>& points() const { return points_; }

 Q_SIGNALS:
  void edited();

 protected:
  void paintEvent(QPaintEvent* e) override;
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;
  void mouseDoubleClickEvent(QMouseEvent* e) override;

 private:
  QPointF toWidget(const Vec2& p) const;
  Vec2 toCurve(const QPointF& p) const;
  std::vector<Vec2> points_{{0.0f, 0.0f}, {1.0f, 1.0f}};
  int drag_ = -1;
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
  const QImage& image() const;
  // Blocks until the render finishes (self test / screenshots). Returns false on timeout.
  bool waitForFinish(int timeoutMs);

  // Access for scripted tests.
  Renderer& renderer() { return renderer_; }
  void showElement(const QString& name);
  void setLightMixIntensity(const QString& group, float intensity);
  void saveToHistory();
  int historySize() const;
  bool saveImageTo(const QString& path, bool withAlpha);
  QStringList elementList() const;
  QStringList lightGroups() const;

 Q_SIGNALS:
  void renderAgainRequested();

 private:
  void tick();
  void resolveImage(bool final);
  void saveImageDialog();
  void saveAllElements();
  void copyToClipboard();
  void updatePixelInfo(int x, int y);
  void rebuildLightMix();
  void rebuildElements();
  void loadDisplay(const DisplaySettings& d);
  void saveDisplayToScene();
  DisplaySettings display() const;
  void redisplay();

  Renderer renderer_;
  ImageView* view_;
  QTimer timer_;
  QAction *renderAct_, *stopAct_, *saveAct_, *denoiseAct_, *regionAct_, *selectedAct_, *trackAct_, *compareAct_,
      *lensAct_;
  QComboBox* element_;
  QComboBox* channel_;
  QTabWidget* tabs_;
  // Display.
  QDoubleSpinBox* exposure_;
  QComboBox* tonemap_;
  QDoubleSpinBox *mapDark_, *mapBright_, *mapGamma_, *mapBurn_;
  QCheckBox *mapAffectBg_, *mapAdaptOnly_;
  QDoubleSpinBox* contrast_;
  QDoubleSpinBox* saturation_;
  // Corrections.
  QCheckBox* ccOn_;
  QDoubleSpinBox *wbK_, *tint_, *hue_, *burn_, *levelsBlack_, *levelsWhite_, *levelsGamma_, *lutStrength_;
  QLineEdit* lutFile_;
  CurveWidget* curve_;
  // Lens effects.
  QDoubleSpinBox *bloomI_, *bloomS_, *glareI_, *glareS_, *glareRot_, *vignette_;
  QSpinBox* glareRays_;
  // Light Mix.
  QWidget* lightMixPanel_;
  QVBoxLayout* lightMixRows_;
  QCheckBox* lightMixOn_;
  struct MixRow {
    QString name;
    QCheckBox* on;
    ColorSwatch* color;
    QDoubleSpinBox* intensity;
  };
  std::vector<MixRow> mixRows_;
  // History.
  QListWidget* history_;
  struct HistoryEntry {
    QImage image;
    QString label;
  };
  std::vector<HistoryEntry> historyEntries_;
  // Info.
  QLabel* stats_;
  QListWidget* log_;
  QLabel* pixelInfo_;
  QString viewName_;
  bool finalized_ = true;
  bool loading_ = false;
  QImage displayed_;
  std::vector<float> hdr_, alpha_, counts_;  // last resolved buffers (pixel info, saving)
};

}  // namespace veraa
