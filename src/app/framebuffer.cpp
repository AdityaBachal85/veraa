#include "framebuffer.h"

#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <QThread>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "icons.h"
#include "paramform.h"

namespace veraa {

ImageView::ImageView(QWidget* parent) : QWidget(parent) {
  setMinimumSize(320, 200);
  setMouseTracking(false);
}

void ImageView::setImage(const QImage& img) {
  bool sizeChanged = img.size() != image_.size();
  image_ = img;
  if (sizeChanged && fitMode_) fit();
  update();
}

void ImageView::fit() {
  fitMode_ = true;
  if (image_.isNull()) return;
  zoom_ = std::min(1.0, std::min(static_cast<double>(width() - 8) / image_.width(),
                                 static_cast<double>(height() - 8) / image_.height()));
  zoom_ = std::max(zoom_, 0.02);
  offset_ = QPointF();
  update();
}

void ImageView::zoom100() {
  fitMode_ = false;
  zoom_ = 1.0;
  offset_ = QPointF();
  update();
}

void ImageView::resizeEvent(QResizeEvent*) {
  if (fitMode_) fit();
}

void ImageView::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.fillRect(rect(), QColor(0x26, 0x26, 0x26));
  if (image_.isNull()) {
    p.setPen(QColor(150, 150, 150));
    p.drawText(rect(), Qt::AlignCenter, tr("Press Render to start"));
    return;
  }
  QSizeF s(image_.width() * zoom_, image_.height() * zoom_);
  QPointF tl(width() * 0.5 - s.width() * 0.5 + offset_.x(), height() * 0.5 - s.height() * 0.5 + offset_.y());
  p.setRenderHint(QPainter::SmoothPixmapTransform, zoom_ < 1.0);
  p.drawImage(QRectF(tl, s), image_);
  p.setPen(QColor(10, 10, 10));
  p.drawRect(QRectF(tl, s).adjusted(-1, -1, 0, 0));
}

void ImageView::wheelEvent(QWheelEvent* e) {
  fitMode_ = false;
  double f = std::pow(1.15, e->angleDelta().y() / 120.0);
  QPointF c(width() * 0.5, height() * 0.5);
  QPointF m = e->position() - c - offset_;
  double nz = std::clamp(zoom_ * f, 0.02, 32.0);
  offset_ -= m * (nz / zoom_ - 1.0);
  zoom_ = nz;
  update();
}

void ImageView::mousePressEvent(QMouseEvent* e) { last_ = e->position().toPoint(); }

void ImageView::mouseMoveEvent(QMouseEvent* e) {
  if (!(e->buttons() & (Qt::LeftButton | Qt::MiddleButton))) return;
  fitMode_ = false;
  offset_ += e->position().toPoint() - last_;
  last_ = e->position().toPoint();
  update();
}

void ImageView::mouseDoubleClickEvent(QMouseEvent*) { fit(); }

// ---------------------------------------------------------------------------------------------

