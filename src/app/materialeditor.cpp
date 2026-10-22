#include "materialeditor.h"

#include <QComboBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QVBoxLayout>

#include "paramform.h"
#include "renderer.h"

namespace veraa {

namespace {
constexpr int kSlotSize = 96;

// Same pixmap for normal and selected states (no blue selection tint over the swatch).
QIcon slotIcon(const QImage& img) {
  QPixmap pm = QPixmap::fromImage(img.scaled(kSlotSize, kSlotSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
  QIcon icon;
  icon.addPixmap(pm, QIcon::Normal);
  icon.addPixmap(pm, QIcon::Selected);
  return icon;
}

// Sample slots that can be dragged onto objects in a viewport.
class SlotList : public QListWidget {
 public:
  using QListWidget::QListWidget;

 protected:
  QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override {
    if (items.isEmpty()) return nullptr;
    auto* mime = new QMimeData;
    mime->setData(kMaterialMime, QByteArray::number(items.first()->data(Qt::UserRole).toUInt()));
    return mime;
  }
  QStringList mimeTypes() const override { return {kMaterialMime}; }
};
constexpr int kPreviewSize = 160;
constexpr int kPreviewSamples = 48;

QImage placeholder(int size) {
  QImage img(size, size, QImage::Format_RGB32);
  img.fill(QColor(0x3a, 0x3a, 0x3a));
  QPainter p(&img);
  p.setRenderHint(QPainter::Antialiasing);
  for (int y = 0; y < size; y += 12)
    for (int x = (y / 12) % 2 * 12; x < size; x += 24) p.fillRect(x, y, 12, 12, QColor(0x46, 0x46, 0x46));
  p.setPen(Qt::NoPen);
  p.setBrush(QColor(0x80, 0x80, 0x80));
  p.drawEllipse(QRectF(size * 0.18, size * 0.14, size * 0.64, size * 0.64));
  return img;
}
}  // namespace

// ---------------------------------------------------------------------------------------------

PreviewRenderer::PreviewRenderer(QObject* parent) : QObject(parent) {
  thread_ = std::thread([this] { run(); });
}

PreviewRenderer::~PreviewRenderer() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    quit_ = true;
    jobs_.clear();
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void PreviewRenderer::request(uint32_t id, const Material& material, int size, int samples) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = jobs_.begin(); it != jobs_.end();)
      it = it->id == id ? jobs_.erase(it) : it + 1;
    jobs_.push_back(Job{id, material, size, samples});
  }
  cv_.notify_one();
}

void PreviewRenderer::run() {
  for (;;) {
    Job job{0, Material(), 0, 0};
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return quit_ || !jobs_.empty(); });
      if (quit_) return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    std::vector<uint8_t> rgba = renderMaterialPreview(job.material, nullptr, job.size, job.samples);
    if (rgba.size() != static_cast<size_t>(job.size) * job.size * 4) continue;
    QImage img(rgba.data(), job.size, job.size, job.size * 4, QImage::Format_RGBA8888);
    QImage copy = img.copy();
    uint32_t id = job.id;
    QMetaObject::invokeMethod(this, [this, id, copy] { Q_EMIT ready(id, copy); }, Qt::QueuedConnection);
  }
}

// ---------------------------------------------------------------------------------------------

