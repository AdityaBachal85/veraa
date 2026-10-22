#include "renderdialogs.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <iterator>

#include "icons.h"
#include "rendersetup.h"

namespace veraa {

Node* addSunAndSky(SceneDocument* doc) {
  Scene& scene = doc->scene();
  Node* sun = nullptr;
  for (const auto& n : scene.nodes) {
    std::string t = n->object ? n->object->type() : std::string();
    if (t == "VeraaSun" || t == "SunPositioner") {
      sun = n.get();
      break;
    }
  }
  if (!sun) {
    sun = doc->createNode("SunPositioner");
    if (sun) doc->notifyNodeChanged(sun, ChangeAll);
  }
  ParamBlock& rs = scene.renderSettings;
  rs.setNumber("envMode", 3);  // physical sky
  if (rs.getInt("exposureControl") == 0) {
    rs.setNumber("exposureControl", 1);
    rs.setNumber("ev100", 14.0);
  }
  doc->notifyRenderSettingsChanged();
  return sun;
}

// ---------------------------------------------------------------------------------------------
// Environment and Effects

EnvironmentDialog::EnvironmentDialog(SceneDocument* doc, QWidget* parent) : QDialog(parent), doc_(doc) {
  setWindowTitle(tr("Environment and Effects"));
  setWindowFlag(Qt::WindowContextHelpButtonHint, false);
  resize(440, 720);
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(6, 6, 6, 6);
  tabs_ = new QTabWidget(this);
  tabs_->setDocumentMode(true);
  v->addWidget(tabs_, 1);

  auto scrolled = [](QWidget* w) {
    auto* s = new QScrollArea;
    s->setWidgetResizable(true);
    s->setFrameShape(QFrame::NoFrame);
    s->setWidget(w);
    return s;
  };

  auto* env = new QWidget;
  auto* ev = new QVBoxLayout(env);
  ev->setContentsMargins(4, 4, 4, 4);
  auto* sunSky = new QPushButton(tr("Add Sun and Sky (physical sky, physical exposure)"), env);
  ev->addWidget(sunSky);
  ev->addWidget(new RenderSettingsPanel(doc_, {"Environment"}, env));
  ev->addWidget(new RenderSettingsPanel(doc_, {"Exposure"}, env));
  ev->addWidget(new RenderSettingsPanel(doc_, {"Atmosphere"}, env));
  auto* note = new QLabel(tr("Fog fills the scene's bounding box. Height fog thins out exponentially above "
                             "the fog height; lights and the sun scatter in it (volumetric light)."),
                          env);
  note->setWordWrap(true);
  note->setStyleSheet("color: #a8a8a8;");
  ev->addWidget(note);
  ev->addStretch();
  tabs_->addTab(scrolled(env), tr("Environment"));

  auto* fx = new QWidget;
  auto* fv = new QVBoxLayout(fx);
  fv->setContentsMargins(4, 4, 4, 4);
  fv->addWidget(new RenderSettingsPanel(doc_, {"Lens effects", "Color corrections"}, fx));
  fv->addStretch();
  tabs_->addTab(scrolled(fx), tr("Effects"));

  auto* buttons = new QHBoxLayout;
  buttons->addStretch();
  auto* close = new QPushButton(tr("Close"), this);
  buttons->addWidget(close);
  v->addLayout(buttons);
  connect(close, &QPushButton::clicked, this, &QDialog::close);
  connect(sunSky, &QPushButton::clicked, this, [this] {
    doc_->beginEdit(tr("Add Sun and Sky"));
    Node* sun = addSunAndSky(doc_);
    doc_->endEdit();
    if (sun) doc_->select(sun->id, false);
  });
}

// ---------------------------------------------------------------------------------------------
// Batch render jobs

std::vector<BatchJob> batchJobsFromScene(const Scene& scene) {
  std::vector<BatchJob> out;
  auto it = scene.extensions.find("batchRender");
  if (it == scene.extensions.end() || !it->is_object() || !it->contains("jobs")) return out;
  for (const auto& j : (*it)["jobs"]) {
    if (!j.is_object()) continue;
    BatchJob b;
    b.enabled = j.value("enabled", true);
    b.name = j.value("name", std::string("View"));
    b.camera = j.value("camera", std::string());
    b.width = j.value("width", 0);
    b.height = j.value("height", 0);
    b.samples = j.value("samples", 0);
    b.output = j.value("output", std::string());
    b.saveElements = j.value("saveElements", false);
    out.push_back(b);
  }
  return out;
}

void batchJobsToScene(Scene& scene, const std::vector<BatchJob>& jobs) {
  if (jobs.empty()) {
    scene.extensions.erase("batchRender");
    return;
  }
  json arr = json::array();
  for (const auto& b : jobs)
    arr.push_back({{"enabled", b.enabled},
                   {"name", b.name},
                   {"camera", b.camera},
                   {"width", b.width},
                   {"height", b.height},
                   {"samples", b.samples},
                   {"output", b.output},
                   {"saveElements", b.saveElements}});
  scene.extensions["batchRender"] = json{{"jobs", arr}};
}

namespace {
enum Column { ColOn, ColName, ColCamera, ColSize, ColSamples, ColOutput, ColStatus, ColCount };

QString outputPathFor(const Scene& scene, const std::string& output) {
  QString p = QString::fromStdString(output);
  if (p.isEmpty() || QFileInfo(p).isAbsolute()) return p;
  QString base = scene.filePath.empty() ? QDir::currentPath()
                                        : QFileInfo(QString::fromStdString(scene.filePath)).absolutePath();
  return QDir(base).filePath(p);
}
}  // namespace

BatchRenderDialog::BatchRenderDialog(SceneDocument* doc, QWidget* parent) : QDialog(parent), doc_(doc) {
  setWindowTitle(tr("Batch Render"));
  setWindowFlag(Qt::WindowContextHelpButtonHint, false);
  resize(860, 520);
  auto* v = new QVBoxLayout(this);

  table_ = new QTableWidget(0, ColCount, this);
  table_->setHorizontalHeaderLabels({"", tr("Name"), tr("Camera"), tr("Output size"), tr("Samples"), tr("Output path"),
                                     tr("Status")});
  table_->horizontalHeader()->setSectionResizeMode(ColOutput, QHeaderView::Stretch);
  table_->horizontalHeader()->setSectionResizeMode(ColOn, QHeaderView::ResizeToContents);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->verticalHeader()->hide();
  v->addWidget(table_, 1);

  auto* rowButtons = new QHBoxLayout;
  auto* add = new QPushButton(tr("Add"), this);
  auto* dup = new QPushButton(tr("Duplicate"), this);
  auto* del = new QPushButton(tr("Delete"), this);
  rowButtons->addWidget(add);
  rowButtons->addWidget(dup);
  rowButtons->addWidget(del);
  rowButtons->addStretch();
  v->addLayout(rowButtons);

  auto* form = new QFormLayout;
  name_ = new QLineEdit(this);
  camera_ = new QComboBox(this);
  auto* size = new QHBoxLayout;
  width_ = new QSpinBox(this);
  height_ = new QSpinBox(this);
  for (QSpinBox* s : {width_, height_}) {
    s->setRange(0, 16384);
    s->setSpecialValueText(tr("Render Setup"));
  }
  size->addWidget(width_);
  size->addWidget(new QLabel("x", this));
  size->addWidget(height_);
  samples_ = new QSpinBox(this);
  samples_->setRange(0, 1000000);
  samples_->setSpecialValueText(tr("Render Setup"));
  auto* out = new QHBoxLayout;
  output_ = new QLineEdit(this);
  output_->setPlaceholderText(tr("renders/view.png  (.png .jpg .tif .exr .hdr ...)"));
  auto* browse = new QPushButton(tr("..."), this);
  browse->setFixedWidth(32);
  out->addWidget(output_, 1);
  out->addWidget(browse);
  elements_ = new QCheckBox(tr("Also save render elements (name_Element.ext)"), this);
  form->addRow(tr("Name:"), name_);
  form->addRow(tr("Camera:"), camera_);
  form->addRow(tr("Output size:"), size);
  form->addRow(tr("Max samples:"), samples_);
  form->addRow(tr("Output path:"), out);
  form->addRow(QString(), elements_);
  v->addLayout(form);

  auto* bottom = new QHBoxLayout;
  progress_ = new QLabel(this);
  bottom->addWidget(progress_, 1);
  renderBtn_ = new QPushButton(icons::render(), tr("Render"), this);
  stopBtn_ = new QPushButton(tr("Stop"), this);
  stopBtn_->setEnabled(false);
  auto* close = new QPushButton(tr("Close"), this);
  bottom->addWidget(renderBtn_);
  bottom->addWidget(stopBtn_);
  bottom->addWidget(close);
  v->addLayout(bottom);

  connect(add, &QPushButton::clicked, this, [this] {
    BatchJob j;
    j.name = QString("View%1").arg(jobs_.size() + 1, 2, 10, QChar('0')).toStdString();
    j.output = "renders/" + j.name + ".png";
    addJob(j);
  });
  connect(dup, &QPushButton::clicked, this, [this] {
    int r = table_->currentRow();
    if (r < 0 || r >= jobCount()) return;
    BatchJob j = jobs_[r];
    j.name += "_copy";
    addJob(j);
  });
  connect(del, &QPushButton::clicked, this, [this] {
    int r = table_->currentRow();
    if (r < 0 || r >= jobCount() || running_) return;
    jobs_.erase(jobs_.begin() + r);
    store(tr("Delete Batch Job"));
    refreshTable();
  });
  connect(table_, &QTableWidget::currentCellChanged, this, [this] { loadEditor(); });
  connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
    if (loading_ || item->column() != ColOn || item->row() >= jobCount()) return;
    jobs_[item->row()].enabled = item->checkState() == Qt::Checked;
    store(tr("Batch Job On/Off"));
  });
  connect(name_, &QLineEdit::editingFinished, this, &BatchRenderDialog::editorChanged);
  connect(output_, &QLineEdit::editingFinished, this, &BatchRenderDialog::editorChanged);
  connect(camera_, &QComboBox::activated, this, &BatchRenderDialog::editorChanged);
  for (QSpinBox* s : {width_, height_, samples_}) connect(s, &QSpinBox::editingFinished, this, &BatchRenderDialog::editorChanged);
  connect(elements_, &QCheckBox::toggled, this, &BatchRenderDialog::editorChanged);
  connect(browse, &QPushButton::clicked, this, &BatchRenderDialog::browseOutput);
  connect(renderBtn_, &QPushButton::clicked, this, &BatchRenderDialog::startBatch);
  connect(stopBtn_, &QPushButton::clicked, this, &BatchRenderDialog::stopBatch);
  connect(close, &QPushButton::clicked, this, &QDialog::close);

  connect(doc_, &SceneDocument::structureChanged, this, &BatchRenderDialog::reload);
  connect(doc_, &SceneDocument::fileChanged, this, &BatchRenderDialog::reload);
  connect(doc_, &SceneDocument::renderSettingsChanged, this, &BatchRenderDialog::reload);

  timer_.setInterval(200);
  connect(&timer_, &QTimer::timeout, this, &BatchRenderDialog::tick);
  reload();
}

