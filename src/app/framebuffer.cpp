#include "framebuffer.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QThread>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

#include "icons.h"
#include "mainwindow.h"
#include "paramform.h"
#include "rendersettings.h"

namespace veraa {

// ---------------------------------------------------------------------------------------------
// ImageView

ImageView::ImageView(QWidget* parent) : QWidget(parent) {
  setMinimumSize(320, 200);
  setMouseTracking(true);
}

void ImageView::setImage(const QImage& img) {
  bool sizeChanged = img.size() != image_.size();
  image_ = img;
  if (sizeChanged && fitMode_) fit();
  update();
}

void ImageView::setRegion(bool on, const QRectF& r) {
  regionOn_ = on;
  region_ = r;
  update();
}

void ImageView::setBuckets(const std::vector<std::array<int, 4>>& b) {
  if (b == buckets_) return;
  buckets_ = b;
  update();
}

void ImageView::setCompare(const QImage& other) {
  compare_ = other;
  update();
}

void ImageView::clearCompare() {
  compare_ = QImage();
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

QRectF ImageView::imageRect() const {
  QSizeF s(image_.width() * zoom_, image_.height() * zoom_);
  QPointF tl(width() * 0.5 - s.width() * 0.5 + offset_.x(), height() * 0.5 - s.height() * 0.5 + offset_.y());
  return QRectF(tl, s);
}

QPointF ImageView::toImage(const QPointF& p) const {
  QRectF r = imageRect();
  return QPointF((p.x() - r.left()) / zoom_, (p.y() - r.top()) / zoom_);
}

void ImageView::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.fillRect(rect(), QColor(0x26, 0x26, 0x26));
  if (image_.isNull()) {
    p.setPen(QColor(150, 150, 150));
    p.drawText(rect(), Qt::AlignCenter, tr("Press Render to start"));
    return;
  }
  QRectF r = imageRect();
  p.setRenderHint(QPainter::SmoothPixmapTransform, zoom_ < 1.0);
  p.drawImage(r, image_);
  if (!compare_.isNull() && compare_.size() == image_.size()) {
    double sx = r.left() + r.width() * split_;
    QRectF right(sx, r.top(), r.right() - sx, r.height());
    QRectF src((sx - r.left()) / zoom_, 0, right.width() / zoom_, image_.height());
    p.drawImage(right, compare_, src);
    p.setPen(QPen(QColor(0xff, 0xd0, 0x60), 2));
    p.drawLine(QPointF(sx, r.top()), QPointF(sx, r.bottom()));
    p.setPen(Qt::white);
    p.drawText(QPointF(sx - 22, r.top() + 16), "A");
    p.drawText(QPointF(sx + 10, r.top() + 16), "B");
  }
  p.setPen(QColor(10, 10, 10));
  p.drawRect(r.adjusted(-1, -1, 0, 0));
  // Buckets being rendered.
  p.setPen(QPen(QColor(0xff, 0xff, 0xff, 200), 1));
  for (const auto& b : buckets_) {
    QRectF br(r.left() + b[0] * zoom_, r.top() + b[1] * zoom_, (b[2] - b[0]) * zoom_, (b[3] - b[1]) * zoom_);
    double c = std::min(br.width(), br.height()) * 0.25;
    for (int k = 0; k < 4; ++k) {
      QPointF corner = k == 0 ? br.topLeft() : k == 1 ? br.topRight() : k == 2 ? br.bottomRight() : br.bottomLeft();
      double dx = (k == 0 || k == 3) ? c : -c, dy = (k < 2) ? c : -c;
      p.drawLine(corner, corner + QPointF(dx, 0));
      p.drawLine(corner, corner + QPointF(0, dy));
    }
  }
  if (regionOn_ || draggingRegion_) {
    QRectF rr(r.left() + region_.left() * r.width(), r.top() + region_.top() * r.height(), region_.width() * r.width(),
              region_.height() * r.height());
    p.setPen(QPen(QColor(0xff, 0x60, 0x60), 1.5, Qt::DashLine));
    p.setBrush(Qt::NoBrush);
    p.drawRect(rr);
  }
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

void ImageView::mousePressEvent(QMouseEvent* e) {
  last_ = e->position().toPoint();
  if (e->button() == Qt::LeftButton && !compare_.isNull()) {
    QRectF r = imageRect();
    double sx = r.left() + r.width() * split_;
    if (std::fabs(e->position().x() - sx) < 8) {
      draggingSplit_ = true;
      return;
    }
  }
  if (e->button() == Qt::LeftButton && regionMode_ && !image_.isNull()) {
    QPointF ip = toImage(e->position());
    regionStart_ = QPointF(std::clamp(ip.x() / image_.width(), 0.0, 1.0), std::clamp(ip.y() / image_.height(), 0.0, 1.0));
    region_ = QRectF(regionStart_, QSizeF(0, 0));
    draggingRegion_ = true;
  }
}

void ImageView::mouseMoveEvent(QMouseEvent* e) {
  if (!image_.isNull()) {
    QPointF ip = toImage(e->position());
    Q_EMIT pixelHovered(static_cast<int>(std::floor(ip.x())), static_cast<int>(std::floor(ip.y())));
  }
  if (draggingSplit_) {
    QRectF r = imageRect();
    split_ = std::clamp((e->position().x() - r.left()) / std::max(r.width(), 1.0), 0.0, 1.0);
    update();
    return;
  }
  if (draggingRegion_) {
    QPointF ip = toImage(e->position());
    QPointF cur(std::clamp(ip.x() / image_.width(), 0.0, 1.0), std::clamp(ip.y() / image_.height(), 0.0, 1.0));
    region_ = QRectF(regionStart_, cur).normalized();
    update();
    return;
  }
  if (!(e->buttons() & (Qt::LeftButton | Qt::MiddleButton))) return;
  fitMode_ = false;
  offset_ += e->position().toPoint() - last_;
  last_ = e->position().toPoint();
  update();
}

void ImageView::mouseReleaseEvent(QMouseEvent*) {
  draggingSplit_ = false;
  if (draggingRegion_) {
    draggingRegion_ = false;
    regionOn_ = region_.width() > 0.005 && region_.height() > 0.005;
    update();
    Q_EMIT regionChanged();
  }
}

void ImageView::mouseDoubleClickEvent(QMouseEvent*) { fit(); }

// ---------------------------------------------------------------------------------------------
// CurveWidget

CurveWidget::CurveWidget(QWidget* parent) : QWidget(parent) {
  setMinimumSize(200, 160);
  setToolTip(tr("Drag points to shape the curve; double-click adds a point, right-click removes one"));
}

void CurveWidget::setPoints(const std::vector<Vec2>& pts) {
  points_ = pts.size() >= 2 ? pts : std::vector<Vec2>{{0, 0}, {1, 1}};
  update();
}

QPointF CurveWidget::toWidget(const Vec2& p) const {
  QRectF r = QRectF(rect()).adjusted(8, 8, -8, -8);
  return QPointF(r.left() + p.x * r.width(), r.bottom() - p.y * r.height());
}

Vec2 CurveWidget::toCurve(const QPointF& p) const {
  QRectF r = QRectF(rect()).adjusted(8, 8, -8, -8);
  return Vec2(std::clamp(static_cast<float>((p.x() - r.left()) / r.width()), 0.0f, 1.0f),
              std::clamp(static_cast<float>((r.bottom() - p.y()) / r.height()), 0.0f, 1.0f));
}

void CurveWidget::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  QRectF r = QRectF(rect()).adjusted(8, 8, -8, -8);
  p.fillRect(rect(), QColor(0x2b, 0x2b, 0x2b));
  p.setPen(QColor(0x44, 0x44, 0x44));
  for (int i = 1; i < 4; ++i) {
    p.drawLine(QPointF(r.left() + r.width() * i / 4, r.top()), QPointF(r.left() + r.width() * i / 4, r.bottom()));
    p.drawLine(QPointF(r.left(), r.top() + r.height() * i / 4), QPointF(r.right(), r.top() + r.height() * i / 4));
  }
  p.drawRect(r);
  // Sample the same monotone curve the renderer uses by drawing through a fine polyline.
  DisplaySettings d;
  d.corrections = true;
  d.curve = points_;
  d.tonemap = 2;
  QPolygonF poly;
  for (int i = 0; i <= 64; ++i) {
    float x = i / 64.0f;
    // Encode x as linear so that after the sRGB encode it lands on x, then read the result back.
    float lin = x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
    float px[3] = {lin, lin, lin};
    std::vector<uint8_t> out;
    toneMapImage(std::vector<float>(px, px + 3), 1, 1, d, out);
    poly << toWidget(Vec2(x, out.empty() ? x : out[0] / 255.0f));
  }
  p.setPen(QPen(QColor(0xe0, 0xe0, 0xe0), 1.5));
  p.drawPolyline(poly);
  p.setBrush(QColor(0xff, 0xd0, 0x60));
  p.setPen(Qt::NoPen);
  for (const auto& pt : points_) p.drawEllipse(toWidget(pt), 4, 4);
}

void CurveWidget::mousePressEvent(QMouseEvent* e) {
  drag_ = -1;
  for (size_t i = 0; i < points_.size(); ++i)
    if (QLineF(toWidget(points_[i]), e->position()).length() < 8) drag_ = static_cast<int>(i);
  if (e->button() == Qt::RightButton && drag_ > 0 && drag_ + 1 < static_cast<int>(points_.size())) {
    points_.erase(points_.begin() + drag_);
    drag_ = -1;
    update();
    Q_EMIT edited();
  }
}

void CurveWidget::mouseMoveEvent(QMouseEvent* e) {
  if (drag_ < 0 || !(e->buttons() & Qt::LeftButton)) return;
  Vec2 c = toCurve(e->position());
  float lo = drag_ > 0 ? points_[drag_ - 1].x + 0.01f : 0.0f;
  float hi = drag_ + 1 < static_cast<int>(points_.size()) ? points_[drag_ + 1].x - 0.01f : 1.0f;
  if (drag_ == 0) c.x = 0.0f;
  if (drag_ + 1 == static_cast<int>(points_.size())) c.x = 1.0f;
  c.x = std::clamp(c.x, lo, hi);
  points_[drag_] = c;
  update();
  Q_EMIT edited();
}

void CurveWidget::mouseReleaseEvent(QMouseEvent*) { drag_ = -1; }

void CurveWidget::mouseDoubleClickEvent(QMouseEvent* e) {
  Vec2 c = toCurve(e->position());
  auto it = std::upper_bound(points_.begin(), points_.end(), c, [](const Vec2& a, const Vec2& b) { return a.x < b.x; });
  points_.insert(it, c);
  update();
  Q_EMIT edited();
}

// ---------------------------------------------------------------------------------------------
// FrameBufferWindow

namespace {

QDoubleSpinBox* spin(QWidget* parent, double lo, double hi, double step, int decimals, const QString& suffix = {}) {
  auto* s = new QDoubleSpinBox(parent);
  s->setRange(lo, hi);
  s->setSingleStep(step);
  s->setDecimals(decimals);
  if (!suffix.isEmpty()) s->setSuffix(suffix);
  installSpinnerScrub(s);
  return s;
}

QWidget* scrollPage(QWidget* content) {
  auto* scroll = new QScrollArea;
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setWidget(content);
  return scroll;
}

}  // namespace

FrameBufferWindow::FrameBufferWindow(QWidget* parent) : QMainWindow(parent, Qt::Window) {
  setWindowTitle(tr("Rendered Frame Window - Veraa"));
  setAttribute(Qt::WA_DeleteOnClose, false);
  resize(1320, 820);
  view_ = new ImageView(this);
  setCentralWidget(view_);

  auto* tb = addToolBar(tr("Frame Buffer"));
  tb->setMovable(false);
  tb->setIconSize(QSize(20, 20));
  tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  renderAct_ = tb->addAction(icons::render(), tr("Render"), this, [this] { Q_EMIT renderAgainRequested(); });
  stopAct_ = tb->addAction(icons::stop(), tr("Stop"), this, [this] { stopRender(); });
  tb->addSeparator();
  regionAct_ = tb->addAction(tr("Region"));
  regionAct_->setCheckable(true);
  regionAct_->setToolTip(tr("Region render: drag a rectangle on the image, then render again"));
  selectedAct_ = tb->addAction(tr("Selected"));
  selectedAct_->setCheckable(true);
  selectedAct_->setToolTip(tr("Render only the selected objects"));
  tb->addSeparator();
  element_ = new QComboBox(this);
  element_->setMinimumWidth(170);
  element_->setToolTip(tr("Render element to display"));
  tb->addWidget(element_);
  channel_ = new QComboBox(this);
  channel_->addItems({tr("RGB"), tr("Red"), tr("Green"), tr("Blue"), tr("Alpha"), tr("Luminance")});
  tb->addWidget(channel_);
  tb->addSeparator();
  denoiseAct_ = tb->addAction(tr("Denoise"));
  denoiseAct_->setCheckable(true);
  denoiseAct_->setEnabled(Renderer::denoiserAvailable());
  denoiseAct_->setToolTip(Renderer::denoiserAvailable() ? tr("Intel Open Image Denoise")
                                                        : tr("Denoiser not available in this build"));
  lensAct_ = tb->addAction(tr("Lens FX"));
  lensAct_->setCheckable(true);
  lensAct_->setToolTip(tr("Bloom, glare and vignetting"));
  trackAct_ = tb->addAction(tr("Pixel Info"));
  trackAct_->setCheckable(true);
  trackAct_->setChecked(true);
  compareAct_ = tb->addAction(tr("A/B"));
  compareAct_->setCheckable(true);
  compareAct_->setToolTip(tr("Compare the current render (A) with the selected history image (B)"));
  tb->addSeparator();
  saveAct_ = tb->addAction(icons::save(), tr("Save..."), this, &FrameBufferWindow::saveImageDialog);
  saveAct_->setShortcut(QKeySequence::Save);
  tb->addAction(tr("Save Elements..."), this, &FrameBufferWindow::saveAllElements);
  tb->addAction(tr("Copy"), this, &FrameBufferWindow::copyToClipboard);
  tb->addSeparator();
  tb->addAction(icons::zoomExtents(), tr("Fit"), this, [this] { view_->fit(); });
  tb->addAction(tr("100%"), this, [this] { view_->zoom100(); });

  // ---- Side panel ----
  auto* dock = new QDockWidget(tr("Display"), this);
  dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
  tabs_ = new QTabWidget(dock);

  // Display tab.
  auto* disp = new QWidget;
  auto* dv = new QFormLayout(disp);
  exposure_ = spin(disp, -20, 20, 0.1, 2, " EV");
  tonemap_ = new QComboBox(disp);
  tonemap_->addItems({tr("Filmic (ACES)"), tr("Reinhard"), tr("Linear")});
  contrast_ = spin(disp, 0, 3, 0.05, 2);
  saturation_ = spin(disp, 0, 3, 0.05, 2);
  dv->addRow(tr("Exposure:"), exposure_);
  dv->addRow(tr("Tone mapping:"), tonemap_);
  dv->addRow(tr("Contrast:"), contrast_);
  dv->addRow(tr("Saturation:"), saturation_);
  auto* saveDisp = new QPushButton(tr("Save Display Settings to Scene"), disp);
  saveDisp->setToolTip(tr("Stores exposure, corrections, lens effects and Light Mix in the scene (used by later renders)"));
  dv->addRow(saveDisp);
  tabs_->addTab(scrollPage(disp), tr("Display"));

  // Corrections tab.
  auto* cc = new QWidget;
  auto* cv = new QFormLayout(cc);
  ccOn_ = new QCheckBox(tr("Enable color corrections"), cc);
  wbK_ = spin(cc, 1000, 40000, 100, 0, " K");
  tint_ = spin(cc, -1, 1, 0.01, 2);
  hue_ = spin(cc, -180, 180, 1, 1, " deg");
  burn_ = spin(cc, 0, 1, 0.05, 2);
  levelsBlack_ = spin(cc, 0, 1, 0.01, 3);
  levelsWhite_ = spin(cc, 0, 1, 0.01, 3);
  levelsGamma_ = spin(cc, 0.1, 10, 0.01, 3);
  curve_ = new CurveWidget(cc);
  auto* lutRow = new QHBoxLayout;
  lutFile_ = new QLineEdit(cc);
  lutFile_->setPlaceholderText(tr("No LUT"));
  auto* lutBrowse = new QPushButton(tr("..."), cc);
  lutBrowse->setMaximumWidth(30);
  lutRow->addWidget(lutFile_);
  lutRow->addWidget(lutBrowse);
  lutStrength_ = spin(cc, 0, 1, 0.05, 2);
  auto* curveReset = new QPushButton(tr("Reset Curve"), cc);
  cv->addRow(ccOn_);
  cv->addRow(tr("White balance:"), wbK_);
  cv->addRow(tr("Tint (green/magenta):"), tint_);
  cv->addRow(tr("Hue shift:"), hue_);
  cv->addRow(tr("Highlight burn:"), burn_);
  cv->addRow(tr("Levels black:"), levelsBlack_);
  cv->addRow(tr("Levels white:"), levelsWhite_);
  cv->addRow(tr("Levels gamma:"), levelsGamma_);
  cv->addRow(tr("Curve:"), curve_);
  cv->addRow(curveReset);
  cv->addRow(tr("LUT (.cube):"), lutRow);
  cv->addRow(tr("LUT strength:"), lutStrength_);
  tabs_->addTab(scrollPage(cc), tr("Corrections"));

  // Lens tab.
  auto* lens = new QWidget;
  auto* lv = new QFormLayout(lens);
  bloomI_ = spin(lens, 0, 10, 0.01, 3);
  bloomS_ = spin(lens, 0.001, 1, 0.005, 3);
  glareI_ = spin(lens, 0, 10, 0.01, 3);
  glareS_ = spin(lens, 0.001, 1, 0.01, 3);
  glareRays_ = new QSpinBox(lens);
  glareRays_->setRange(0, 16);
  glareRot_ = spin(lens, -360, 360, 1, 1, " deg");
  vignette_ = spin(lens, 0, 1, 0.05, 2);
  lv->addRow(new QLabel(tr("Turn on with the Lens FX button."), lens));
  lv->addRow(tr("Bloom intensity:"), bloomI_);
  lv->addRow(tr("Bloom size:"), bloomS_);
  lv->addRow(tr("Glare intensity:"), glareI_);
  lv->addRow(tr("Glare size:"), glareS_);
  lv->addRow(tr("Glare rays:"), glareRays_);
  lv->addRow(tr("Glare rotation:"), glareRot_);
  lv->addRow(tr("Vignetting:"), vignette_);
  tabs_->addTab(scrollPage(lens), tr("Lens"));

  // Light Mix tab.
  lightMixPanel_ = new QWidget;
  auto* lmv = new QVBoxLayout(lightMixPanel_);
  lightMixOn_ = new QCheckBox(tr("Use Light Mix"), lightMixPanel_);
  lmv->addWidget(lightMixOn_);
  lightMixRows_ = new QVBoxLayout;
  lmv->addLayout(lightMixRows_);
  auto* lmButtons = new QHBoxLayout;
  auto* lmReset = new QPushButton(tr("Reset"), lightMixPanel_);
  lmButtons->addWidget(lmReset);
  lmButtons->addStretch();
  lmv->addLayout(lmButtons);
  lmv->addStretch();
  tabs_->addTab(scrollPage(lightMixPanel_), tr("Light Mix"));

  // History tab.
  auto* hist = new QWidget;
  auto* hv = new QVBoxLayout(hist);
  history_ = new QListWidget(hist);
  history_->setIconSize(QSize(96, 54));
  hv->addWidget(history_, 1);
  auto* hb = new QHBoxLayout;
  auto* hSave = new QPushButton(tr("Save to History"), hist);
  auto* hRemove = new QPushButton(tr("Remove"), hist);
  hb->addWidget(hSave);
  hb->addWidget(hRemove);
  hv->addLayout(hb);
  hv->addWidget(new QLabel(tr("Select an entry and press A/B to compare."), hist));
  tabs_->addTab(hist, tr("History"));

  // Info tab.
  auto* info = new QWidget;
  auto* iv = new QVBoxLayout(info);
  stats_ = new QLabel(info);
  stats_->setWordWrap(true);
  stats_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  iv->addWidget(stats_);
  log_ = new QListWidget(info);
  iv->addWidget(log_, 1);
  tabs_->addTab(info, tr("Info"));

  dock->setWidget(tabs_);
  dock->setMinimumWidth(320);
  addDockWidget(Qt::RightDockWidgetArea, dock);

  pixelInfo_ = new QLabel(this);
  statusBar()->addPermanentWidget(pixelInfo_, 1);

  // ---- Connections ----
  auto changed = [this] { redisplay(); };
  for (QDoubleSpinBox* s : {exposure_, contrast_, saturation_, wbK_, tint_, hue_, burn_, levelsBlack_, levelsWhite_,
                            levelsGamma_, lutStrength_, bloomI_, bloomS_, glareI_, glareS_, glareRot_, vignette_})
    connect(s, &QDoubleSpinBox::valueChanged, this, changed);
  connect(glareRays_, &QSpinBox::valueChanged, this, changed);
  connect(tonemap_, &QComboBox::currentIndexChanged, this, changed);
  connect(channel_, &QComboBox::currentIndexChanged, this, changed);
  connect(element_, &QComboBox::currentIndexChanged, this, changed);
  connect(ccOn_, &QCheckBox::toggled, this, changed);
  connect(lightMixOn_, &QCheckBox::toggled, this, changed);
  connect(lutFile_, &QLineEdit::editingFinished, this, changed);
  connect(curve_, &CurveWidget::edited, this, changed);
  connect(curveReset, &QPushButton::clicked, this, [this] {
    curve_->setPoints({{0, 0}, {1, 1}});
    redisplay();
  });
  connect(lutBrowse, &QPushButton::clicked, this, [this] {
    QString f = QFileDialog::getOpenFileName(this, tr("Load LUT"), QString(), tr("Cube LUT (*.cube);;All files (*)"));
    if (f.isEmpty()) return;
    lutFile_->setText(f);
    ccOn_->setChecked(true);
    redisplay();
  });
  connect(denoiseAct_, &QAction::toggled, this, changed);
  connect(lensAct_, &QAction::toggled, this, changed);
  connect(regionAct_, &QAction::toggled, this, [this](bool on) {
    view_->setRegionMode(on);
    if (!on) view_->setRegion(false, QRectF());
  });
  connect(compareAct_, &QAction::toggled, this, [this](bool on) {
    int row = history_->currentRow();
    if (on && row >= 0 && row < static_cast<int>(historyEntries_.size())) view_->setCompare(historyEntries_[row].image);
    else {
      view_->clearCompare();
      if (on) {
        QSignalBlocker b(compareAct_);
        compareAct_->setChecked(false);
        log_->addItem(tr("Select a history entry first"));
      }
    }
  });
  connect(history_, &QListWidget::currentRowChanged, this, [this](int row) {
    if (compareAct_->isChecked() && row >= 0 && row < static_cast<int>(historyEntries_.size()))
      view_->setCompare(historyEntries_[row].image);
  });
  connect(hSave, &QPushButton::clicked, this, &FrameBufferWindow::saveToHistory);
  connect(hRemove, &QPushButton::clicked, this, [this] {
    int row = history_->currentRow();
    if (row < 0 || row >= static_cast<int>(historyEntries_.size())) return;
    historyEntries_.erase(historyEntries_.begin() + row);
    delete history_->takeItem(row);
    view_->clearCompare();
    compareAct_->setChecked(false);
  });
  connect(lmReset, &QPushButton::clicked, this, [this] {
    for (auto& r : mixRows_) {
      QSignalBlocker b1(r.on), b2(r.intensity);
      r.on->setChecked(true);
      r.intensity->setValue(1.0);
      r.color->setColor(Vec3(1.0f));
    }
    redisplay();
  });
  connect(saveDisp, &QPushButton::clicked, this, &FrameBufferWindow::saveDisplayToScene);
  connect(view_, &ImageView::pixelHovered, this, &FrameBufferWindow::updatePixelInfo);

  timer_.setInterval(200);
  connect(&timer_, &QTimer::timeout, this, &FrameBufferWindow::tick);
  stopAct_->setEnabled(false);
  stats_->setText(tr("No render yet."));
  loadDisplay(DisplaySettings());
  rebuildElements();
}

FrameBufferWindow::~FrameBufferWindow() { renderer_.stop(); }

const QImage& FrameBufferWindow::image() const { return view_->image(); }

void FrameBufferWindow::loadDisplay(const DisplaySettings& d) {
  loading_ = true;
  QSignalBlocker b1(exposure_), b2(tonemap_), b3(contrast_), b4(saturation_), b5(denoiseAct_), b6(ccOn_), b7(wbK_),
      b8(tint_), b9(hue_), b10(burn_), b11(levelsBlack_), b12(levelsWhite_), b13(levelsGamma_), b14(lutStrength_),
      b15(bloomI_), b16(bloomS_), b17(glareI_), b18(glareS_), b19(glareRays_), b20(glareRot_), b21(vignette_),
      b22(lensAct_), b23(lightMixOn_);
  exposure_->setValue(d.exposureEV);
  tonemap_->setCurrentIndex(d.tonemap);
  contrast_->setValue(d.contrast);
  saturation_->setValue(d.saturation);
  denoiseAct_->setChecked(d.denoise && Renderer::denoiserAvailable());
  ccOn_->setChecked(d.corrections);
  wbK_->setValue(d.whiteBalanceK);
  tint_->setValue(d.tint);
  hue_->setValue(d.hueShift);
  burn_->setValue(d.highlightBurn);
  levelsBlack_->setValue(d.levelsBlack);
  levelsWhite_->setValue(d.levelsWhite);
  levelsGamma_->setValue(d.levelsGamma);
  curve_->setPoints(d.curve);
  lutFile_->setText(QString::fromStdString(d.lutFile));
  lutStrength_->setValue(d.lutStrength);
  lensAct_->setChecked(d.lensEffects);
  bloomI_->setValue(d.bloomIntensity);
  bloomS_->setValue(d.bloomSize);
  glareI_->setValue(d.glareIntensity);
  glareS_->setValue(d.glareSize);
  glareRays_->setValue(d.glareRays);
  glareRot_->setValue(d.glareRotation);
  vignette_->setValue(d.vignetting);
  lightMixOn_->setChecked(d.useLightMix);
  loading_ = false;
}

DisplaySettings FrameBufferWindow::display() const {
  DisplaySettings d;
  d.exposureEV = static_cast<float>(exposure_->value());
  d.tonemap = tonemap_->currentIndex();
  d.contrast = static_cast<float>(contrast_->value());
  d.saturation = static_cast<float>(saturation_->value());
  d.denoise = denoiseAct_->isChecked() && Renderer::denoiserAvailable();
  d.corrections = ccOn_->isChecked();
  d.whiteBalanceK = static_cast<float>(wbK_->value());
  d.tint = static_cast<float>(tint_->value());
  d.hueShift = static_cast<float>(hue_->value());
  d.highlightBurn = static_cast<float>(burn_->value());
  d.levelsBlack = static_cast<float>(levelsBlack_->value());
  d.levelsWhite = static_cast<float>(levelsWhite_->value());
  d.levelsGamma = static_cast<float>(levelsGamma_->value());
  d.curve = curve_->points();
  d.lutFile = lutFile_->text().toStdString();
  d.lutStrength = static_cast<float>(lutStrength_->value());
  d.lensEffects = lensAct_->isChecked();
  d.bloomIntensity = static_cast<float>(bloomI_->value());
  d.bloomSize = static_cast<float>(bloomS_->value());
  d.glareIntensity = static_cast<float>(glareI_->value());
  d.glareSize = static_cast<float>(glareS_->value());
  d.glareRays = glareRays_->value();
  d.glareRotation = static_cast<float>(glareRot_->value());
  d.vignetting = static_cast<float>(vignette_->value());
  d.useLightMix = lightMixOn_->isChecked() && !mixRows_.empty();
  for (const auto& r : mixRows_) d.lightMix.push_back({r.name.toStdString(), r.color->color(), static_cast<float>(r.intensity->value()), r.on->isChecked()});
  QString e = element_->currentData().toString();
  if (!e.isEmpty() && e != "SampleRate") d.element = e.toStdString();
  return d;
}

void FrameBufferWindow::redisplay() {
  if (loading_) return;
  resolveImage(!renderer_.isRunning());
}

void FrameBufferWindow::rebuildElements() {
  QString cur = element_->currentData().toString();
  QSignalBlocker b(element_);
  element_->clear();
  element_->addItem(tr("Beauty (RGB)"), QString());
  const auto& infos = renderElementInfos();
  for (const auto& name : renderer_.elementNames())
    for (const auto& info : infos)
      if (name == info.key) element_->addItem(tr(info.label), QString::fromStdString(name));
  element_->addItem(tr("Sample rate"), QString("SampleRate"));
  int idx = element_->findData(cur);
  element_->setCurrentIndex(idx >= 0 ? idx : 0);
}

void FrameBufferWindow::rebuildLightMix() {
  // Keep the user's settings for groups that still exist.
  std::map<QString, std::tuple<bool, Vec3, double>> keep;
  for (const auto& r : mixRows_) keep[r.name] = {r.on->isChecked(), r.color->color(), r.intensity->value()};
  while (QLayoutItem* it = lightMixRows_->takeAt(0)) {
    if (it->widget()) it->widget()->deleteLater();
    delete it;
  }
  mixRows_.clear();
  std::vector<std::string> groups = renderer_.lightGroupNames();
  if (groups.empty()) {
    auto* l = new QLabel(tr("Turn on Light Mix in Render Setup > Render Elements and render again to adjust lights "
                            "after rendering."),
                         lightMixPanel_);
    l->setWordWrap(true);
    lightMixRows_->addWidget(l);
    return;
  }
  for (const auto& g : groups) {
    QString name = QString::fromStdString(g);
    auto* row = new QWidget(lightMixPanel_);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    MixRow r;
    r.name = name;
    r.on = new QCheckBox(name, row);
    r.on->setChecked(true);
    r.color = new ColorSwatch(row);
    r.color->setColor(Vec3(1.0f));
    r.color->setMaximumWidth(36);
    r.intensity = spin(row, 0, 1000, 0.1, 2);
    r.intensity->setValue(1.0);
    auto it = keep.find(name);
    if (it != keep.end()) {
      r.on->setChecked(std::get<0>(it->second));
      r.color->setColor(std::get<1>(it->second));
      r.intensity->setValue(std::get<2>(it->second));
    }
    h->addWidget(r.on, 1);
    h->addWidget(r.color);
    h->addWidget(r.intensity);
    lightMixRows_->addWidget(row);
    connect(r.on, &QCheckBox::toggled, this, [this] {
      lightMixOn_->setChecked(true);
      redisplay();
    });
    connect(r.intensity, &QDoubleSpinBox::valueChanged, this, [this] {
      lightMixOn_->setChecked(true);
      redisplay();
    });
    connect(r.color, &ColorSwatch::colorEdited, this, [this](const Vec3&) {
      lightMixOn_->setChecked(true);
      redisplay();
    });
    mixRows_.push_back(r);
  }
}

void FrameBufferWindow::startRender(const Scene& scene, const RenderView& view, const QString& viewName) {
  renderer_.stop();
  log_->clear();
  viewName_ = viewName;
  std::string err;
  QElapsedTimer t;
  t.start();
  // Region and render-selected from the frame buffer tools.
  QRectF r = view_->region();
  renderer_.setRegion(regionAct_->isChecked() && view_->hasRegion(), static_cast<float>(r.left()),
                      static_cast<float>(r.top()), static_cast<float>(r.right()), static_cast<float>(r.bottom()));
  std::vector<uint32_t> only;
  if (selectedAct_->isChecked())
    if (auto* mw = qobject_cast<MainWindow*>(parentWidget()))
      for (uint32_t id : mw->document()->selection()) only.push_back(id);
  if (selectedAct_->isChecked() && only.empty()) log_->addItem(tr("Render Selected is on but nothing is selected"));
  renderer_.setRenderOnly(only);
  renderer_.setView(view);  // before setScene: motion blur needs the shutter
  if (!renderer_.setScene(scene, &err)) {
    log_->addItem(tr("Error: %1").arg(QString::fromStdString(err)));
    stats_->setText(tr("Render failed."));
    return;
  }
  for (const auto& w : renderer_.warnings()) log_->addItem(tr("Warning: %1").arg(QString::fromStdString(w)));
  log_->addItem(tr("Scene prepared in %1 ms").arg(t.elapsed()));
  DisplaySettings d = displaySettingsFromScene(scene);
  loadDisplay(d);
  setWindowTitle(tr("Rendered Frame Window - %1 - Veraa").arg(viewName));
  finalized_ = false;
  renderer_.start();
  rebuildElements();
  rebuildLightMix();
  // Apply the scene's saved Light Mix values to the new rows.
  for (auto& row : mixRows_)
    for (const auto& m : d.lightMix)
      if (row.name == QString::fromStdString(m.name)) {
        QSignalBlocker b1(row.on), b2(row.intensity);
        row.on->setChecked(m.enabled);
        row.intensity->setValue(m.intensity);
        row.color->setColor(m.color);
      }
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
  stats_->setText(tr("View: %1\nResolution: %2 x %3\nSamples: %4 / %5\nConverged: %6%\nTime: %7:%8\nSpeed: %9 M samples/s\n"
                     "Triangles: %10\nDenoiser: %11")
                      .arg(viewName_)
                      .arg(renderer_.width())
                      .arg(renderer_.height())
                      .arg(s.passes)
                      .arg(s.targetPasses)
                      .arg(static_cast<int>(s.converged * 100.0f))
                      .arg(secs / 60, 2, 10, QChar('0'))
                      .arg(secs % 60, 2, 10, QChar('0'))
                      .arg(s.samplesPerSecond * 1e-6, 0, 'f', 2)
                      .arg(QLocale().toString(static_cast<qulonglong>(s.triangles)))
                      .arg(Renderer::denoiserAvailable() ? tr("available") : tr("not available")));
  statusBar()->showMessage(s.running ? tr("Rendering: %1 samples, %2% converged, %3 s").arg(s.passes).arg(static_cast<int>(s.converged * 100)).arg(s.seconds, 0, 'f', 1)
                                     : tr("Done: %1 samples in %2 s").arg(s.passes).arg(s.seconds, 0, 'f', 1));
  view_->setBuckets(s.buckets);
  bool running = renderer_.isRunning();
  if (running) {
    resolveImage(false);
    return;
  }
  timer_.stop();
  stopAct_->setEnabled(false);
  view_->setBuckets({});
  if (!finalized_) {
    finalized_ = true;
    resolveImage(true);
    log_->addItem(tr("Finished: %1 samples in %2 s").arg(s.passes).arg(s.seconds, 0, 'f', 1));
  }
}

void FrameBufferWindow::resolveImage(bool final) {
  int w = renderer_.width(), h = renderer_.height();
  if (w <= 0 || h <= 0) return;
  DisplaySettings d = display();
  if (!final) d.denoise = false;  // denoise only when the render is done (it is slow)
  std::vector<uint8_t> rgba;
  QString e = element_->currentData().toString();
  renderer_.resolveFinalHdr(d, hdr_);
  renderer_.resolveAlpha(alpha_);
  renderer_.resolveSampleCounts(counts_);
  if (e == "SampleRate") {
    float mx = 1.0f;
    for (float c : counts_) mx = std::max(mx, c);
    rgba.resize(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < counts_.size(); ++i) {
      float t = counts_[i] / mx;
      QColor c = QColor::fromHsvF(std::max(0.0f, 0.66f * (1.0f - t)), 0.9f, 0.25f + 0.75f * t);
      rgba[i * 4] = static_cast<uint8_t>(c.red());
      rgba[i * 4 + 1] = static_cast<uint8_t>(c.green());
      rgba[i * 4 + 2] = static_cast<uint8_t>(c.blue());
      rgba[i * 4 + 3] = 255;
    }
  } else {
    renderer_.resolve(d, rgba);
  }
  if (rgba.size() != static_cast<size_t>(w) * h * 4) return;
  int ch = channel_->currentIndex();
  if (ch > 0)
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
      uint8_t* p = &rgba[i * 4];
      uint8_t v = ch == 1 ? p[0] : ch == 2 ? p[1] : ch == 3 ? p[2]
                 : ch == 4 ? static_cast<uint8_t>(std::lround(std::clamp(i < alpha_.size() ? alpha_[i] : 1.0f, 0.0f, 1.0f) * 255.0f))
                           : static_cast<uint8_t>((p[0] * 54 + p[1] * 183 + p[2] * 19) >> 8);
      p[0] = p[1] = p[2] = v;
    }
  QImage img(rgba.data(), w, h, w * 4, QImage::Format_RGBA8888);
  displayed_ = img.convertToFormat(QImage::Format_RGB32);
  view_->setImage(displayed_);
}