FrameBufferWindow::FrameBufferWindow(QWidget* parent) : QMainWindow(parent, Qt::Window) {
  setWindowTitle(tr("Rendered Frame Window - Veraa"));
  setAttribute(Qt::WA_DeleteOnClose, false);
  resize(1180, 760);
  view_ = new ImageView(this);
  setCentralWidget(view_);

  auto* tb = addToolBar(tr("Frame Buffer"));
  tb->setMovable(false);
  tb->setIconSize(QSize(20, 20));
  tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  renderAct_ = tb->addAction(icons::render(), tr("Render"), this, [this] { Q_EMIT renderAgainRequested(); });
  stopAct_ = tb->addAction(icons::stop(), tr("Stop"), this, [this] { stopRender(); });
  tb->addSeparator();
  saveAct_ = tb->addAction(icons::save(), tr("Save Image..."), this, &FrameBufferWindow::saveImage);
  saveAct_->setShortcut(QKeySequence::Save);
  tb->addSeparator();
  denoiseAct_ = tb->addAction(tr("Denoise"));
  denoiseAct_->setCheckable(true);
  denoiseAct_->setEnabled(Renderer::denoiserAvailable());
  denoiseAct_->setToolTip(Renderer::denoiserAvailable() ? tr("Intel Open Image Denoise")
                                                        : tr("Denoiser not available in this build"));
  connect(denoiseAct_, &QAction::toggled, this, [this] { resolveImage(!renderer_.isRunning()); });
  tb->addSeparator();
  tb->addAction(icons::zoomExtents(), tr("Fit"), this, [this] { view_->fit(); });
  tb->addAction(tr("100%"), this, [this] { view_->zoom100(); });

  // Color corrections panel (applied to the existing render, no re-render needed).
  auto* dock = new QDockWidget(tr("Display"), this);
  dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
  auto* panel = new QWidget(dock);
  auto* pv = new QVBoxLayout(panel);
  auto* roll = new Rollout(tr("Color Corrections"), panel);
  exposure_ = new QDoubleSpinBox(panel);
  exposure_->setRange(-20, 20);
  exposure_->setSingleStep(0.1);
  exposure_->setSuffix(" EV");
  installSpinnerScrub(exposure_);
  tonemap_ = new QComboBox(panel);
  tonemap_->addItems({tr("Filmic (ACES)"), tr("Reinhard"), tr("Linear")});
  contrast_ = new QDoubleSpinBox(panel);
  contrast_->setRange(0, 3);
  contrast_->setSingleStep(0.05);
  installSpinnerScrub(contrast_);
  saturation_ = new QDoubleSpinBox(panel);
  saturation_->setRange(0, 3);
  saturation_->setSingleStep(0.05);
  installSpinnerScrub(saturation_);
  roll->form()->addRow(tr("Exposure:"), exposure_);
  roll->form()->addRow(tr("Tone mapping:"), tonemap_);
  roll->form()->addRow(tr("Contrast:"), contrast_);
  roll->form()->addRow(tr("Saturation:"), saturation_);
  pv->addWidget(roll);
  auto* statsRoll = new Rollout(tr("Render Info"), panel);
  stats_ = new QLabel(panel);
  stats_->setWordWrap(true);
  stats_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  statsRoll->form()->addRow(stats_);
  pv->addWidget(statsRoll);
  auto* logRoll = new Rollout(tr("Messages"), panel);
  log_ = new QListWidget(panel);
  log_->setMinimumHeight(120);
  logRoll->form()->addRow(log_);
  pv->addWidget(logRoll);
  pv->addStretch();
  dock->setWidget(panel);
  dock->setMinimumWidth(270);
  addDockWidget(Qt::RightDockWidgetArea, dock);

  auto redisplay = [this] { resolveImage(!renderer_.isRunning()); };
  connect(exposure_, &QDoubleSpinBox::valueChanged, this, redisplay);
  connect(contrast_, &QDoubleSpinBox::valueChanged, this, redisplay);
  connect(saturation_, &QDoubleSpinBox::valueChanged, this, redisplay);
  connect(tonemap_, &QComboBox::currentIndexChanged, this, redisplay);

  timer_.setInterval(200);
  connect(&timer_, &QTimer::timeout, this, &FrameBufferWindow::tick);
  stopAct_->setEnabled(false);
  stats_->setText(tr("No render yet."));
}

FrameBufferWindow::~FrameBufferWindow() { renderer_.stop(); }

DisplaySettings FrameBufferWindow::display() const {
  DisplaySettings d;
  d.exposureEV = static_cast<float>(exposure_->value());
  d.tonemap = tonemap_->currentIndex();
  d.contrast = static_cast<float>(contrast_->value());
  d.saturation = static_cast<float>(saturation_->value());
  d.denoise = denoiseAct_->isChecked() && Renderer::denoiserAvailable();
  return d;
}