BatchRenderDialog::~BatchRenderDialog() {
  if (renderer_) renderer_->stop();
}

void BatchRenderDialog::addJob(const BatchJob& job) {
  jobs_.push_back(job);
  store(tr("Add Batch Job"));
  refreshTable();
  table_->setCurrentCell(jobCount() - 1, ColName);
}

void BatchRenderDialog::reload() {
  if (running_) return;
  std::vector<BatchJob> jobs = batchJobsFromScene(doc_->scene());
  bool same = jobs.size() == jobs_.size();
  for (size_t i = 0; same && i < jobs.size(); ++i)
    same = jobs[i].name == jobs_[i].name && jobs[i].output == jobs_[i].output && jobs[i].camera == jobs_[i].camera &&
           jobs[i].enabled == jobs_[i].enabled && jobs[i].width == jobs_[i].width && jobs[i].height == jobs_[i].height &&
           jobs[i].samples == jobs_[i].samples && jobs[i].saveElements == jobs_[i].saveElements;
  rebuildCameras();
  if (same && table_->rowCount() == jobCount()) return;
  jobs_ = std::move(jobs);
  status_.assign(jobs_.size(), QString());
  refreshTable();
}

void BatchRenderDialog::store(const QString& label) {
  doc_->beginEdit(label);
  batchJobsToScene(doc_->scene(), jobs_);
  doc_->endEdit();
  status_.resize(jobs_.size());
}