void FrameBufferWindow::updatePixelInfo(int x, int y) {
  int w = renderer_.width(), h = renderer_.height();
  if (!trackAct_->isChecked() || x < 0 || y < 0 || x >= w || y >= h || hdr_.size() < static_cast<size_t>(w) * h * 3) {
    pixelInfo_->clear();
    return;
  }
  size_t i = static_cast<size_t>(y) * w + x;
  const float* p = &hdr_[i * 3];
  pixelInfo_->setText(tr("[%1, %2]   R %3  G %4  B %5   A %6   samples %7")
                          .arg(x)
                          .arg(y)
                          .arg(p[0], 0, 'f', 4)
                          .arg(p[1], 0, 'f', 4)
                          .arg(p[2], 0, 'f', 4)
                          .arg(i < alpha_.size() ? alpha_[i] : 1.0f, 0, 'f', 3)
                          .arg(i < counts_.size() ? static_cast<int>(counts_[i]) : 0));
}

bool FrameBufferWindow::saveImageTo(const QString& path, bool withAlpha) {
  int w = renderer_.width(), h = renderer_.height();
  DisplaySettings d = display();
  std::vector<uint8_t> rgba;
  renderer_.resolve(d, rgba);
  std::vector<float> hdr, alpha;
  renderer_.resolveFinalHdr(d, hdr);
  renderer_.resolveAlpha(alpha);
  std::string err;
  bool ok = saveImage(path.toStdString(), w, h, rgba, &hdr, &alpha, withAlpha, &err);
  if (!ok) log_->addItem(tr("Error: %1").arg(QString::fromStdString(err)));
  else log_->addItem(tr("Saved %1").arg(QFileInfo(path).fileName()));
  return ok;
}