void FrameBufferWindow::startRender(const Scene& scene, const RenderView& view, const QString& viewName) {
  renderer_.stop();
  log_->clear();
  viewName_ = viewName;
  std::string err;
  QElapsedTimer t;
  t.start();
  if (!renderer_.setScene(scene, &err)) {
    log_->addItem(tr("Error: %1").arg(QString::fromStdString(err)));
    stats_->setText(tr("Render failed."));
    return;
  }
  for (const auto& w : renderer_.warnings()) log_->addItem(tr("Warning: %1").arg(QString::fromStdString(w)));
  log_->addItem(tr("Scene prepared in %1 ms").arg(t.elapsed()));
  renderer_.setView(view);
  DisplaySettings d = displaySettingsFromScene(scene);
  {
    QSignalBlocker b1(exposure_), b2(tonemap_), b3(contrast_), b4(saturation_), b5(denoiseAct_);
    exposure_->setValue(d.exposureEV);
    tonemap_->setCurrentIndex(d.tonemap);
    contrast_->setValue(d.contrast);
    saturation_->setValue(d.saturation);
    denoiseAct_->setChecked(d.denoise && Renderer::denoiserAvailable());
  }
  setWindowTitle(tr("Rendered Frame Window - %1 - Veraa").arg(viewName));
  finalized_ = false;
  renderer_.start();
  stopAct_->setEnabled(true);
  timer_.start();
  tick();
}

void FrameBufferWindow::stopRender() {
  if (!renderer_.isRunning()) return;
  renderer_.stop();
  log_->addItem(tr("Render stopped by user"));
  tick();
}

bool FrameBufferWindow::isRendering() const { return renderer_.isRunning(); }

void FrameBufferWindow::tick() {
  RenderStats s = renderer_.stats();
  int secs = static_cast<int>(s.seconds);
  stats_->setText(tr("View: %1\nResolution: %2 x %3\nPasses: %4 / %5\nTime: %6:%7\nTriangles: %8\nDenoiser: %9")
                      .arg(viewName_)
                      .arg(renderer_.width())
                      .arg(renderer_.height())
                      .arg(s.passes)
                      .arg(s.targetPasses)
                      .arg(secs / 60, 2, 10, QChar('0'))
                      .arg(secs % 60, 2, 10, QChar('0'))
                      .arg(QLocale().toString(static_cast<qulonglong>(s.triangles)))
                      .arg(Renderer::denoiserAvailable() ? tr("available") : tr("not available")));
  bool running = renderer_.isRunning();
  if (running) {
    resolveImage(false);
    return;
  }
  timer_.stop();
  stopAct_->setEnabled(false);
  if (!finalized_) {
    finalized_ = true;
    resolveImage(true);
    log_->addItem(tr("Finished: %1 passes in %2 s").arg(s.passes).arg(s.seconds, 0, 'f', 1));
  }
}

void FrameBufferWindow::resolveImage(bool final) {
  int w = renderer_.width(), h = renderer_.height();
  if (w <= 0 || h <= 0) return;
  DisplaySettings d = display();
  if (!final) d.denoise = false;  // denoise only when the render is done (it is slow)
  std::vector<uint8_t> rgba;
  renderer_.resolve(d, rgba);
  if (rgba.size() != static_cast<size_t>(w) * h * 4) return;
  QImage img(rgba.data(), w, h, w * 4, QImage::Format_RGBA8888);
  view_->setImage(img.convertToFormat(QImage::Format_RGB32));
}

void FrameBufferWindow::saveImage() {
  if (view_->image().isNull()) return;
  QString path = QFileDialog::getSaveFileName(this, tr("Save Rendered Image"), QString(),
                                              tr("PNG (*.png);;JPEG (*.jpg);;Bitmap (*.bmp);;Radiance HDR (*.hdr)"));
  if (path.isEmpty()) return;
  bool ok;
  if (QFileInfo(path).suffix().compare("hdr", Qt::CaseInsensitive) == 0) {
    std::vector<float> rgb;
    renderer_.resolveHdr(rgb);
    ok = writeHdr(path.toStdString(), renderer_.width(), renderer_.height(), rgb);
  } else {
    ok = view_->image().save(path, nullptr, 95);
  }
  if (!ok) QMessageBox::warning(this, tr("Save Image"), tr("Could not save %1").arg(path));
  else log_->addItem(tr("Saved %1").arg(QFileInfo(path).fileName()));
}

bool FrameBufferWindow::waitForFinish(int timeoutMs) {
  QElapsedTimer t;
  t.start();
  while (renderer_.isRunning() && t.elapsed() < timeoutMs) {
    QApplication::processEvents(QEventLoop::AllEvents, 50);
    QThread::msleep(15);
  }
  tick();
  return !renderer_.isRunning();
}

}  // namespace veraa