MaterialEditor::MaterialEditor(SceneDocument* doc, QWidget* parent) : QDialog(parent), doc_(doc) {
  setWindowTitle(tr("Material Editor - Veraa"));
  setWindowFlag(Qt::WindowContextHelpButtonHint, false);
  resize(760, 640);

  auto* root = new QHBoxLayout(this);
  root->setContentsMargins(6, 6, 6, 6);
  auto* split = new QSplitter(Qt::Horizontal, this);
  root->addWidget(split);

  // Left: sample slots and material commands.
  auto* left = new QWidget(split);
  auto* lv = new QVBoxLayout(left);
  lv->setContentsMargins(0, 0, 0, 0);
  slots_ = new SlotList(left);
  slots_->setDragEnabled(true);
  slots_->setDragDropMode(QAbstractItemView::DragOnly);
  slots_->setToolTip(tr("Drag a material onto an object in a viewport to assign it"));
  slots_->setViewMode(QListView::IconMode);
  slots_->setIconSize(QSize(kSlotSize, kSlotSize));
  slots_->setGridSize(QSize(kSlotSize + 18, kSlotSize + 30));
  slots_->setResizeMode(QListView::Adjust);
  slots_->setMovement(QListView::Static);
  slots_->setWordWrap(true);
  slots_->setSpacing(4);
  lv->addWidget(slots_, 1);
  auto* row1 = new QHBoxLayout;
  auto* newBtn = new QPushButton(tr("New"), left);
  dup_ = new QPushButton(tr("Duplicate"), left);
  del_ = new QPushButton(tr("Delete"), left);
  row1->addWidget(newBtn);
  row1->addWidget(dup_);
  row1->addWidget(del_);
  lv->addLayout(row1);
  auto* row2 = new QHBoxLayout;
  assign_ = new QPushButton(tr("Assign to Selection"), left);
  pick_ = new QPushButton(tr("Pick from Object"), left);
  row2->addWidget(assign_);
  row2->addWidget(pick_);
  lv->addLayout(row2);

  // Right: preview, name, presets, parameters.
  auto* right = new QWidget(split);
  auto* rv = new QVBoxLayout(right);
  rv->setContentsMargins(0, 0, 0, 0);
  auto* top = new QHBoxLayout;
  bigPreview_ = new QLabel(right);
  bigPreview_->setFixedSize(kPreviewSize, kPreviewSize);
  bigPreview_->setFrameShape(QFrame::Box);
  top->addWidget(bigPreview_);
  auto* info = new QVBoxLayout;
  name_ = new QLineEdit(right);
  presets_ = new QComboBox(right);
  presets_->addItems({tr("Apply preset..."), tr("Default (matte)"), tr("Glossy plastic"), tr("Clear glass"),
                      tr("Frosted glass"), tr("Chrome"), tr("Brushed aluminium"), tr("Gold"), tr("Copper"),
                      tr("Rubber"), tr("Ceramic (glazed)"), tr("Water"), tr("Light emitter")});
  usage_ = new QLabel(right);
  usage_->setWordWrap(true);
  usage_->setStyleSheet("color: #a8a8a8;");
  info->addWidget(new QLabel(tr("Name:"), right));
  info->addWidget(name_);
  info->addWidget(presets_);
  info->addWidget(usage_);
  info->addStretch();
  top->addLayout(info, 1);
  rv->addLayout(top);
  auto* scroll = new QScrollArea(right);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  form_ = new ParamForm;
  scroll->setWidget(form_);
  rv->addWidget(scroll, 1);
  split->setStretchFactor(0, 1);
  split->setStretchFactor(1, 2);
  split->setSizes({300, 460});

  previewTimer_.setSingleShot(true);
  previewTimer_.setInterval(250);
  connect(&previewTimer_, &QTimer::timeout, this, [this] {
    for (uint32_t id : pendingPreviews_) requestPreview(id);
    pendingPreviews_.clear();
  });
  connect(&previews_, &PreviewRenderer::ready, this, [this](uint32_t id, const QImage& img) {
    images_[id] = img;
    for (int i = 0; i < slots_->count(); ++i)
      if (slots_->item(i)->data(Qt::UserRole).toUInt() == id)
        slots_->item(i)->setIcon(slotIcon(img));
    if (id == currentId_) bigPreview_->setPixmap(QPixmap::fromImage(img));
  });

  connect(slots_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* it) {
    if (!it) return;
    currentId_ = it->data(Qt::UserRole).toUInt();
    showCurrent();
  });
  connect(newBtn, &QPushButton::clicked, this, [this] {
    doc_->beginEdit(tr("New Material"));
    Material* m = doc_->scene().createMaterial(doc_->scene().uniqueMaterialName("Material"));
    uint32_t id = m->id;
    doc_->notifyMaterialsChanged();
    doc_->endEdit();
    selectMaterial(id);
  });
  connect(dup_, &QPushButton::clicked, this, [this] {
    Material* cur = current();
    if (!cur) return;
    doc_->beginEdit(tr("Duplicate Material"));
    auto copy = std::make_unique<Material>(*cur);
    copy->name = doc_->scene().uniqueMaterialName(cur->name + " copy");
    uint32_t id = doc_->scene().addMaterial(std::move(copy))->id;
    doc_->notifyMaterialsChanged();
    doc_->endEdit();
    selectMaterial(id);
  });
  connect(del_, &QPushButton::clicked, this, [this] {
    Material* cur = current();
    if (!cur) return;
    QVector<uint32_t> users;
    for (const auto& n : doc_->scene().nodes)
      if (n->materialId == cur->id) users.push_back(n->id);
    doc_->beginEdit(tr("Delete Material"));
    doc_->scene().removeMaterial(cur->id);
    currentId_ = 0;
    doc_->notifyMaterialsChanged();
    if (!users.isEmpty()) doc_->notifyNodesChanged(users, ChangeMaterial);
    doc_->endEdit();
  });
  connect(assign_, &QPushButton::clicked, this, [this] {
    if (Material* cur = current()) doc_->assignMaterialToSelection(cur->id);
    refreshUsage();
  });
  connect(pick_, &QPushButton::clicked, this, [this] {
    Node* n = doc_->firstSelected();
    if (n && n->materialId) selectMaterial(n->materialId);
  });
  connect(name_, &QLineEdit::editingFinished, this, [this] {
    Material* cur = current();
    if (!cur || name_->text().isEmpty() || cur->name == name_->text().toStdString()) return;
    doc_->beginEdit(tr("Rename Material"));
    cur->name = name_->text().toStdString();
    doc_->notifyMaterialsChanged();
    doc_->endEdit();
  });
  connect(presets_, &QComboBox::activated, this, [this](int i) {
    if (i > 0) applyPreset(i);
    QSignalBlocker b(presets_);
    presets_->setCurrentIndex(0);
  });

  form_->beforeChange = [this](const std::string& key) {
    doc_->softEdit(tr("Material Parameter"), QString("mtl/%1/%2").arg(currentId_).arg(QString::fromStdString(key)));
  };
  form_->afterChange = [this](const std::string&) {
    doc_->notifyMaterialsChanged();
    if (!pendingPreviews_.contains(currentId_)) pendingPreviews_.push_back(currentId_);
    previewTimer_.start();
  };

  auto onScene = [this] {
    // Material objects may have been replaced (undo/redo/load).
    rebuildSlots();
  };
  connect(doc_, &SceneDocument::materialsChanged, this, onScene);
  connect(doc_, &SceneDocument::structureChanged, this, onScene);
  connect(doc_, &SceneDocument::nodesChanged, this, [this](const QVector<uint32_t>&, int flags) {
    if (flags & (ChangeMaterial | ChangeName)) refreshUsage();
  });
  connect(doc_, &SceneDocument::selectionChanged, this, [this] {
    assign_->setEnabled(current() && !doc_->selection().isEmpty());
    pick_->setEnabled(doc_->firstSelected() && doc_->firstSelected()->materialId);
  });
  rebuildSlots();
}