void BatchRenderDialog::rebuildCameras() {
  QSignalBlocker b(camera_);
  QString cur = camera_->currentData().toString();
  camera_->clear();
  camera_->addItem(tr("Render Setup camera"), QString());
  for (const auto& n : doc_->scene().nodes)
    if (n->camera()) camera_->addItem(QString::fromStdString(n->name), QString::fromStdString(n->name));
  int i = camera_->findData(cur);
  camera_->setCurrentIndex(std::max(i, 0));
}

void BatchRenderDialog::refreshTable() {
  loading_ = true;
  int cur = table_->currentRow();
  table_->setRowCount(jobCount());
  status_.resize(jobs_.size());
  for (int r = 0; r < jobCount(); ++r) {
    const BatchJob& j = jobs_[r];
    auto* on = new QTableWidgetItem;
    on->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    on->setCheckState(j.enabled ? Qt::Checked : Qt::Unchecked);
    table_->setItem(r, ColOn, on);
    table_->setItem(r, ColName, new QTableWidgetItem(QString::fromStdString(j.name)));
    table_->setItem(r, ColCamera, new QTableWidgetItem(j.camera.empty() ? tr("(Render Setup)") : QString::fromStdString(j.camera)));
    table_->setItem(r, ColSize, new QTableWidgetItem(j.width > 0 && j.height > 0 ? QString("%1 x %2").arg(j.width).arg(j.height)
                                                                                 : tr("(Render Setup)")));
    table_->setItem(r, ColSamples, new QTableWidgetItem(j.samples > 0 ? QString::number(j.samples) : tr("(Render Setup)")));
    table_->setItem(r, ColOutput, new QTableWidgetItem(QString::fromStdString(j.output)));
    table_->setItem(r, ColStatus, new QTableWidgetItem(status_[r]));
  }
  if (jobCount() > 0) table_->setCurrentCell(std::clamp(cur, 0, jobCount() - 1), ColName);
  loading_ = false;
  loadEditor();
}

