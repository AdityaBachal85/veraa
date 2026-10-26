#include "rendersetup.h"

#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QVBoxLayout>

#include "icons.h"
#include "paramform.h"
#include "renderer.h"
#include "rendersettings.h"

namespace veraa {

namespace {
struct SizePreset {
  const char* name;
  int w, h;
};
const SizePreset kSizes[] = {{"Custom", 0, 0},         {"640 x 480", 640, 480},     {"800 x 600", 800, 600},
                             {"HD 1280 x 720", 1280, 720}, {"Full HD 1920 x 1080", 1920, 1080},
                             {"QHD 2560 x 1440", 2560, 1440}, {"4K UHD 3840 x 2160", 3840, 2160},
                             {"Square 1024", 1024, 1024}, {"Square 2048", 2048, 2048},
                             {"Instagram 1080 x 1350", 1080, 1350}, {"A4 portrait 2480 x 3508", 2480, 3508}};

// Groups shown by the dedicated tabs; everything else lands on the Common tab.
const QStringList kClaimed = {"Image sampler", "Global illumination", "Color mapping",   "Exposure",
                              "Environment",   "Atmosphere",          "Lens effects",    "Color corrections",
                              "Render elements", "Camera",            "Path guiding"};

QWidget* scrolled(QWidget* content) {
  auto* scroll = new QScrollArea;
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setWidget(content);
  return scroll;
}
}  // namespace

// ---------------------------------------------------------------------------------------------

RenderSettingsPanel::RenderSettingsPanel(SceneDocument* doc, const QStringList& groups, QWidget* parent,
                                         const QStringList& excludeKeys, const QStringList& claimed)
    : QWidget(parent), doc_(doc), groups_(groups), exclude_(excludeKeys), claimed_(claimed) {
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  form_ = new ParamForm(this);
  v->addWidget(form_);
  form_->beforeChange = [this](const std::string& key) {
    doc_->softEdit(tr("Render Setting"), QString("render/%1").arg(QString::fromStdString(key)));
  };
  form_->afterChange = [this](const std::string&) { doc_->notifyRenderSettingsChanged(); };
  connect(doc_, &SceneDocument::renderSettingsChanged, this, [this] { form_->refresh(); });
  connect(doc_, &SceneDocument::fileChanged, this, &RenderSettingsPanel::reload);
  reload();
}

void RenderSettingsPanel::reload() {
  QString file = doc_->filePath();
  form_->browseDir = file.isEmpty() ? QString() : QFileInfo(file).absolutePath();
  QStringList exclude = exclude_;
  for (const ParamDef& d : doc_->scene().renderSettings.defs()) {
    QString g = QString::fromStdString(d.group);
    bool shown = groups_.contains(g) || (!claimed_.isEmpty() && !claimed_.contains(g));
    if (!shown) exclude << QString::fromStdString(d.key);
  }
  form_->setBlock(&doc_->scene().renderSettings, exclude);
}

// ---------------------------------------------------------------------------------------------

RenderSetupDialog::RenderSetupDialog(SceneDocument* doc, QWidget* parent) : QDialog(parent), doc_(doc) {
  setWindowTitle(tr("Render Setup - Veraa Path Tracer"));
  setWindowFlag(Qt::WindowContextHelpButtonHint, false);
  resize(460, 760);
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(6, 6, 6, 6);

  tabs_ = new QTabWidget(this);
  tabs_->setDocumentMode(true);
  tabs_->setUsesScrollButtons(true);
  v->addWidget(tabs_, 1);

  // ---- Common ----
  auto* common = new QWidget;
  auto* cv = new QVBoxLayout(common);
  cv->setContentsMargins(4, 4, 4, 4);
  auto* top = new QFormLayout;
  camera_ = new QComboBox(common);
  sizePreset_ = new QComboBox(common);
  for (const auto& s : kSizes) sizePreset_->addItem(tr(s.name));
  aspect_ = new QLabel(common);
  top->addRow(tr("View to render:"), camera_);
  top->addRow(tr("Output preset:"), sizePreset_);
  top->addRow(tr("Image aspect:"), aspect_);
  cv->addLayout(top);
  cv->addWidget(new RenderSettingsPanel(doc_, {"Output size"}, common, {"camera"}, kClaimed));
  auto* tools = new QHBoxLayout;
  auto* printSize = new QPushButton(tr("Print Size Assistant..."), common);
  auto* batch = new QPushButton(tr("Batch Render..."), common);
  tools->addWidget(printSize);
  tools->addWidget(batch);
  cv->addLayout(tools);
  auto* presets = new QHBoxLayout;
  auto* savePreset = new QPushButton(tr("Save Preset..."), common);
  auto* loadPreset = new QPushButton(tr("Load Preset..."), common);
  presets->addWidget(savePreset);
  presets->addWidget(loadPreset);
  cv->addLayout(presets);
  cv->addStretch();
  tabs_->addTab(scrolled(common), tr("Common"));
  connect(printSize, &QPushButton::clicked, this, &RenderSetupDialog::printSizeRequested);
  connect(batch, &QPushButton::clicked, this, &RenderSetupDialog::batchRenderRequested);
  connect(savePreset, &QPushButton::clicked, this, &RenderSetupDialog::savePreset);
  connect(loadPreset, &QPushButton::clicked, this, &RenderSetupDialog::loadPreset);

  // ---- Sampler ----
  auto* sampler = new QWidget;
  auto* sv = new QVBoxLayout(sampler);
  sv->setContentsMargins(4, 4, 4, 4);
  auto* qualityRow = new QFormLayout;
  quality_ = new QComboBox(sampler);
  quality_->addItem(tr("Custom"));
  for (const auto& name : renderQualityPresetNames()) quality_->addItem(tr(name.c_str()));
  quality_->setToolTip(tr("Sets samples, noise threshold, bounces, clamping, path guiding, glossy filter and the "
                          "denoiser together"));
  qualityRow->addRow(tr("Quality preset:"), quality_);
  sv->addLayout(qualityRow);
  sv->addWidget(new RenderSettingsPanel(doc_, {"Image sampler"}, sampler));
  auto* info = new QLabel(sampler);
  info->setWordWrap(true);
  info->setStyleSheet("color: #a8a8a8;");
  info->setText(
      tr("Progressive refines the whole image; Bucket renders tiles one after another. With a noise "
         "threshold, pixels stop sampling once clean (adaptive sampling).") +
      "\n" +
      (Renderer::denoiserAvailable() ? tr("Intel Open Image Denoise is available (Color mapping tab).")
                                     : tr("Denoiser not available in this build; images render without denoising.")));
  sv->addWidget(info);
  sv->addStretch();
  tabs_->addTab(scrolled(sampler), tr("Sampler"));

  // ---- GI ----
  auto* gi = new QWidget;
  auto* gv = new QVBoxLayout(gi);
  gv->setContentsMargins(4, 4, 4, 4);
  gv->addWidget(new RenderSettingsPanel(doc_, {"Global illumination", "Path guiding"}, gi));
  auto* giInfo = new QLabel(tr("Global illumination is brute-force path tracing for primary and secondary "
                               "bounces. Clamping (in pixel values after exposure) and the glossy filter remove "
                               "fireflies at the cost of some energy. Path guiding learns where light comes "
                               "from during the first passes and aims later rays there; the light tree "
                               "keeps scenes with many lights clean."),
                            gi);
  giInfo->setWordWrap(true);
  giInfo->setStyleSheet("color: #a8a8a8;");
  gv->addWidget(giInfo);
  gv->addStretch();
  tabs_->addTab(scrolled(gi), tr("GI"));

  // ---- Color mapping ----
  auto* cm = new QWidget;
  auto* cmv = new QVBoxLayout(cm);
  cmv->setContentsMargins(4, 4, 4, 4);
  auto* lookRow = new QHBoxLayout;
  auto* look = new QComboBox(cm);
  look->addItem(tr("Apply look preset..."));
  for (const auto& name : renderLookPresetNames()) look->addItem(QString::fromStdString(name));
  look->setToolTip(tr("Sets the color mapping, exposure and white balance for a kind of scene"));
  lookRow->addWidget(look, 1);
  cmv->addLayout(lookRow);
  connect(look, &QComboBox::activated, this, [this, look](int i) {
    if (i <= 0) return;
    doc_->beginEdit(tr("Look Preset"));
    applyRenderLookPreset(doc_->scene().renderSettings, renderLookPresetNames()[static_cast<size_t>(i - 1)]);
    doc_->notifyRenderSettingsChanged();
    doc_->endEdit();
    QSignalBlocker b(look);
    look->setCurrentIndex(0);
  });
  cmv->addWidget(new RenderSettingsPanel(doc_, {"Color mapping", "Exposure"}, cm));
  cmv->addStretch();
  tabs_->addTab(scrolled(cm), tr("Color Mapping"));

  // ---- Environment ----
  auto* env = new QWidget;
  auto* ev = new QVBoxLayout(env);
  ev->setContentsMargins(4, 4, 4, 4);
  ev->addWidget(new RenderSettingsPanel(doc_, {"Environment", "Atmosphere"}, env));
  auto* envDlg = new QPushButton(tr("Environment and Effects..."), env);
  ev->addWidget(envDlg);
  ev->addStretch();
  tabs_->addTab(scrolled(env), tr("Environment"));
  connect(envDlg, &QPushButton::clicked, this, &RenderSetupDialog::environmentRequested);

  // ---- Frame buffer ----
  auto* fbTab = new QWidget;
  auto* fv = new QVBoxLayout(fbTab);
  fv->setContentsMargins(4, 4, 4, 4);
  fv->addWidget(new RenderSettingsPanel(doc_, {"Lens effects", "Color corrections"}, fbTab));
  auto* fbInfo = new QLabel(tr("These are the frame buffer's starting values; adjust them live in the Rendered "
                               "Frame Window and use \"Save Display Settings to Scene\" there."),
                            fbTab);
  fbInfo->setWordWrap(true);
  fbInfo->setStyleSheet("color: #a8a8a8;");
  fv->addWidget(fbInfo);
  fv->addStretch();
  tabs_->addTab(scrolled(fbTab), tr("Frame Buffer"));

  // ---- Render elements ----
  auto* re = new QWidget;
  auto* rv = new QVBoxLayout(re);
  rv->setContentsMargins(4, 4, 4, 4);
  rv->addWidget(new RenderSettingsPanel(doc_, {"Render elements"}, re));
  auto* reInfo = new QLabel(tr("Beauty elements (Lighting through Background) add up to the final image. "
                               "Light Mix keeps one layer per light group so lights can be rebalanced "
                               "after rendering."),
                            re);
  reInfo->setWordWrap(true);
  reInfo->setStyleSheet("color: #a8a8a8;");
  rv->addWidget(reInfo);
  rv->addStretch();
  tabs_->addTab(scrolled(re), tr("Render Elements"));

  // ---- Buttons ----
  auto* buttons = new QHBoxLayout;
  buttons->addStretch();
  auto* render = new QPushButton(icons::render(), tr("Render"), this);
  render->setDefault(true);
  auto* close = new QPushButton(tr("Close"), this);
  buttons->addWidget(render);
  buttons->addWidget(close);
  v->addLayout(buttons);
  connect(render, &QPushButton::clicked, this, &RenderSetupDialog::renderRequested);
  connect(close, &QPushButton::clicked, this, &QDialog::close);

  connect(quality_, &QComboBox::activated, this, [this](int i) {
    if (i <= 0) return;
    applyQualityPreset(i - 1);
  });
  connect(doc_, &SceneDocument::renderSettingsChanged, this, &RenderSetupDialog::updateQualityPreset);
  connect(doc_, &SceneDocument::fileChanged, this, &RenderSetupDialog::updateQualityPreset);
  updateQualityPreset();
  connect(sizePreset_, &QComboBox::activated, this, [this](int i) {
    if (i <= 0) return;
    doc_->beginEdit(tr("Output Size"));
    doc_->scene().renderSettings.setNumber("width", kSizes[i].w);
    doc_->scene().renderSettings.setNumber("height", kSizes[i].h);
    doc_->notifyRenderSettingsChanged();
    doc_->endEdit();
  });
  connect(camera_, &QComboBox::activated, this, [this](int i) {
    std::string name = i <= 0 ? std::string() : camera_->itemText(i).toStdString();
    if (doc_->scene().renderSettings.getString("camera") == name) return;
    doc_->beginEdit(tr("Render Camera"));
    doc_->scene().renderSettings.setString("camera", name);
    doc_->notifyRenderSettingsChanged();
    doc_->endEdit();
  });
  connect(doc_, &SceneDocument::renderSettingsChanged, this, &RenderSetupDialog::rebuildCameraList);
  connect(doc_, &SceneDocument::structureChanged, this, &RenderSetupDialog::rebuildCameraList);
  connect(doc_, &SceneDocument::nodesChanged, this, [this](const QVector<uint32_t>&, int flags) {
    if (flags & ChangeName) rebuildCameraList();
  });
  rebuildCameraList();
}

bool RenderSetupDialog::showTab(const QString& title) {
  for (int i = 0; i < tabs_->count(); ++i)
    if (tabs_->tabText(i) == title) {
      tabs_->setCurrentIndex(i);
      return true;
    }
  return false;
}

void RenderSetupDialog::applyQualityPreset(int preset) {
  if (preset < 0 || preset >= static_cast<int>(renderQualityPresetNames().size())) return;
  doc_->beginEdit(tr("Quality Preset"));
  applyRenderQualityPreset(doc_->scene().renderSettings, preset);
  doc_->notifyRenderSettingsChanged();
  doc_->endEdit();
  updateQualityPreset();
}

int RenderSetupDialog::qualityPreset() const { return quality_->currentIndex() - 1; }

void RenderSetupDialog::updateQualityPreset() {
  QSignalBlocker b(quality_);
  quality_->setCurrentIndex(matchingRenderQualityPreset(doc_->scene().renderSettings) + 1);
}

QStringList RenderSetupDialog::tabTitles() const {
  QStringList out;
  for (int i = 0; i < tabs_->count(); ++i) out << tabs_->tabText(i);
  return out;
}

void RenderSetupDialog::rebuildCameraList() {
  QSignalBlocker b(camera_);
  camera_->clear();
  camera_->addItem(tr("Active viewport"));
  std::string cur = doc_->scene().renderSettings.getString("camera");
  int sel = 0;
  for (const auto& n : doc_->scene().nodes)
    if (n->camera()) {
      camera_->addItem(QString::fromStdString(n->name));
      if (n->name == cur) sel = camera_->count() - 1;
    }
  camera_->setCurrentIndex(sel);
  int w = doc_->scene().renderSettings.getInt("width"), h = doc_->scene().renderSettings.getInt("height");
  QSignalBlocker b2(sizePreset_);
  int preset = 0;
  for (int i = 1; i < static_cast<int>(sizeof(kSizes) / sizeof(kSizes[0])); ++i)
    if (kSizes[i].w == w && kSizes[i].h == h) preset = i;
  sizePreset_->setCurrentIndex(preset);
  updateAspect();
}

void RenderSetupDialog::updateAspect() {
  int w = doc_->scene().renderSettings.getInt("width"), h = doc_->scene().renderSettings.getInt("height");
  double mp = static_cast<double>(w) * h / 1e6;
  aspect_->setText(tr("%1 : 1   (%2 MP)").arg(static_cast<double>(w) / std::max(1, h), 0, 'f', 3).arg(mp, 0, 'f', 2));
}

void RenderSetupDialog::savePreset() {
  QString path = QFileDialog::getSaveFileName(this, tr("Save Render Preset"), QString(),
                                              tr("Render presets (*.json);;All files (*)"));
  if (path.isEmpty()) return;
  if (!path.endsWith(".json", Qt::CaseInsensitive)) path += ".json";
  json j = doc_->scene().renderSettings.toJson();
  j.erase("camera");  // presets carry quality settings, not the scene's camera
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly) || f.write(QByteArray::fromStdString(j.dump(2))) < 0)
    QMessageBox::warning(this, tr("Save Render Preset"), tr("Could not write %1").arg(path));
}

void RenderSetupDialog::loadPreset() {
  QString path = QFileDialog::getOpenFileName(this, tr("Load Render Preset"), QString(),
                                              tr("Render presets (*.json);;All files (*)"));
  if (path.isEmpty()) return;
  QFile f(path);
  json j;
  if (f.open(QIODevice::ReadOnly)) j = json::parse(f.readAll().toStdString(), nullptr, false);
  if (!j.is_object()) {
    QMessageBox::warning(this, tr("Load Render Preset"), tr("%1 is not a render preset.").arg(path));
    return;
  }
  j.erase("camera");
  doc_->beginEdit(tr("Load Render Preset"));
  doc_->scene().renderSettings.fromJson(j);
  doc_->notifyRenderSettingsChanged();
  doc_->endEdit();
}

}  // namespace veraa