void FrameBufferWindow::saveImageDialog() {
  if (view_->image().isNull()) return;
  QString selected;
  QString path = QFileDialog::getSaveFileName(
      this, tr("Save Rendered Image"), QString(),
      tr("PNG (*.png);;PNG with alpha (*.png);;JPEG (*.jpg);;TIFF (*.tif);;TIFF with alpha (*.tif);;OpenEXR (*.exr);;"
         "OpenEXR with alpha (*.exr);;Radiance HDR (*.hdr);;Bitmap (*.bmp);;Targa (*.tga)"),
      &selected);
  if (path.isEmpty()) return;
  if (QFileInfo(path).suffix().isEmpty()) {
    QString ext = selected.section("*.", 1).section(')', 0, 0);
    path += "." + ext;
  }
  bool alpha = selected.contains("alpha");
  if (!saveImageTo(path, alpha)) QMessageBox::warning(this, tr("Save Image"), tr("Could not save %1").arg(path));
}

void FrameBufferWindow::saveAllElements() {
  std::vector<std::string> names = renderer_.elementNames();
  std::vector<std::string> groups = renderer_.lightGroupNames();
  if (names.empty() && groups.empty()) {
    QMessageBox::information(this, tr("Save Elements"), tr("No render elements were rendered. Turn them on in Render Setup."));
    return;
  }
  QString dir = QFileDialog::getExistingDirectory(this, tr("Save Render Elements To"));
  if (dir.isEmpty()) return;
  std::string err;
  int saved = saveRenderElements(renderer_, display(), dir.toStdString(), "", {".png", ".exr"}, nullptr, &err);
  if (!err.empty()) log_->addItem(QString::fromStdString(err));
  log_->addItem(tr("Saved %1 element files (PNG and EXR) to %2").arg(saved).arg(dir));
}