void BatchRenderDialog::loadEditor() {
  int r = table_->currentRow();
  bool valid = r >= 0 && r < jobCount();
  for (QWidget* w : std::initializer_list<QWidget*>{name_, camera_, width_, height_, samples_, output_, elements_})
    w->setEnabled(valid && !running_);
  if (!valid) return;
  loading_ = true;
  const BatchJob& j = jobs_[r];
  name_->setText(QString::fromStdString(j.name));
  int ci = camera_->findData(QString::fromStdString(j.camera));
  camera_->setCurrentIndex(std::max(ci, 0));
  width_->setValue(j.width);
  height_->setValue(j.height);
  samples_->setValue(j.samples);
  output_->setText(QString::fromStdString(j.output));
  elements_->setChecked(j.saveElements);
  loading_ = false;
}

void BatchRenderDialog::editorChanged() {
  int r = table_->currentRow();
  if (loading_ || r < 0 || r >= jobCount()) return;
  BatchJob j = jobs_[r];
  j.name = name_->text().toStdString();
  j.camera = camera_->currentData().toString().toStdString();
  j.width = width_->value();
  j.height = height_->value();
  j.samples = samples_->value();
  j.output = output_->text().trimmed().toStdString();
  j.saveElements = elements_->isChecked();
  BatchJob& old = jobs_[r];
  if (j.name == old.name && j.camera == old.camera && j.width == old.width && j.height == old.height &&
      j.samples == old.samples && j.output == old.output && j.saveElements == old.saveElements)
    return;
  old = j;
  store(tr("Edit Batch Job"));
  refreshTable();
}

