#include "renderanim.h"

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

#include <cmath>
#include <cstring>
#include <fstream>

#include "animcontroller.h"
#include "avi.h"
#include "extension.h"
#include "mainwindow.h"
#include "renderer.h"
#include "viewport.h"
#include "viewportpanel.h"

namespace veraa {

std::vector<double> parseFrameList(const QString& text, int nth) {
  std::vector<double> out;
  nth = std::max(1, nth);
  for (QString part : text.split(',', Qt::SkipEmptyParts)) {
    part = part.trimmed();
    int dash = part.indexOf('-', 1);
    bool ok1 = false, ok2 = false;
    if (dash > 0) {
      int a = part.left(dash).trimmed().toInt(&ok1), b = part.mid(dash + 1).trimmed().toInt(&ok2);
      if (!ok1 || !ok2) continue;
      if (a <= b)
        for (int f = a; f <= b; f += nth) out.push_back(f);
      else
        for (int f = a; f >= b; f -= nth) out.push_back(f);
    } else {
      int f = part.toInt(&ok1);
      if (ok1) out.push_back(f);
    }
  }
  return out;
}

QString sequenceFileName(const QString& base, int frame) {
  QFileInfo fi(base);
  QString suffix = fi.suffix().isEmpty() ? QStringLiteral("png") : fi.suffix();
  QString stem = fi.completeBaseName();
  return QDir(fi.absolutePath()).filePath(QStringLiteral("%1%2.%3").arg(stem).arg(frame, 4, 10, QChar('0')).arg(suffix));
}

namespace {

bool writeBytes(const QString& path, const std::vector<uint8_t>& data) {
  std::ofstream f(QFile::encodeName(path).toStdString(), std::ios::binary);
  if (!f) return false;
  f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  return static_cast<bool>(f);
}

}  // namespace

int renderAnimation(MainWindow& w, const RenderAnimOptions& o, const std::function<bool(int, int)>& progress, QString* error) {
  AnimController* c = AnimController::instance();
  if (!c || o.frames.empty() || o.path.isEmpty()) {
    if (error) *error = QObject::tr("Nothing to render.");
    return 0;
  }
  Scene& s = c->scene();
  double original = c->frame();
  int width = o.width > 0 ? o.width : s.renderSettings.getInt("width");
  int height = o.height > 0 ? o.height : s.renderSettings.getInt("height");
  QDir().mkpath(QFileInfo(o.path).absolutePath());
  AviWriter avi;
  if (o.format == 2 && !avi.open(QFile::encodeName(o.path).toStdString(), width, height, c->fps(), o.quality)) {
    if (error) *error = QString::fromStdString(avi.error());
    return 0;
  }
  int written = 0;
  int total = static_cast<int>(o.frames.size());
  bool cancelled = false;
  for (int i = 0; i < total && !cancelled; ++i) {
    double f = o.frames[static_cast<size_t>(i)];
    if (Animation* anim = c->animation()) anim->evaluate(s, f);
    else s.time.currentFrame = f;
    Renderer r;
    std::string err;
    if (!r.setScene(s, &err)) {
      if (error) *error = QString::fromStdString(err);
      break;
    }
    r.setView(w.renderView(nullptr));
    r.setResolution(width, height);
    if (o.samples > 0) r.setMaxSamples(o.samples);
    if (o.timeLimit > 0) r.setTimeLimit(o.timeLimit);
    r.start();
    while (r.isRunning()) {
      QApplication::processEvents(QEventLoop::AllEvents, 30);
      if (progress && !progress(i, total)) {
        r.stop();
        cancelled = true;
        break;
      }
      QThread::msleep(5);
    }
    if (cancelled) break;
    std::vector<uint8_t> rgba;
    r.resolve(displaySettingsFromScene(s), rgba);
    int rw = r.width(), rh = r.height();
    bool ok = false;
    int number = static_cast<int>(std::lround(f)) + o.numberBase;
    if (o.format == 0) {
      ok = writePng(QFile::encodeName(sequenceFileName(o.path, number)).toStdString(), rw, rh, rgba);
    } else if (o.format == 1) {
      std::vector<uint8_t> jpg;
      ok = encodeJpeg(rgba.data(), rw, rh, 4, o.quality, jpg) && writeBytes(sequenceFileName(o.path, number), jpg);
    } else {
      ok = avi.addFrame(rgba.data(), 4);
    }
    if (!ok) {
      if (error) *error = QObject::tr("Could not write frame %1.").arg(f);
      break;
    }
    ++written;
    if (progress && !progress(i + 1, total)) cancelled = true;
  }
  if (o.format == 2) avi.close();
  c->setFrame(original);
  return written;
}

int createViewportPreview(MainWindow& w, const std::vector<double>& frames, const QString& path, int percentSize,
                          const std::function<bool(int, int)>& progress, QString* error) {
  AnimController* c = AnimController::instance();
  Viewport* vp = w.viewports() ? w.viewports()->active() : nullptr;
  if (!c || !vp || frames.empty()) {
    if (error) *error = QObject::tr("Nothing to preview.");
    return 0;
  }
  double original = c->frame();
  bool png = path.endsWith(".png", Qt::CaseInsensitive);
  AviWriter avi;
  QDir().mkpath(QFileInfo(path).absolutePath());
  int written = 0;
  int total = static_cast<int>(frames.size());
  for (int i = 0; i < total; ++i) {
    c->setFrame(frames[static_cast<size_t>(i)]);
    QApplication::processEvents();
    QImage img = vp->grabFramebuffer();
    if (img.isNull()) {
      if (error) *error = QObject::tr("Could not grab the viewport.");
      break;
    }
    if (percentSize > 0 && percentSize != 100)
      img = img.scaled(std::max(16, img.width() * percentSize / 100), std::max(16, img.height() * percentSize / 100),
                       Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    // Even sizes keep video players happy.
    img = img.copy(0, 0, img.width() & ~1, img.height() & ~1).convertToFormat(QImage::Format_RGB888);
    if (png) {
      if (!img.save(sequenceFileName(path, static_cast<int>(std::lround(frames[static_cast<size_t>(i)]))))) break;
    } else {
      if (!avi.isOpen() && !avi.open(QFile::encodeName(path).toStdString(), img.width(), img.height(), c->fps(), 85)) {
        if (error) *error = QString::fromStdString(avi.error());
        break;
      }
      std::vector<uint8_t> rgb(static_cast<size_t>(img.width()) * img.height() * 3);
      for (int y = 0; y < img.height(); ++y)
        std::memcpy(&rgb[static_cast<size_t>(y) * img.width() * 3], img.constScanLine(y), static_cast<size_t>(img.width()) * 3);
      if (!avi.addFrame(rgb.data(), 3)) break;
    }
    ++written;
    if (progress && !progress(i + 1, total)) break;
  }
  avi.close();
  c->setFrame(original);
  return written;
}

namespace {

class RenderAnimDialog : public QDialog {
 public:
  RenderAnimDialog(MainWindow& w, AnimController* c) : QDialog(&w), w_(w), c_(c) {
    setObjectName("renderAnimationDialog");
    setWindowTitle(tr("Render Animation"));
    auto* v = new QVBoxLayout(this);
    auto* range = new QGroupBox(tr("Time Output"), this);
    auto* rf = new QFormLayout(range);
    single_ = new QRadioButton(tr("Single (current frame)"), range);
    active_ = new QRadioButton(tr("Active Time Segment: %1 To %2").arg(c->startFrame()).arg(c->endFrame()), range);
    rangeBtn_ = new QRadioButton(tr("Range:"), range);
    framesBtn_ = new QRadioButton(tr("Frames:"), range);
    active_->setChecked(true);
    from_ = new QSpinBox(range);
    to_ = new QSpinBox(range);
    for (QSpinBox* s : {from_, to_}) s->setRange(-100000, 100000);
    from_->setValue(c->startFrame());
    to_->setValue(c->endFrame());
    nth_ = new QSpinBox(range);
    nth_->setRange(1, 1000);
    list_ = new QLineEdit(QStringLiteral("%1-%2").arg(c->startFrame()).arg(std::min(c->endFrame(), c->startFrame() + 10)), range);
    numberBase_ = new QSpinBox(range);
    numberBase_->setRange(-100000, 100000);
    auto* rangeRow = new QHBoxLayout;
    rangeRow->addWidget(from_);
    rangeRow->addWidget(new QLabel(tr("To"), range));
    rangeRow->addWidget(to_);
    rf->addRow(single_);
    rf->addRow(active_);
    rf->addRow(rangeBtn_, rangeRow);
    rf->addRow(framesBtn_, list_);
    rf->addRow(tr("Every Nth Frame:"), nth_);
    rf->addRow(tr("File Number Base:"), numberBase_);
    v->addWidget(range);

    auto* size = new QGroupBox(tr("Output Size and Quality"), this);
    auto* sf = new QFormLayout(size);
    width_ = new QSpinBox(size);
    height_ = new QSpinBox(size);
    for (QSpinBox* s : {width_, height_}) s->setRange(8, 16384);
    width_->setValue(c->scene().renderSettings.getInt("width"));
    height_->setValue(c->scene().renderSettings.getInt("height"));
    samples_ = new QSpinBox(size);
    samples_->setRange(0, 100000);
    samples_->setSpecialValueText(tr("Scene setting"));
    timeLimit_ = new QSpinBox(size);
    timeLimit_->setRange(0, 100000);
    timeLimit_->setSuffix(tr(" s"));
    timeLimit_->setSpecialValueText(tr("Scene setting"));
    auto* sizeRow = new QHBoxLayout;
    sizeRow->addWidget(width_);
    sizeRow->addWidget(new QLabel("x", size));
    sizeRow->addWidget(height_);
    sf->addRow(tr("Width x Height:"), sizeRow);
    sf->addRow(tr("Samples per Pixel:"), samples_);
    sf->addRow(tr("Time Limit per Frame:"), timeLimit_);
    v->addWidget(size);

    auto* out = new QGroupBox(tr("Render Output"), this);
    auto* of = new QFormLayout(out);
    format_ = new QComboBox(out);
    format_->addItems({tr("PNG Image Sequence"), tr("JPEG Image Sequence"), tr("AVI Video (Motion JPEG)")});
    quality_ = new QSpinBox(out);
    quality_->setRange(1, 100);
    quality_->setValue(90);
    path_ = new QLineEdit(out);
    QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (dir.isEmpty()) dir = QDir::homePath();
    path_->setText(QDir(dir).filePath("render.png"));
    auto* browse = new QPushButton(tr("Files..."), out);
    auto* pathRow = new QHBoxLayout;
    pathRow->addWidget(path_, 1);
    pathRow->addWidget(browse);
    of->addRow(tr("Format:"), format_);
    of->addRow(tr("Quality:"), quality_);
    of->addRow(tr("Save File:"), pathRow);
    v->addWidget(out);
    connect(format_, &QComboBox::currentIndexChanged, this, [this](int i) {
      QFileInfo fi(path_->text());
      const char* ext[] = {"png", "jpg", "avi"};
      path_->setText(QDir(fi.absolutePath()).filePath(fi.completeBaseName() + "." + ext[i]));
    });
    connect(browse, &QPushButton::clicked, this, [this] {
      const char* filters[] = {"PNG (*.png)", "JPEG (*.jpg)", "AVI (*.avi)"};
      QString f = QFileDialog::getSaveFileName(this, tr("Render Output"), path_->text(), tr(filters[format_->currentIndex()]));
      if (!f.isEmpty()) path_->setText(f);
    });

    preview_ = new QLabel(this);
    preview_->setMinimumHeight(140);
    preview_->setAlignment(Qt::AlignCenter);
    preview_->setStyleSheet("background: #202020;");
    v->addWidget(preview_);
    progress_ = new QProgressBar(this);
    status_ = new QLabel(this);
    v->addWidget(progress_);
    v->addWidget(status_);
    auto* bb = new QDialogButtonBox(this);
    renderBtn_ = bb->addButton(tr("Render"), QDialogButtonBox::ActionRole);
    cancelBtn_ = bb->addButton(tr("Cancel"), QDialogButtonBox::RejectRole);
    bb->addButton(QDialogButtonBox::Close);
    cancelBtn_->setEnabled(false);
    v->addWidget(bb);
    connect(bb, &QDialogButtonBox::rejected, this, [this] {
      if (running_) cancel_ = true;
      else reject();
    });
    connect(cancelBtn_, &QPushButton::clicked, this, [this] { cancel_ = true; });
    connect(renderBtn_, &QPushButton::clicked, this, [this] { run(); });
  }

  RenderAnimOptions options() const {
    RenderAnimOptions o;
    if (single_->isChecked()) o.frames = {c_->frame()};
    else if (active_->isChecked()) o.frames = parseFrameList(QStringLiteral("%1-%2").arg(c_->startFrame()).arg(c_->endFrame()), nth_->value());
    else if (rangeBtn_->isChecked()) o.frames = parseFrameList(QStringLiteral("%1-%2").arg(from_->value()).arg(to_->value()), nth_->value());
    else o.frames = parseFrameList(list_->text(), nth_->value());
    o.width = width_->value();
    o.height = height_->value();
    o.samples = samples_->value();
    o.timeLimit = timeLimit_->value();
    o.format = format_->currentIndex();
    o.quality = quality_->value();
    o.path = path_->text();
    o.numberBase = numberBase_->value();
    return o;
  }

  void run() {
    RenderAnimOptions o = options();
    if (o.frames.empty()) {
      QMessageBox::information(this, windowTitle(), tr("No frames to render."));
      return;
    }
    running_ = true;
    cancel_ = false;
    renderBtn_->setEnabled(false);
    cancelBtn_->setEnabled(true);
    progress_->setRange(0, static_cast<int>(o.frames.size()));
    QElapsedTimer t;
    t.start();
    QString err;
    int n = renderAnimation(w_, o, [&](int done, int total) {
      progress_->setValue(done);
      if (done > 0) {
        double per = t.elapsed() / 1000.0 / done;
        status_->setText(tr("Frame %1 of %2 - %3 s per frame, about %4 s left")
                             .arg(done)
                             .arg(total)
                             .arg(per, 0, 'f', 1)
                             .arg(per * (total - done), 0, 'f', 0));
        if (o.format != 2) {
          QPixmap pm(sequenceFileName(o.path, static_cast<int>(std::lround(o.frames[static_cast<size_t>(done - 1)])) + o.numberBase));
          if (!pm.isNull()) preview_->setPixmap(pm.scaled(preview_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
      }
      return !cancel_;
    }, &err);
    running_ = false;
    renderBtn_->setEnabled(true);
    cancelBtn_->setEnabled(false);
    status_->setText(err.isEmpty() ? tr("%1 frame(s) written to %2").arg(n).arg(QDir::toNativeSeparators(QFileInfo(o.path).absolutePath()))
                                   : err);
  }

 private:
  MainWindow& w_;
  AnimController* c_;
  QRadioButton *single_, *active_, *rangeBtn_, *framesBtn_;
  QSpinBox *from_, *to_, *nth_, *numberBase_, *width_, *height_, *samples_, *timeLimit_, *quality_;
  QLineEdit *list_, *path_;
  QComboBox* format_;
  QLabel *preview_, *status_;
  QProgressBar* progress_;
  QPushButton *renderBtn_, *cancelBtn_;
  bool running_ = false, cancel_ = false;
};

class PreviewDialog : public QDialog {
 public:
  PreviewDialog(MainWindow& w, AnimController* c) : QDialog(&w), w_(w), c_(c) {
    setWindowTitle(tr("Make Preview"));
    auto* f = new QFormLayout(this);
    from_ = new QSpinBox(this);
    to_ = new QSpinBox(this);
    for (QSpinBox* s : {from_, to_}) s->setRange(-100000, 100000);
    from_->setValue(c->startFrame());
    to_->setValue(c->endFrame());
    nth_ = new QSpinBox(this);
    nth_->setRange(1, 100);
    percent_ = new QSpinBox(this);
    percent_->setRange(10, 100);
    percent_->setValue(100);
    percent_->setSuffix("%");
    path_ = new QLineEdit(QDir(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation).isEmpty()
                                   ? QDir::homePath()
                                   : QStandardPaths::writableLocation(QStandardPaths::MoviesLocation))
                              .filePath("preview.avi"),
                          this);
    play_ = new QCheckBox(tr("Play when done"), this);
    play_->setChecked(true);
    f->addRow(tr("Start:"), from_);
    f->addRow(tr("End:"), to_);
    f->addRow(tr("Every Nth Frame:"), nth_);
    f->addRow(tr("Percent of Viewport:"), percent_);
    f->addRow(tr("Output (.avi or .png):"), path_);
    f->addRow(play_);
    progress_ = new QProgressBar(this);
    f->addRow(progress_);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    bb->button(QDialogButtonBox::Ok)->setText(tr("Create"));
    f->addRow(bb);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(bb, &QDialogButtonBox::accepted, this, [this] {
      std::vector<double> frames = parseFrameList(QStringLiteral("%1-%2").arg(from_->value()).arg(to_->value()), nth_->value());
      progress_->setRange(0, static_cast<int>(frames.size()));
      QString err;
      int n = createViewportPreview(w_, frames, path_->text(), percent_->value(), [this](int d, int) {
        progress_->setValue(d);
        return true;
      }, &err);
      if (n == 0) {
        QMessageBox::warning(this, windowTitle(), err.isEmpty() ? tr("No frames written.") : err);
        return;
      }
      if (play_->isChecked() && !path_->text().endsWith(".png", Qt::CaseInsensitive))
        QDesktopServices::openUrl(QUrl::fromLocalFile(path_->text()));
      accept();
    });
  }

 private:
  MainWindow& w_;
  AnimController* c_;
  QSpinBox *from_, *to_, *nth_, *percent_;
  QLineEdit* path_;
  QCheckBox* play_;
  QProgressBar* progress_;
};

void installRenderAnimation(MainWindow& w) {
  AnimController* c = AnimController::instance();
  if (!c) return;
  QMenu* r = w.menuAt("Rendering");
  r->addSeparator();
  QAction* ra = r->addAction(QObject::tr("Render Animation..."));
  w.registerCommand("render.animation", ra);
  QObject::connect(ra, &QAction::triggered, &w, [&w, c] {
    RenderAnimDialog d(w, c);
    d.exec();
  });
  QMenu* tools = w.menuAt("Tools");
  tools->addSeparator();
  QMenu* prev = tools->addMenu(QObject::tr("Preview - Grab Viewport"));
  QAction* make = prev->addAction(QObject::tr("Create Preview Animation..."));
  w.registerCommand("tools.makePreview", make);
  QObject::connect(make, &QAction::triggered, &w, [&w, c] {
    PreviewDialog d(w, c);
    d.exec();
  });
}

}  // namespace

VERAA_APP_EXTENSION(renderAnimation, 306, installRenderAnimation);

}  // namespace veraa