Material* MaterialEditor::current() const { return doc_->scene().findMaterial(currentId_); }

void MaterialEditor::selectMaterial(uint32_t id) {
  for (int i = 0; i < slots_->count(); ++i)
    if (slots_->item(i)->data(Qt::UserRole).toUInt() == id) {
      slots_->setCurrentRow(i);
      return;
    }
}

void MaterialEditor::rebuildSlots() {
  const Scene& scene = doc_->scene();
  // Keep the list when only names/params changed; rebuild if the set of materials changed.
  QList<uint32_t> ids;
  for (const auto& m : scene.materials) ids.push_back(m->id);
  QList<uint32_t> shown;
  for (int i = 0; i < slots_->count(); ++i) shown.push_back(slots_->item(i)->data(Qt::UserRole).toUInt());

  QSignalBlocker b(slots_);
  if (ids != shown) {
    slots_->clear();
    for (const auto& m : scene.materials) {
      auto* it = new QListWidgetItem(QString::fromStdString(m->name), slots_);
      it->setData(Qt::UserRole, m->id);
      QImage img = images_.value(m->id, placeholder(kSlotSize));
      it->setIcon(slotIcon(img));
      it->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
      requestPreview(m->id);
    }
  } else {
    for (int i = 0; i < slots_->count(); ++i)
      if (const Material* m = scene.findMaterial(ids[i])) slots_->item(i)->setText(QString::fromStdString(m->name));
  }
  if (!scene.findMaterial(currentId_)) currentId_ = scene.materials.empty() ? 0 : scene.materials.front()->id;
  for (int i = 0; i < slots_->count(); ++i)
    if (slots_->item(i)->data(Qt::UserRole).toUInt() == currentId_) slots_->setCurrentRow(i);
  showCurrent();
}

void MaterialEditor::showCurrent() {
  Material* m = current();
  dup_->setEnabled(m);
  del_->setEnabled(m);
  presets_->setEnabled(m);
  name_->setEnabled(m);
  assign_->setEnabled(m && !doc_->selection().isEmpty());
  pick_->setEnabled(doc_->firstSelected() && doc_->firstSelected()->materialId);
  {
    QSignalBlocker b(name_);
    name_->setText(m ? QString::fromStdString(m->name) : QString());
  }
  if (m != formMaterial_) {
    formMaterial_ = m;
    QString file = doc_->filePath();
    form_->browseDir = file.isEmpty() ? QString() : QFileInfo(file).absolutePath();
    form_->setBlock(m ? &m->params : nullptr);
  } else {
    form_->refresh();
  }
  bigPreview_->setPixmap(QPixmap::fromImage(m ? images_.value(m->id, placeholder(kPreviewSize))
                                              : placeholder(kPreviewSize))
                             .scaled(kPreviewSize, kPreviewSize));
  refreshUsage();
}