void BatchRenderDialog::browseOutput() {
  QString start = outputPathFor(doc_->scene(), output_->text().toStdString());
  QString path = QFileDialog::getSaveFileName(
      this, tr("Batch Render Output"), start,
      tr("PNG (*.png);;JPEG (*.jpg);;TIFF (*.tif);;OpenEXR (*.exr);;Radiance HDR (*.hdr);;Targa (*.tga);;BMP (*.bmp)"));
  if (path.isEmpty()) return;
  output_->setText(path);
  editorChanged();
}

void BatchRenderDialog::startBatch() {
  if (running_) return;
  doc_->commitSoftEdit();
  status_.assign(jobs_.size(), QString());
  for (int i = 0; i < jobCount(); ++i)
    if (jobs_[i].enabled) status_[i] = tr("Queued");
  refreshTable();
  running_ = true;
  current_ = -1;
  renderBtn_->setEnabled(false);
  stopBtn_->setEnabled(true);
  loadEditor();
  nextJob();
}

void BatchRenderDialog::stopBatch() {
  if (!running_) return;
  timer_.stop();
  if (renderer_) renderer_->stop();
  if (current_ >= 0 && current_ < jobCount()) status_[current_] = tr("Stopped");
  for (auto& s : status_)
    if (s == tr("Queued")) s = tr("Skipped");
  running_ = false;
  current_ = -1;
  renderBtn_->setEnabled(true);
  stopBtn_->setEnabled(false);
  progress_->setText(tr("Batch stopped."));
  refreshTable();
}

void BatchRenderDialog::nextJob() {
  for (++current_; current_ < jobCount(); ++current_)
    if (jobs_[current_].enabled) break;
  if (current_ >= jobCount()) {
    running_ = false;
    current_ = -1;
    renderBtn_->setEnabled(true);
    stopBtn_->setEnabled(false);
    int done = static_cast<int>(std::count_if(status_.begin(), status_.end(), [](const QString& s) { return s.startsWith("Done"); }));
    progress_->setText(tr("Batch finished: %1 image(s) saved.").arg(done));
    refreshTable();
    return;
  }
  const BatchJob& job = jobs_[current_];
  const Scene& scene = doc_->scene();
  const ParamBlock& rs = scene.renderSettings;
  if (job.output.empty()) {
    status_[current_] = tr("Failed: no output path");
    refreshTable();
    nextJob();
    return;
  }
  renderer_ = std::make_unique<Renderer>();
  int w = job.width > 0 ? job.width : rs.getInt("width");
  int h = job.height > 0 ? job.height : rs.getInt("height");
  if (job.width > 0 && job.height > 0) renderer_->setResolution(w, h);
  if (job.samples > 0) renderer_->setMaxSamples(job.samples);
  std::string camName = job.camera.empty() ? rs.getString("camera") : job.camera;
  const Node* cam = nullptr;
  if (!camName.empty()) {
    cam = scene.findNodeByName(camName);
    if (cam && !cam->camera()) cam = nullptr;
    if (!cam) {
      status_[current_] = tr("Failed: camera \"%1\" not found").arg(QString::fromStdString(camName));
      refreshTable();
      nextJob();
      return;
    }
  } else {
    for (const auto& n : scene.nodes)
      if (n->camera()) {
        cam = n.get();
        break;
      }
  }
  float aspect = static_cast<float>(w) / static_cast<float>(std::max(h, 1));
  renderer_->setView(cam ? viewFromCamera(*cam, aspect) : defaultViewForScene(scene, aspect));
  std::string err;
  if (!renderer_->setScene(scene, &err)) {
    status_[current_] = tr("Failed: %1").arg(QString::fromStdString(err));
    refreshTable();
    nextJob();
    return;
  }
  status_[current_] = tr("Rendering...");
  refreshTable();
  started_ = static_cast<double>(QDateTime::currentMSecsSinceEpoch());
  renderer_->start();
  timer_.start();
}