void FrameBufferWindow::copyToClipboard() {
  if (view_->image().isNull()) return;
  QApplication::clipboard()->setImage(view_->image());
  log_->addItem(tr("Image copied to the clipboard"));
}

void FrameBufferWindow::saveToHistory() {
  if (view_->image().isNull()) return;
  HistoryEntry e;
  e.image = view_->image();
  RenderStats s = renderer_.stats();
  e.label = tr("%1  %2  (%3 spp, %4 s)")
                .arg(QTime::currentTime().toString("HH:mm:ss"))
                .arg(viewName_)
                .arg(s.passes)
                .arg(s.seconds, 0, 'f', 1);
  historyEntries_.insert(historyEntries_.begin(), e);
  auto* item = new QListWidgetItem(QIcon(QPixmap::fromImage(e.image.scaled(96, 54, Qt::KeepAspectRatio, Qt::SmoothTransformation))), e.label);
  history_->insertItem(0, item);
  while (historyEntries_.size() > 30) {
    historyEntries_.pop_back();
    delete history_->takeItem(history_->count() - 1);
  }
  history_->setCurrentRow(0);
}

int FrameBufferWindow::historySize() const { return static_cast<int>(historyEntries_.size()); }

QStringList FrameBufferWindow::elementList() const {
  QStringList out;
  for (const auto& n : renderer_.elementNames()) out << QString::fromStdString(n);
  return out;
}