void MaterialEditor::refreshUsage() {
  Material* m = current();
  if (!m) {
    usage_->setText(doc_->scene().materials.empty() ? tr("No materials yet. Click New to create one.") : QString());
    return;
  }
  QStringList users;
  for (const auto& n : doc_->scene().nodes)
    if (n->materialId == m->id) users << QString::fromStdString(n->name);
  usage_->setText(users.isEmpty() ? tr("Not assigned to any object.")
                                  : tr("Used by: %1").arg(users.mid(0, 12).join(", ") +
                                                          (users.size() > 12 ? QString(", ...") : QString())));
}

void MaterialEditor::requestPreview(uint32_t id) {
  const Material* m = doc_->scene().findMaterial(id);
  if (!m) return;
  Material copy = *m;
  // Resolve texture paths now so the worker never touches the scene.
  for (const auto& d : copy.params.defs())
    if (d.type == ParamType::File) copy.params.setString(d.key, doc_->scene().resolvePath(copy.params.getString(d.key)));
  previews_.request(id, copy, kPreviewSize, kPreviewSamples);
}

void MaterialEditor::applyPreset(int index) {
  Material* m = current();
  if (!m) return;
  doc_->beginEdit(tr("Material Preset"));
  ParamBlock fresh = Material().params;  // start from defaults, keep maps
  for (const auto& d : fresh.defs()) {
    if (d.type == ParamType::File) continue;
    if (d.type == ParamType::Color) m->params.setColor(d.key, fresh.getColor(d.key));
    else if (d.type == ParamType::String) m->params.setString(d.key, fresh.getString(d.key));
    else m->params.setNumber(d.key, fresh.getFloat(d.key));
  }
  ParamBlock& p = m->params;
  auto glossy = [&](const Vec3& refl, float gloss) {
    p.setColor("reflect", refl);
    p.setNumber("reflectGlossiness", gloss);
  };
  switch (index) {
    case 1: p.setColor("diffuse", Vec3(0.6f)); break;
    case 2: p.setColor("diffuse", Vec3(0.55f, 0.08f, 0.06f)); glossy(Vec3(1.0f), 0.88f); p.setNumber("fresnelIor", 1.5); break;
    case 3:
      p.setColor("diffuse", Vec3(0.0f)); glossy(Vec3(1.0f), 1.0f); p.setColor("refract", Vec3(1.0f));
      p.setNumber("ior", 1.52); p.setNumber("fresnelIor", 1.52);
      break;
    case 4:
      p.setColor("diffuse", Vec3(0.0f)); glossy(Vec3(1.0f), 0.8f); p.setColor("refract", Vec3(0.95f));
      p.setNumber("refractGlossiness", 0.75); p.setNumber("ior", 1.52);
      break;
    case 5: p.setColor("diffuse", Vec3(0.95f)); glossy(Vec3(1.0f), 0.98f); p.setNumber("metalness", 1.0); break;
    case 6: p.setColor("diffuse", Vec3(0.91f, 0.92f, 0.92f)); glossy(Vec3(1.0f), 0.72f); p.setNumber("metalness", 1.0); break;
    case 7: p.setColor("diffuse", Vec3(1.0f, 0.77f, 0.34f)); glossy(Vec3(1.0f), 0.9f); p.setNumber("metalness", 1.0); break;
    case 8: p.setColor("diffuse", Vec3(0.95f, 0.64f, 0.54f)); glossy(Vec3(1.0f), 0.85f); p.setNumber("metalness", 1.0); break;
    case 9: p.setColor("diffuse", Vec3(0.04f)); glossy(Vec3(0.5f), 0.45f); break;
    case 10: p.setColor("diffuse", Vec3(0.85f, 0.85f, 0.82f)); glossy(Vec3(1.0f), 0.95f); p.setNumber("fresnelIor", 1.55); break;
    case 11:
      p.setColor("diffuse", Vec3(0.0f)); glossy(Vec3(1.0f), 1.0f); p.setColor("refract", Vec3(1.0f));
      p.setNumber("ior", 1.333); p.setNumber("fresnelIor", 1.333); p.setColor("fogColor", Vec3(0.75f, 0.92f, 0.95f));
      p.setNumber("fogMultiplier", 0.05);
      break;
    case 12: p.setColor("diffuse", Vec3(0.0f)); p.setColor("selfIllum", Vec3(1.0f, 0.95f, 0.85f)); p.setNumber("selfIllumMultiplier", 5.0); break;
    default: break;
  }
  doc_->notifyMaterialsChanged();
  doc_->endEdit();
  form_->refresh();
  requestPreview(m->id);
}

}  // namespace veraa