void BatchRenderDialog::tick() {
  if (!renderer_ || current_ < 0) return;
  RenderStats s = renderer_->stats();
  int enabled = static_cast<int>(std::count_if(jobs_.begin(), jobs_.end(), [](const BatchJob& j) { return j.enabled; }));
  int index = static_cast<int>(std::count_if(jobs_.begin(), jobs_.begin() + current_, [](const BatchJob& j) { return j.enabled; }));
  progress_->setText(tr("Job %1 of %2: %3 - %4 / %5 samples, %6 s")
                         .arg(index + 1)
                         .arg(enabled)
                         .arg(QString::fromStdString(jobs_[current_].name))
                         .arg(s.passes)
                         .arg(s.targetPasses)
                         .arg(s.seconds, 0, 'f', 1));
  if (!renderer_->isRunning()) finishJob();
}

void BatchRenderDialog::finishJob() {
  timer_.stop();
  const BatchJob& job = jobs_[current_];
  const Scene& scene = doc_->scene();
  DisplaySettings display = displaySettingsFromScene(scene);
  std::vector<uint8_t> rgba;
  renderer_->resolve(display, rgba);
  std::vector<float> hdr, alpha;
  renderer_->resolveFinalHdr(display, hdr);
  renderer_->resolveAlpha(alpha);
  QString path = outputPathFor(scene, job.output);
  QDir().mkpath(QFileInfo(path).absolutePath());
  std::string err;
  double secs = renderer_->stats().seconds;
  if (!saveImage(path.toStdString(), renderer_->width(), renderer_->height(), rgba, &hdr, &alpha, false, &err)) {
    status_[current_] = tr("Failed: %1").arg(QString::fromStdString(err));
  } else {
    status_[current_] = tr("Done (%1 s)").arg(secs, 0, 'f', 1);
    if (job.saveElements) {
      QFileInfo fi(path);
      std::string ext = fi.suffix().compare("exr", Qt::CaseInsensitive) == 0 ? ".exr" : ".png";
      int n = saveRenderElements(*renderer_, display, fi.absolutePath().toStdString(),
                                 fi.completeBaseName().toStdString() + "_", {ext}, nullptr, &err);
      status_[current_] += tr(", %1 elements").arg(n);
    }
  }
  renderer_.reset();
  refreshTable();
  nextJob();
}

bool BatchRenderDialog::waitForFinish(int timeoutMs) {
  QElapsedTimer t;
  t.start();
  while (running_ && t.elapsed() < timeoutMs) QApplication::processEvents(QEventLoop::AllEvents, 50);
  return !running_;
}

QString BatchRenderDialog::jobStatus(int index) const {
  return index >= 0 && index < static_cast<int>(status_.size()) ? status_[index] : QString();
}

// ---------------------------------------------------------------------------------------------
// Print Size Assistant

namespace {
struct Paper {
  const char* name;
  double w, h;  // millimeters, portrait
};
const Paper kPapers[] = {{"A0", 841, 1189},     {"A1", 594, 841},          {"A2", 420, 594},
                         {"A3", 297, 420},      {"A4", 210, 297},          {"A5", 148, 210},
                         {"A6", 105, 148},      {"Letter", 215.9, 279.4},  {"Legal", 215.9, 355.6},
                         {"Tabloid", 279.4, 431.8}, {"Custom", 0, 0}};
}  // namespace