QStringList FrameBufferWindow::lightGroups() const {
  QStringList out;
  for (const auto& n : renderer_.lightGroupNames()) out << QString::fromStdString(n);
  return out;
}

void FrameBufferWindow::showElement(const QString& name) {
  int idx = element_->findData(name);
  element_->setCurrentIndex(idx >= 0 ? idx : 0);
}

void FrameBufferWindow::setLightMixIntensity(const QString& group, float intensity) {
  for (auto& r : mixRows_)
    if (r.name == group) r.intensity->setValue(intensity);
  lightMixOn_->setChecked(true);
  redisplay();
}

void FrameBufferWindow::saveDisplayToScene() {
  auto* mw = qobject_cast<MainWindow*>(parentWidget());
  if (!mw) return;
  SceneDocument* doc = mw->document();
  DisplaySettings d = display();
  doc->beginEdit(tr("Save Display Settings"));
  ParamBlock& p = doc->scene().renderSettings;
  p.setNumber("exposure", d.exposureEV);
  p.setNumber("tonemap", d.tonemap);
  p.setNumber("contrast", d.contrast);
  p.setNumber("saturation", d.saturation);
  p.setNumber("denoise", d.denoise ? 1 : 0);
  p.setNumber("ccOn", d.corrections ? 1 : 0);
  p.setNumber("ccWhiteBalanceK", d.whiteBalanceK);
  p.setNumber("ccTint", d.tint);
  p.setNumber("ccHue", d.hueShift);
  p.setNumber("ccHighlightBurn", d.highlightBurn);
  QStringList pts;
  for (const auto& v : d.curve) pts << QString("%1,%2").arg(v.x, 0, 'f', 4).arg(v.y, 0, 'f', 4);
  p.setString("ccCurve", pts.join(";").toStdString());
  p.setNumber("ccLevelsBlack", d.levelsBlack);
  p.setNumber("ccLevelsWhite", d.levelsWhite);
  p.setNumber("ccLevelsGamma", d.levelsGamma);
  p.setString("ccLutFile", d.lutFile);
  p.setNumber("ccLutStrength", d.lutStrength);
  p.setNumber("lensEffects", d.lensEffects ? 1 : 0);
  p.setNumber("bloomIntensity", d.bloomIntensity);
  p.setNumber("bloomSize", d.bloomSize);
  p.setNumber("glareIntensity", d.glareIntensity);
  p.setNumber("glareSize", d.glareSize);
  p.setNumber("glareRays", d.glareRays);
  p.setNumber("glareRotation", d.glareRotation);
  p.setNumber("vignetting", d.vignetting);
  json lm{{"enabled", d.useLightMix}};
  json groups = json::array();
  for (const auto& g : d.lightMix)
    groups.push_back({{"name", g.name}, {"color", vec3ToJson(g.color)}, {"intensity", g.intensity}, {"enabled", g.enabled}});
  lm["groups"] = groups;
  doc->scene().extensions["lightMix"] = lm;
  doc->notifyRenderSettingsChanged();
  doc->endEdit();
  log_->addItem(tr("Display settings saved to the scene"));
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
