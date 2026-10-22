#include "rendersetup.h"

#include <QComboBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "icons.h"
#include "paramform.h"
#include "renderer.h"

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
                             {"A4 portrait 2480 x 3508", 2480, 3508}};
}  // namespace

RenderSetupDialog::RenderSetupDialog(SceneDocument* doc, QWidget* parent) : QDialog(parent), doc_(doc) {
  setWindowTitle(tr("Render Setup - Veraa Path Tracer"));
  setWindowFlag(Qt::WindowContextHelpButtonHint, false);
  resize(430, 720);
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(6, 6, 6, 6);

  auto* top = new QFormLayout;
  camera_ = new QComboBox(this);
  sizePreset_ = new QComboBox(this);
  for (const auto& s : kSizes) sizePreset_->addItem(tr(s.name));
  top->addRow(tr("View to render:"), camera_);
  top->addRow(tr("Output preset:"), sizePreset_);
  v->addLayout(top);

  auto* scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  form_ = new ParamForm;
  scroll->setWidget(form_);
  v->addWidget(scroll, 1);

  auto* info = new QLabel(this);
  info->setWordWrap(true);
  info->setStyleSheet("color: #a8a8a8;");
  info->setText(Renderer::denoiserAvailable()
                    ? tr("Intel Open Image Denoise is available.")
                    : tr("Denoiser not available in this build; images render without denoising."));
  v->addWidget(info);

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

  form_->beforeChange = [this](const std::string& key) {
    doc_->softEdit(tr("Render Setting"), QString("render/%1").arg(QString::fromStdString(key)));
  };
  form_->afterChange = [this](const std::string& key) {
    if (key == "width" || key == "height") {
      QSignalBlocker b(sizePreset_);
      sizePreset_->setCurrentIndex(0);
    }
    doc_->notifyRenderSettingsChanged();
  };
  connect(sizePreset_, &QComboBox::activated, this, [this](int i) {
    if (i <= 0) return;
    doc_->beginEdit(tr("Output Size"));
    doc_->scene().renderSettings.setNumber("width", kSizes[i].w);
    doc_->scene().renderSettings.setNumber("height", kSizes[i].h);
    doc_->notifyRenderSettingsChanged();
    doc_->endEdit();
    form_->refresh();
  });
  connect(camera_, &QComboBox::activated, this, [this](int i) {
    std::string name = i <= 0 ? std::string() : camera_->itemText(i).toStdString();
    if (doc_->scene().renderSettings.getString("camera") == name) return;
    doc_->beginEdit(tr("Render Camera"));
    doc_->scene().renderSettings.setString("camera", name);
    doc_->notifyRenderSettingsChanged();
    doc_->endEdit();
  });

  auto reload = [this] {
    QString file = doc_->filePath();
    form_->browseDir = file.isEmpty() ? QString() : QFileInfo(file).absolutePath();
    form_->setBlock(&doc_->scene().renderSettings, {"camera"});
    rebuildCameraList();
  };
  // renderSettings is a member of the scene and never reallocated, but values change on undo.
  connect(doc_, &SceneDocument::renderSettingsChanged, this, [this] {
    form_->refresh();
    rebuildCameraList();
  });
  connect(doc_, &SceneDocument::structureChanged, this, [this] { rebuildCameraList(); });
  connect(doc_, &SceneDocument::nodesChanged, this, [this](const QVector<uint32_t>&, int flags) {
    if (flags & ChangeName) rebuildCameraList();
  });
  reload();
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
}

}  // namespace veraa