PrintSizeDialog::PrintSizeDialog(SceneDocument* doc, QWidget* parent) : QDialog(parent), doc_(doc) {
  setWindowTitle(tr("Print Size Assistant"));
  setWindowFlag(Qt::WindowContextHelpButtonHint, false);
  auto* v = new QVBoxLayout(this);
  auto* form = new QFormLayout;
  paper_ = new QComboBox(this);
  for (const auto& p : kPapers) paper_->addItem(p.name);
  orientation_ = new QComboBox(this);
  orientation_->addItems({tr("Portrait"), tr("Landscape")});
  widthMm_ = new QDoubleSpinBox(this);
  heightMm_ = new QDoubleSpinBox(this);
  for (QDoubleSpinBox* s : {widthMm_, heightMm_}) {
    s->setRange(1.0, 10000.0);
    s->setDecimals(1);
    s->setSuffix(" mm");
  }
  dpi_ = new QSpinBox(this);
  dpi_->setRange(36, 2400);
  dpi_->setValue(300);
  dpi_->setSuffix(" dpi");
  result_ = new QLabel(this);
  QFont f = result_->font();
  f.setBold(true);
  result_->setFont(f);
  form->addRow(tr("Paper size:"), paper_);
  form->addRow(tr("Orientation:"), orientation_);
  form->addRow(tr("Paper width:"), widthMm_);
  form->addRow(tr("Paper height:"), heightMm_);
  form->addRow(tr("Resolution:"), dpi_);
  form->addRow(tr("Image size:"), result_);
  v->addLayout(form);
  auto* buttons = new QHBoxLayout;
  buttons->addStretch();
  auto* applyBtn = new QPushButton(tr("Set Render Size"), this);
  auto* render = new QPushButton(icons::render(), tr("Render"), this);
  auto* close = new QPushButton(tr("Close"), this);
  buttons->addWidget(applyBtn);
  buttons->addWidget(render);
  buttons->addWidget(close);
  v->addLayout(buttons);

  connect(paper_, &QComboBox::currentIndexChanged, this, &PrintSizeDialog::paperChanged);
  connect(orientation_, &QComboBox::currentIndexChanged, this, &PrintSizeDialog::paperChanged);
  connect(widthMm_, &QDoubleSpinBox::valueChanged, this, &PrintSizeDialog::updateResult);
  connect(heightMm_, &QDoubleSpinBox::valueChanged, this, &PrintSizeDialog::updateResult);
  connect(dpi_, &QSpinBox::valueChanged, this, &PrintSizeDialog::updateResult);
  connect(applyBtn, &QPushButton::clicked, this, &PrintSizeDialog::apply);
  connect(render, &QPushButton::clicked, this, [this] {
    apply();
    Q_EMIT renderRequested();
  });
  connect(close, &QPushButton::clicked, this, &QDialog::close);
  paper_->setCurrentIndex(4);  // A4
  paperChanged();
}

void PrintSizeDialog::setPaper(const QString& name, bool landscape, int dpi) {
  int i = paper_->findText(name);
  if (i >= 0) paper_->setCurrentIndex(i);
  orientation_->setCurrentIndex(landscape ? 1 : 0);
  dpi_->setValue(dpi);
  paperChanged();
}

void PrintSizeDialog::paperChanged() {
  const Paper& p = kPapers[std::clamp(paper_->currentIndex(), 0, static_cast<int>(std::size(kPapers)) - 1)];
  bool custom = p.w <= 0.0;
  widthMm_->setEnabled(custom);
  heightMm_->setEnabled(custom);
  if (!custom) {
    bool landscape = orientation_->currentIndex() == 1;
    QSignalBlocker b1(widthMm_), b2(heightMm_);
    widthMm_->setValue(landscape ? p.h : p.w);
    heightMm_->setValue(landscape ? p.w : p.h);
  }
  updateResult();
}

int PrintSizeDialog::pixelWidth() const {
  return std::clamp(static_cast<int>(std::lround(widthMm_->value() / 25.4 * dpi_->value())), 16, 16384);
}

int PrintSizeDialog::pixelHeight() const {
  return std::clamp(static_cast<int>(std::lround(heightMm_->value() / 25.4 * dpi_->value())), 16, 16384);
}

void PrintSizeDialog::updateResult() {
  int w = pixelWidth(), h = pixelHeight();
  result_->setText(tr("%1 x %2 pixels (%3 MP)").arg(w).arg(h).arg(static_cast<double>(w) * h / 1e6, 0, 'f', 1));
}

void PrintSizeDialog::apply() {
  doc_->beginEdit(tr("Print Size"));
  doc_->scene().renderSettings.setNumber("width", pixelWidth());
  doc_->scene().renderSettings.setNumber("height", pixelHeight());
  doc_->notifyRenderSettingsChanged();
  doc_->endEdit();
}

}  // namespace veraa
